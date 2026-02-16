#include "AdaptivePackedCellContainer.hpp"

namespace AtomicCScompact
{

    struct AdaptivePackedCellContainer::QSBRGuard
    {
        bool IsQSBRGuardActive;
        AdaptivePackedCellContainer* ParentContainer;
        

        QSBRGuard(AdaptivePackedCellContainer* apc_ptr = nullptr) noexcept :
            ParentContainer(apc_ptr), IsQSBRGuardActive(false)
        {
            if (ParentContainer)
            {
                ParentContainer ->QSBREnterCritical_();
                IsQSBRGuardActive = true;
            }
            
        }

        ~QSBRGuard() noexcept 
        {
            if (IsQSBRGuardActive)
            {
                ParentContainer->QSBRExitCritical_();
            }
        }
        QSBRGuard(const QSBRGuard&) = delete;
        QSBRGuard& operator = (const QSBRGuard&) = delete;
        QSBRGuard(QSBRGuard&& oprtr) noexcept :
            ParentContainer(oprtr.ParentContainer), IsQSBRGuardActive(oprtr.IsQSBRGuardActive)
        {
            oprtr.IsQSBRGuardActive = false;//1
            oprtr.ParentContainer = nullptr;//2
        }
    };
    
    struct AdaptivePackedCellContainer::RelEntryGuard
    {
        QSBRGuard QSBRGuardRE;
        RelEntry_* RelEntryPtr;
        RelEntryGuard() noexcept :
            QSBRGuardRE(nullptr), RelEntryPtr(nullptr)
        {}
        RelEntryGuard(RelEntry_* relentry_ptr, QSBRGuard&& address_of_qsbrguard_address) noexcept :
            QSBRGuardRE(std::move(address_of_qsbrguard_address)), RelEntryPtr(relentry_ptr)
        {}
        ~RelEntryGuard() = default;
        RelEntryGuard(const RelEntryGuard&) = delete;
        RelEntryGuard& operator = (const RelEntryGuard&) = delete;
        RelEntryGuard(RelEntryGuard&& oprtr) noexcept :
            QSBRGuardRE(std::move(oprtr.QSBRGuardRE)), RelEntryPtr(oprtr.RelEntryPtr)
        {
            oprtr.RelEntryPtr = nullptr;
        }
    };
    
    struct AdaptivePackedCellContainer::RelEntry_
    {
        enum class APCKind : uint8_t
        {
            CHILD_CONTAINER = 0,
            PACKED_NODE = 1,
            HEAP_NODE = 2
        };

        APCKind Kind;
        AdaptivePackedCellContainer* ChildContainerPtr;
        size_t ChildBaseIdx;
        packed64_t RelEntryPacked;

        void* HeapPtr;
        size_t HeapSize;

        PackedCellDataType RECellDType;

        FinalizerKind_ KindFinalizer;
        std::function<void(RelEntry_*)> FinalizerPtr;
        DeviceFence_ APCDeviceFence;
        std::atomic<uint64_t> RetireEpoch;
        std::atomic<RelEntry_*> NextPtr;

        //child container constructor
        RelEntry_(AdaptivePackedCellContainer* apc_container = nullptr, size_t base = 0) noexcept :
            Kind(APCKind::CHILD_CONTAINER), ChildContainerPtr(apc_container), ChildBaseIdx(base), RelEntryPacked(0),
            HeapPtr(nullptr), HeapSize(0), RECellDType(PackedCellDataType::UnsignedPCellDataType),
            KindFinalizer(FinalizerKind_::NONE), FinalizerPtr(nullptr), APCDeviceFence{}, RetireEpoch(0), NextPtr(nullptr)
        {}
        //packed cell constructor
        RelEntry_(packed64_t p) noexcept :
            Kind(APCKind::PACKED_NODE), ChildContainerPtr(nullptr), ChildBaseIdx(0), RelEntryPacked(p),
            HeapPtr(0), RECellDType(PackedCellDataType::UnsignedPCellDataType),
            KindFinalizer(FinalizerKind_::NONE), FinalizerPtr(nullptr), APCDeviceFence{}, RetireEpoch(0), NextPtr(nullptr)
        {}
        //heap constructor
        RelEntry_(void* heap_ptr, size_t heap_size, PackedCellDataType pc_dtype) noexcept :
            Kind(APCKind::HEAP_NODE), ChildContainerPtr(nullptr), ChildBaseIdx(0), RelEntryPacked(0),
            HeapPtr(heap_ptr), HeapSize(heap_size), RECellDType(pc_dtype),
            KindFinalizer(FinalizerKind_::HOST), FinalizerPtr(nullptr), APCDeviceFence{}, RetireEpoch(0), NextPtr(nullptr)
        {}
    };

    uint64_t AdaptivePackedCellContainer::ComputeMinThreadEpoch() const noexcept
    {
        uint64_t min_epoch = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < ThreadEpochs_.size(); i++)
        {
            uint64_t val = ThreadEpochs_[i].load(MoLoad_);
            if (val == std::numeric_limits<uint64_t>::max())
            {
                continue;
            }
            if (val < min_epoch)
            {
                min_epoch = val;
            }
        }
        return min_epoch;
    }
    
    size_t AdaptivePackedCellContainer::RegisterThreadForQSBRImplementation_() noexcept
    {
        if (QSBRThreadIdx_ != SIZE_MAX)
        {
            return QSBRThreadIdx_;
        }
        uint64_t sentinal = std::numeric_limits<uint64_t>::max();
        uint64_t cur_epoch = GlobalEpoch_.load(MoLoad_);
        for (size_t i = 0; i < ThreadEpochs_.size(); i++)
        {
            uint64_t val = ThreadEpochs_[i].load(std::memory_order_relaxed);
            if (val == sentinal)
            {
                if (ThreadEpochs_[i].compare_exchange_strong(val, cur_epoch, EXsuccess_, EXfailure_))
                {
                    QSBRThreadIdx_ = i;
                    return i;
                }
                else
                {
                    TotalCasFailure_.fetch_add(1, MoStoreUnSeq_);
                }
            }
        }
        return SIZE_MAX;
    }

    void AdaptivePackedCellContainer::RetirePushLocked_(RelEntry_* rel_entry_ptr) noexcept
    {
        RelEntry_* head = RetireHead_.load(MoLoad_);
        while (true)
        {
            rel_entry_ptr->NextPtr.store(head, MoStoreUnSeq_);
            if (RetireHead_.compare_exchange_strong(head, rel_entry_ptr, EXsuccess_, std::memory_order_acquire))
            {
                size_t cur = RetireCount_.fetch_add(1, std::memory_order_acq_rel) + 1;
                size_t prev_max = RetireQueDepthMax_.load(std::memory_order_relaxed);
                while (cur > prev_max && RetireQueDepthMax_.compare_exchange_weak(prev_max, cur, std::memory_order_relaxed))
                {
                    /* code */
                }
                TotalRetired_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            else
            {
                TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    bool AdaptivePackedCellContainer::DeviceFenceSatisfied_(const RelEntry_& rel_entry_address) noexcept
    {
        if (!rel_entry_address.APCDeviceFence.HandleDeviceFencePtr)
        {
            return true;
        }
        if (!rel_entry_address.APCDeviceFence.IsSignaled)
        {
            return false;
        }
        bool signaled = false;
        try
        {
            signaled = rel_entry_address.APCDeviceFence.IsSignaled(rel_entry_address.APCDeviceFence.HandleDeviceFencePtr);
        }
        catch(...)
        {
            signaled = false;
        }
        
        return signaled;
    }

    void AdaptivePackedCellContainer::TryReclaimRetired_() noexcept
    {
        size_t retire_count = RetireCount_.load(MoLoad_);
        if (retire_count == 0 || retire_count < RetireBatchThreshold_)
        {
            return;
        }
        RelEntry_* stolen = RetireHead_.exchange(nullptr, std::memory_order_acq_rel);
        if (!stolen)
        {
            RetireCount_.store(0, MoStoreSeq_);
            return;
        }
        RetireCount_.store(0, MoStoreSeq_);
        uint64_t min_epoch = ComputeMinThreadEpoch();
        RelEntry_* cur_relentry = stolen;
        RelEntry_* keep_head = nullptr;
        RelEntry_* keep_tail = nullptr;
        size_t track_count = 0;

        while (cur_relentry)
        {
            RelEntry_* next_relentry = cur_relentry->NextPtr.load(std::memory_order_relaxed);
            uint64_t cur_retire_epoch = cur_relentry->RetireEpoch.load(MoLoad_);
            bool  can_reclaim = false;
            if (cur_retire_epoch == 0)
            {
                uint64_t now_epoch = GlobalEpoch_.load(MoLoad_);
                cur_relentry->RetireEpoch.store(now_epoch, MoStoreSeq_);
                can_reclaim = false;
            }
            else
            {
                if (min_epoch != std::numeric_limits<uint64_t>::max())
                {
                    if (DeviceFenceSatisfied_(*cur_relentry))
                    {
                        can_reclaim = true;
                    }
                    else
                    {
                        can_reclaim = false;
                    }
                }
                else
                {
                    can_reclaim = false;
                }
            }
            if (can_reclaim)
            {
                if (cur_relentry->FinalizerPtr)
                {
                    try 
                    {
                        cur_relentry->FinalizerPtr(cur_relentry);
                    }
                    catch (...)
                    {
                        if (APCLogger_)
                        {
                            APCLogger_("TryReclaimRetired_()", "RelEntry finalizer threw an exception::cur_relentry->FinalizerPtr");
                        }
                    }
                }

                if (cur_relentry->Kind == RelEntry_::APCKind::HEAP_NODE && cur_relentry->HeapPtr)
                {
                    ::operator delete(cur_relentry->HeapPtr, std::align_val_t{alignof(std::max_align_t)});
                    TotalReclaimedBytes_.fetch_add(cur_relentry->HeapSize, std::memory_order_relaxed);
                    cur_relentry->HeapPtr = nullptr;
                }
                TotalReclaimed_.fetch_add(1, std::memory_order_relaxed);
                delete cur_relentry;
            }
            else
            {
                cur_relentry->NextPtr.store(nullptr, MoStoreUnSeq_);
                if (!keep_head)
                {
                    keep_head = cur_relentry;
                    keep_tail = cur_relentry;
                }
                else
                {
                    keep_tail->NextPtr.store(cur_relentry, MoStoreUnSeq_);
                    keep_tail = cur_relentry;
                }
                ++track_count;
            }
            cur_relentry = next_relentry;
        }
        if (!keep_head)
        {
            return;
        }

        RelEntry_* head_relentry = RetireHead_.load(MoLoad_);
        while (true)
        {
            keep_tail->NextPtr.store(head_relentry, MoStoreUnSeq_);
            if (RetireHead_.compare_exchange_strong(head_relentry, keep_head, EXsuccess_, std::memory_order_acquire))
            {
                RetireCount_.fetch_add(1, std::memory_order_acq_rel);
                break;
            }
            else
            {
                TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    bool AdaptivePackedCellContainer::PollDeviceFencesOnce_() noexcept
    {
        bool any_signaled = false;
        for (size_t i = 0; i < RelOffset_.size(); i++)
        {
            std::uintptr_t raw_reloffset_ptr = RelOffset_[i].load(MoLoad_);
            if (!raw_reloffset_ptr)
            {
                continue;
            }
            RelEntry_* relentry_ptr = reinterpret_cast<RelEntry_*>(raw_reloffset_ptr);
            if (relentry_ptr->APCDeviceFence.HandleDeviceFencePtr && relentry_ptr->APCDeviceFence.HandleDeviceFencePtr)
            {
                bool signal = false;
                try 
                {
                    signal = relentry_ptr->APCDeviceFence.IsSignaled(relentry_ptr->APCDeviceFence.HandleDeviceFencePtr);
                }
                catch (...)
                {
                    signal = false;
                }
                if (signal)
                {
                    any_signaled = true;
                }
            }
        }
        return any_signaled;
    }

    void AdaptivePackedCellContainer::BackgroundReclaimerMainThread_() noexcept
    {
        unsigned interval_ms = std::max<unsigned>(1u, APCContainerCfg_.BackgroundEpochAdvanceMS);
        std::unique_lock<std::mutex>lk(BackgroundMutex_);
        while (!BackgroundThreadStop_)
        {
            BackgroundCondVar_.wait_for(lk, std::chrono::milliseconds(interval_ms), [this] {
                return BackgroundThreadStop_;
            });

            if (BackgroundThreadStop_)
            {
                break;
            }
            GlobalEpoch_.fetch_add(1, std::memory_order_acq_rel);
            PollDeviceFencesOnce_();
            TryReclaimRetired_();
        }
    }




    
}
