#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    class PackedCellContainerManager;
    
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
        std::function<void(void*)> FinalizerPtr;
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
        if (!ThreadEpochArray_)
        {
            return std::numeric_limits<uint64_t>::max();
        }
        uint64_t min_epoch = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < ThreadEpochCapacity_; i++)
        {
            uint64_t val = ThreadEpochArray_[i].load(MoLoad_);
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
        if (!ThreadEpochArray_)
        {
            return SIZE_MAX;
        }
        uint64_t sentinal = std::numeric_limits<uint64_t>::max();
        uint64_t cur_epoch = GlobalEpoch_.load(MoLoad_);
        for (size_t i = 0; i < ThreadEpochCapacity_; i++)
        {
            uint64_t val = ThreadEpochArray_[i].load(std::memory_order_relaxed);
            if (val == sentinal)
            {
                if (ThreadEpochArray_[i].compare_exchange_strong(val, cur_epoch, EXsuccess_, EXfailure_))
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
                if (min_epoch != std::numeric_limits<uint64_t>::max() && cur_retire_epoch < min_epoch)
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
                else if (min_epoch == std::numeric_limits<uint64_t>::max())
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
                if (cur_relentry->KindFinalizer != FinalizerKind_::NONE && cur_relentry->Kind == RelEntry_::APCKind::HEAP_NODE)
                {
                    if (cur_relentry->FinalizerPtr)
                    {
                        try
                        {
                            cur_relentry->FinalizerPtr(cur_relentry->HeapPtr);
                        }
                        catch (...)
                        {
                            if (APCLogger_)
                            {
                                APCLogger_("TryReclaimRetired_()", "RelEntry finalizer threw an exception::cur_relentry->FinalizerPtr");
                            }
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
                RetireCount_.fetch_add(track_count, std::memory_order_acq_rel);
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
        RelEntry_* cur_relentry_ptr = RetireHead_.load(MoLoad_);
        while (cur_relentry_ptr)
        {
            if (cur_relentry_ptr->APCDeviceFence.HandleDeviceFencePtr && cur_relentry_ptr->APCDeviceFence.IsSignaled)
            {
                bool signaled_now = false;
                try
                {
                    signaled_now = cur_relentry_ptr->APCDeviceFence.IsSignaled(cur_relentry_ptr->APCDeviceFence.HandleDeviceFencePtr);
                }
                catch (...)
                {
                    signaled_now = false;
                }
                if (signaled_now)
                {
                    any_signaled = true;
                }
            }
            cur_relentry_ptr = cur_relentry_ptr->NextPtr.load(MoLoad_);
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

    PublishResult AdaptivePackedCellContainer::PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask_with_ptrflag, int max_probs) noexcept
    {
        if (!IfAnyValid_())
        {
            return { PublishStatus::INVALID, SIZE_MAX};
        }
        uint64_t full_ptrval = reinterpret_cast<uint64_t>(object_ptr);
        uint32_t low32_half = static_cast<uint32_t>(full_ptrval & MaskBits(VALBITS));
        uint32_t high32_half = static_cast<uint32_t>((full_ptrval >> VALBITS) & MaskBits(VALBITS));
        size_t next_sequence = NextProducerSequence();
        if (next_sequence == SIZE_MAX)
        {
            return {PublishStatus::INVALID, SIZE_MAX};
        }
        
        size_t start = next_sequence % ContainerCapacity_;
        size_t step = GetHashedRendomizedStep_(next_sequence);
        int probes = 0;
        size_t idx = start;
        while (true)
        {
            size_t head = idx;
            size_t tail = (head + 1) % ContainerCapacity_;
            packed64_t cur_head = BackingPtr[head].load(MoLoad_);
            packed64_t cur_tail = BackingPtr[tail].load(MoLoad_);
            tag8_t head_locality = PackedCell64_t::ExtractLocalityFromPacked(cur_head);
            tag8_t tail_locality = PackedCell64_t::ExtractLocalityFromPacked(cur_tail);
            if (head_locality == ST_IDLE && tail_locality == ST_IDLE)
            {
                packed64_t claimed_cur_head = PackedCell64_t::SetLocalityInPacked(cur_head, ST_CLAIMED);
                packed64_t claimed_cur_tail = PackedCell64_t::SetLocalityInPacked(cur_tail, ST_CLAIMED);
                packed64_t expected_head = cur_head;
                if (!BackingPtr[head].compare_exchange_strong(expected_head, claimed_cur_head, EXsuccess_, EXfailure_))
                {
                    TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    packed64_t expected_tail = cur_tail;
                    if (!BackingPtr[tail].compare_exchange_strong(expected_tail, claimed_cur_tail, EXsuccess_, EXfailure_))
                    {
                        BackingPtr[head].store(cur_head, MoStoreSeq_);
                        BackingPtr[head].notify_all();
                        TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        val32_t tail_ptr_val32 = high32_half;
                        strl16_t strl_tail = MakeSTRL4_t(ZERO_PRIORITY, ST_PUBLISHED, rel_mask_with_ptrflag, RELOFFSET_TAIL_PTR, static_cast<unsigned>(PackedMode::MODE_VALUE32));
                        packed64_t tail_packed = PackedCell64_t::ComposeValue32u_64(tail_ptr_val32, 0u, strl_tail);
                        BackingPtr[tail].store(tail_packed, MoStoreSeq_);

                        val32_t head_ptr_value32 = low32_half;
                        strl16_t strl_head = MakeSTRL4_t(DEFAULT_PAIRED_HEAD_HALF_PRIORITY, ST_PUBLISHED, rel_mask_with_ptrflag, REL_OFFSET_HEAD_PTR, static_cast<unsigned>(PackedMode::MODE_VALUE32));
                        packed64_t head_packed = PackedCell64_t::ComposeValue32u_64(head_ptr_value32, 0u, strl_head);
                        BackingPtr[head].store(head_packed, MoStoreSeq_);
                        BackingPtr[tail].notify_all();
                        BackingPtr[head].notify_all();
                        Occupancy_.fetch_add(1, std::memory_order_acq_rel);
                        return {PublishStatus::OK, head};
                    }
                }
            }
            ++probes;
            if ((max_probs >=0 && probes >= max_probs) || probes >= static_cast<int>(ContainerCapacity_))
            {
                return {PublishStatus::FULL, SIZE_MAX};
            }
            idx = (idx + step) % ContainerCapacity_;
        }
    }

    std::optional<uint64_t>AdaptivePackedCellContainer::TryAssemblePairedPtr_(size_t probable_idx, RelOffsetMode& ptr_position_for_easy_return) const noexcept
    {
        if (!IfIdxValid_(probable_idx))
        {
            return std::nullopt;
        }
        packed64_t cell_data = BackingPtr[probable_idx].load(MoLoad_);
        tag8_t head_tail_or_null = PackedCell64_t::ExtractRelOffsetFromPacked(cell_data);
        if ((static_cast<RelOffsetMode>(head_tail_or_null) == RelOffsetMode::REL_OFFSET_HEAD_PTR))
        {
            tag8_t head_locality = PackedCell64_t::ExtractLocalityFromPacked(cell_data);
            PackedMode head_celltype = static_cast<PackedMode>(PackedCell64_t::ExtractPCellTypeFromPacked(cell_data));
            if ((head_locality != ST_PUBLISHED && head_locality != ST_CLAIMED) || head_celltype != PackedMode::MODE_VALUE32)
            {
                return std::nullopt;
            }
            size_t tail_idx = (probable_idx + 1) % ContainerCapacity_;
            packed64_t tail_cell_data = BackingPtr[tail_idx].load(MoLoad_);
            tag8_t tail_locality = PackedCell64_t::ExtractLocalityFromPacked(tail_cell_data);
            PackedMode tell_celltype = static_cast<PackedMode>(PackedCell64_t::ExtractPCellTypeFromPacked(tail_cell_data));
            if ((tail_locality != ST_PUBLISHED && tail_locality != ST_CLAIMED) || tell_celltype != PackedMode::MODE_VALUE32)
            {
                return std::nullopt;
            }
            val32_t head_val32 = PackedCell64_t::ExtractValue32(cell_data);
            val32_t tail_val32 = PackedCell64_t::ExtractValue32(tail_cell_data);
            uint64_t assembeled = (static_cast<uint64_t>(tail_val32) << VALBITS) | (static_cast<uint64_t>(head_val32));
            ptr_position_for_easy_return = RelOffsetMode::REL_OFFSET_HEAD_PTR;
            return assembeled;
            
        }
        else if(static_cast<RelOffsetMode>(head_tail_or_null) == RelOffsetMode::RELOFFSET_TAIL_PTR)
        {
            tag8_t tail_locality = PackedCell64_t::ExtractLocalityFromPacked(cell_data);
            PackedMode tail_cell_type = static_cast<PackedMode>(PackedCell64_t::ExtractPCellTypeFromPacked(cell_data));
            if ((tail_locality != ST_PUBLISHED && tail_locality != ST_CLAIMED) || tail_cell_type != PackedMode::MODE_VALUE32)
            {
                return std::nullopt;
            }
            size_t head_idx = (probable_idx + ContainerCapacity_ - 1) % ContainerCapacity_;
            packed64_t head_cell_data = BackingPtr[head_idx].load(MoLoad_);
            tag8_t head_locality = PackedCell64_t::ExtractLocalityFromPacked(head_cell_data);
            PackedMode head_cell_type = static_cast<PackedMode>(PackedCell64_t::ExtractPCellTypeFromPacked(head_cell_data));
            if ((head_locality != ST_PUBLISHED && head_locality != ST_CLAIMED)|| head_cell_type != PackedMode::MODE_VALUE32)
            {
                return std::nullopt;
            }
            val32_t tail_val32 = PackedCell64_t::ExtractValue32(cell_data);
            val32_t head_val32 = PackedCell64_t::ExtractValue32(head_cell_data);
            uint64_t assembeled = (static_cast<uint64_t>(tail_val32) << VALBITS) | (static_cast<uint64_t>(head_val32));
            ptr_position_for_easy_return = RelOffsetMode::RELOFFSET_TAIL_PTR;
            return assembeled;
        }
        else
        {
            return std::nullopt;
        }
    }

    std::optional<uint64_t> AdaptivePackedCellContainer::GetAssembledPtrWithTriCASReset(size_t probable_idx, RelOffsetMode& ptr_position_for_easy_return, 
                                            std::optional<bool>ownership_cas, std::optional<tag8_t>third_cas) noexcept
    {
        auto maybe_first = TryAssemblePairedPtr_(probable_idx, ptr_position_for_easy_return);
        size_t head_idx = 0, tail_idx = 0;
        if (!maybe_first)
        {
            return std::nullopt;
        }
        if (!ownership_cas)
        {
            return maybe_first;
        }
        if (ptr_position_for_easy_return == RelOffsetMode::REL_OFFSET_HEAD_PTR)
        {
            head_idx = probable_idx;
            tail_idx = (probable_idx + 1) % ContainerCapacity_;
        }
        else
        {
            head_idx = (probable_idx + ContainerCapacity_ - 1) % ContainerCapacity_;
            tail_idx = probable_idx;
        }
        packed64_t oring_head = BackingPtr[head_idx].load(MoLoad_);
        packed64_t oring_tail = BackingPtr[tail_idx].load(MoLoad_);

        packed64_t claimed_head = PackedCell64_t::SetLocalityInPacked(oring_head, ST_CLAIMED);
        packed64_t claimed_tail = PackedCell64_t::SetLocalityInPacked(oring_tail, ST_CLAIMED);

        const int MAX_CLAIM_ATTEMPTS  = std::max<int>(BURNCYCLE_THRESHOLD, (1000 / 8)); // 8 represents number of threads
        int attempt = 0;
        bool head_claimed = false;
        bool tail_claimed = false;

        for (; attempt < MAX_CLAIM_ATTEMPTS; attempt++)
        {
            packed64_t expected_head = oring_head;
            if (!BackingPtr[head_idx].compare_exchange_strong(expected_head, claimed_head, EXsuccess_, EXfailure_))
            {
                oring_head = expected_head;
                auto maybe_retry = TryAssemblePairedPtr_(probable_idx, ptr_position_for_easy_return);
                if (!maybe_retry)
                {
                    return std::nullopt;
                }

                if (ptr_position_for_easy_return == RelOffsetMode::REL_OFFSET_HEAD_PTR)
                {
                    head_idx = probable_idx;
                    tail_idx = (probable_idx + 1) % ContainerCapacity_;
                }
                else
                {
                    head_idx = (probable_idx + ContainerCapacity_ - 1) % ContainerCapacity_;
                    tail_idx = probable_idx;
                }
                
                oring_head = BackingPtr[head_idx].load(MoLoad_);
                oring_tail = BackingPtr[tail_idx].load(MoLoad_);
                claimed_head = PackedCell64_t::SetLocalityInPacked(oring_head, ST_CLAIMED);
                claimed_tail = PackedCell64_t::SetLocalityInPacked(oring_tail, ST_CLAIMED);
                continue;
            }
            head_claimed = true;
            packed64_t expected_tail = oring_tail;
            if (!BackingPtr[tail_idx].compare_exchange_strong(expected_tail, claimed_tail, EXsuccess_, EXfailure_))
            {
                BackingPtr[head_idx].compare_exchange_strong(claimed_head, oring_head, EXsuccess_, EXfailure_);
                BackingPtr[head_idx].notify_all();
                head_claimed = false;

                auto maybe_retry_2 = TryAssemblePairedPtr_(probable_idx, ptr_position_for_easy_return);
                if (!maybe_retry_2)
                {
                    return std::nullopt;
                }
            oring_head = BackingPtr[head_idx].load(MoLoad_);
            oring_tail = BackingPtr[tail_idx].load(MoLoad_);
            claimed_head = PackedCell64_t::SetLocalityInPacked(oring_head, ST_CLAIMED);
            claimed_tail = PackedCell64_t::SetLocalityInPacked(oring_tail, ST_CLAIMED);
                if (APCManagerPtr_)
                {
                    APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(oring_tail);
                }
                continue;
            }
            tail_claimed = true;
            break;
        }
        if (!head_claimed || !tail_claimed)
        {
            return std::nullopt;
        }
        
        auto maybe_second = TryAssemblePairedPtr_(probable_idx, ptr_position_for_easy_return);
        if (!maybe_second)
        {
            BackingPtr[head_idx].compare_exchange_strong(claimed_head, oring_head, EXsuccess_, EXfailure_);
            BackingPtr[tail_idx].compare_exchange_strong(claimed_tail, oring_tail, EXsuccess_, EXfailure_);
            BackingPtr[head_idx].notify_all();
            BackingPtr[tail_idx].notify_all();
            return std::nullopt;
        }
        if (!third_cas)
        {
            return maybe_second;
        }

        //t7his posion will be updated using enum class for best beheviour
        tag8_t desired_locality = *third_cas;
        packed64_t recycled_head = PackedCell64_t::SetLocalityInPacked(claimed_head, desired_locality);
        packed64_t recycled_tail = PackedCell64_t::SetLocalityInPacked(claimed_tail, desired_locality);

        bool head_recycled = BackingPtr[head_idx].compare_exchange_strong(claimed_head, recycled_head, EXsuccess_, EXfailure_);
        bool tail_recycled = BackingPtr[tail_idx].compare_exchange_strong(claimed_tail, recycled_tail, EXsuccess_, EXfailure_);

        if (!head_recycled || !tail_recycled) {
            if (head_recycled) {
                BackingPtr[head_idx].compare_exchange_strong(recycled_head, oring_head, EXsuccess_, EXfailure_);
            }
            if (tail_recycled) {
                BackingPtr[tail_idx].compare_exchange_strong(recycled_tail, oring_tail, EXsuccess_, EXfailure_);
            }
            BackingPtr[head_idx].notify_all();
            BackingPtr[tail_idx].notify_all();
            return std::nullopt; 
        }
        
        BackingPtr[head_idx].notify_all();
        BackingPtr[tail_idx].notify_all();
        return maybe_second;
    }


    void AdaptivePackedCellContainer::RetirePairedPtrAtIdx_(
        size_t probable_idx,
        DeviceFence_ fence
    ) noexcept
    {
        RelOffsetMode ptr_position_probableidx = RelOffsetMode::RELOFFSET_GENERIC_VALUE;
        auto maybe_ptr = TryAssemblePairedPtr_(probable_idx, ptr_position_probableidx);
        void* obj_ptr = nullptr;
        if (maybe_ptr)
        {
            obj_ptr = reinterpret_cast<void*>(*maybe_ptr);
        }
        packed64_t idle_cell32 = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
        if (ptr_position_probableidx == RelOffsetMode::RELOFFSET_GENERIC_VALUE)
        {
            return;
        }
        else if (ptr_position_probableidx == RelOffsetMode::REL_OFFSET_HEAD_PTR)
        {
            size_t tail_idx = (probable_idx + 1) % ContainerCapacity_;
            BackingPtr[probable_idx].store(idle_cell32, MoStoreSeq_);
            BackingPtr[tail_idx].store(idle_cell32, MoStoreSeq_);
            BackingPtr[probable_idx].notify_all();
            BackingPtr[tail_idx].notify_all();
        }
        else if (ptr_position_probableidx == RelOffsetMode::RELOFFSET_TAIL_PTR)
        {
            size_t head_idx = (probable_idx + ContainerCapacity_ - 1) % ContainerCapacity_;
            BackingPtr[probable_idx].store(idle_cell32, MoStoreSeq_);
            BackingPtr[head_idx].store(idle_cell32, MoStoreSeq_);
            BackingPtr[head_idx].notify_all();
            BackingPtr[probable_idx].notify_all();
        }
        Occupancy_.fetch_sub(1, std::memory_order_acq_rel);
        if (obj_ptr)
        {
            packed64_t packed_val = static_cast<packed64_t>(reinterpret_cast<uint64_t>(obj_ptr));
            RelEntry_* rel_entry = new RelEntry_(packed_val);
            rel_entry->KindFinalizer = FinalizerKind_::NONE;
            rel_entry->APCDeviceFence = std::move(fence);
            rel_entry->APCDeviceFence = std::move(fence);
            uint64_t cur_epoch = GlobalEpoch_.load(MoLoad_);
            rel_entry->RetireEpoch.store(cur_epoch, MoStoreSeq_);
            RetirePushLocked_(rel_entry);
            TryReclaimRetired_();
        }
    }

    void AdaptivePackedCellContainer::UpdateRegionRelForIdx_(size_t idx, tag8_t rel_mask) noexcept
    {
        if (RegionSize_ == 0)
        {
            return;
        }
        size_t region = idx / RegionSize_;
        RegionRelArray_[region].fetch_or(rel_mask, std::memory_order_acq_rel);
        size_t w = region / MAX_VAL;
        size_t b =  region % MAX_VAL;
        uint64_t region_mask = (1ull << b);
        for (unsigned i = 0; i < LN_OF_BYTE_IN_BITS; i++)
        {
            if (rel_mask & (1u << i))
            {
                std::atomic_ref<uint64_t>aref(RelBitmaps_[i][w]);
                aref.fetch_or(region_mask, std::memory_order_acq_rel);
            }
        }
    }

    void AdaptivePackedCellContainer::StartBackgroundReclaimerIfNeed()
    {
        if (APCContainerCfg_.BackgroundEpochAdvanceMS == 0)
        {
            return;
        }
        std::lock_guard<std::mutex>LK(BackgroundMutex_);
        if (BackgroundThread_.joinable())
        {
            return;
        }
        BackgroundThreadStop_ = false;
        BackgroundThread_ = std::thread([this]
        {
            BackgroundReclaimerMainThread_();
        }
        );                
    }
    
    void AdaptivePackedCellContainer::StopBackgroundReclaimer() noexcept
    {
        {
            std::lock_guard<std::mutex>lk(BackgroundMutex_);
            BackgroundThreadStop_ = true;
            BackgroundCondVar_.notify_all();
        }
        if (BackgroundThread_.joinable())
        {
            BackgroundThread_.join();
        }
    }

    void AdaptivePackedCellContainer::InitOwned(size_t capacity, int node, ContainerConf container_cfg, size_t alignment)
    {
        (void)node, (void)alignment;
        FreeAll();
        if (capacity == 0)
        {
            throw std::invalid_argument("Capacity == 0");
        }
        BackingPtr = new std::atomic<packed64_t>[capacity];
        packed64_t idle_cell = PackedCell64_t::MakeInitialPacked(container_cfg.InitialMode);
        for (size_t i = 0; i < capacity; i++)
        {
            BackingPtr[i].store(idle_cell, MoStoreUnSeq_);
        }
        ContainerCapacity_ = capacity;
        IsContainerOwned_ = true;
        APCContainerCfg_ = container_cfg;
        RetireBatchThreshold_ = std::max<unsigned>(1, APCContainerCfg_.RetireBatchThreshold);
        size_t tls_size = std::min<size_t>(APCContainerCfg_.MaxTlsCandidates ? APCContainerCfg_.MaxTlsCandidates : APCContainerCfg_.MAXTLS, APCContainerCfg_.MAXTLS);
        ThreadEpochArray_.reset(
            new std::atomic<uint64_t>[tls_size]
        );
        ThreadEpochCapacity_ = tls_size;
        for (size_t i = 0; i < ThreadEpochCapacity_; i++)
        {
            ThreadEpochArray_[i].store(std::numeric_limits<uint64_t>::max(), MoStoreUnSeq_);
        }
        InitZeroState_();


        if (APCContainerCfg_.RegionSize)
        {
            InitRegionIdx(APCContainerCfg_.RegionSize);
        }
        try
        {
            PackedCellContainerManager::Instance().RegisterAdaptivePackedCellContainer(this);
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        
        // attach manager-provided master clock and adaptive backoff only after allocations succeed
        try {
            MasterClockConfPtr_ = &PackedCellContainerManager::Instance().GetMasterClockAdaptivePackedCellContainerManager();
            AdaptiveBackoffOfAPCPtr_ = &PackedCellContainerManager::Instance().GetManagersAdaptiveBackoff();
            if (AdaptiveBackoffOfAPCPtr_ && MasterClockConfPtr_) {
                AdaptiveBackoffOfAPCPtr_->AttachMasterClockToAadaptiveBackOff(MasterClockConfPtr_);
            }
        } catch (...) {
            // best-effort; do not throw for integration issues
            MasterClockConfPtr_ = nullptr;
            AdaptiveBackoffOfAPCPtr_ = nullptr;
            if (APCLogger_) APCLogger_("InitOwned", "Attach masterclock/backoff failed (non-fatal)");
        }
    }

    void AdaptivePackedCellContainer::InitZeroState_() noexcept
    {
        Occupancy_.store(0, MoStoreUnSeq_);
        ProducerCursor_.store(0, MoStoreUnSeq_);
        ConsumerCursor_.store(0, MoStoreUnSeq_);
        RetireHead_.store(nullptr, MoStoreSeq_);
        RetireCount_.store(0, MoStoreSeq_);
        GlobalEpoch_.store(1, MoStoreSeq_);
    }

    void AdaptivePackedCellContainer::FreeAll() noexcept
    {
        try 
        {
            PackedCellContainerManager::Instance().UnRegisterAdaptivePackedCellContainer(this);
        }
        catch (...)
        {

        }
        AdaptiveBackoffOfAPCPtr_ = nullptr;
        MasterClockConfPtr_ = nullptr;
        
        StopBackgroundReclaimer();
        if (BackingPtr)
        {
            if (IsContainerOwned_)
            {
                delete[] BackingPtr;
            }
            BackingPtr = nullptr;
        }
        RelEntry_* stolen = RetireHead_.exchange(nullptr, std::memory_order_acq_rel);
        while (stolen)
        {
            RelEntry_* next_ptr = stolen->NextPtr.load(std::memory_order_relaxed);
            if (stolen->Kind == RelEntry_::APCKind::HEAP_NODE && stolen->HeapPtr)
            {
                ::operator delete(stolen->HeapPtr, std::align_val_t{
                    alignof(std::max_align_t)
                });
            }
            if (stolen->FinalizerPtr)
            {
                try
                {
                    stolen->FinalizerPtr(stolen->HeapPtr);
                }
                catch (...)
                {
                    if (APCLogger_)
                    {
                        APCLogger_("FreeAll()", "Finalizer threw exception");
                    }
                }
            }
            delete stolen;
            stolen = next_ptr;
        }
        RetireCount_.store(0, MoStoreUnSeq_);
        ThreadEpochArray_.reset();
        ThreadEpochCapacity_ = 0;
        RegionRelArray_.reset();
        RelBitmaps_.clear();

        ContainerCapacity_ = 0;
        IsContainerOwned_ = false;
        RegionSize_ = 0;
        NumRegion_ = 0;
    }

    void AdaptivePackedCellContainer::InitRegionIdx(size_t region_size)
    {
        if (!IfAnyValid_())
        {
            throw std::runtime_error("Container not initialized");
        }
        if (region_size == 0)
        {
            throw std::invalid_argument("region size == 0");
        }
        RegionSize_ = region_size;
        NumRegion_ = ((ContainerCapacity_ + RegionSize_ - 1) / RegionSize_);
        RegionRelArray_.reset(
            new std::atomic<uint8_t>[NumRegion_]
        );
        RegionEpochArray_.reset(
            new std::atomic<uint64_t>[NumRegion_]
        );
        for (size_t region = 0; region < NumRegion_; region++)
        {
            RegionRelArray_[region].store(0, MoStoreSeq_);
            RegionEpochArray_[region].store(0, MoStoreSeq_);
        }
        size_t words = (NumRegion_ + MAX_VAL - 1) / MAX_VAL;
        RelBitmaps_.assign(LN_OF_BYTE_IN_BITS, std::vector<uint64_t>(words, 0ull));
        for (size_t region = 0; region < NumRegion_; region++)
        {
            size_t base = region * RegionSize_;
            size_t end = std::min(ContainerCapacity_, base + RegionSize_);
            tag8_t accum = 0;
            for (size_t i = base; i < end; i++)
            {
                accum |= PackedCell64_t::ExtractFullRelFromPacked(BackingPtr[i].load(MoLoad_));
            }
            RegionRelArray_[region].store(accum, MoStoreSeq_);
            if (accum)
            {
                size_t w = region / MAX_VAL;
                size_t b = region % MAX_VAL;
                uint64_t mask = (1ull << b);
                for (unsigned bit = 0; bit < LN_OF_BYTE_IN_BITS; bit++)
                {
                    if (accum & (1u << bit))
                    {
                        std::atomic_ref<uint64_t>aref(RelBitmaps_[bit][w]);
                        aref.fetch_or(mask, std::memory_order_acq_rel);
                    }
                }
            }
        }
    }

    size_t AdaptivePackedCellContainer::NextProducerSequence() noexcept
    {
        thread_local size_t block_base = 0;
        thread_local size_t block_left = 0;
        if (block_left == 0)
        {
            size_t block = std::min<size_t>(APCContainerCfg_.ProducerBlockSize, ContainerCapacity_);
            size_t base = ReserveProducerSlots(block);
            if (base == SIZE_MAX)
            {
                return SIZE_MAX;
            }
            block_base = base;
            block_left = block;
        }
        size_t seq = block_base++;
        --block_left;
        return seq;
    }

    void AdaptivePackedCellContainer::TryReclaimRetiredWithMinEpoch(uint64_t min_epoch) noexcept
    {
        size_t retire_count = RetireCount_.load(MoLoad_);
        if (retire_count == 0 || retire_count < RetireBatchThreshold_)
        {
            return;
        }
        RelEntry_* stolen = RetireHead_.exchange(nullptr, std::memory_order_acq_rel);
        if (!stolen)
        {
            RetireCount_.store(NO_VAL, MoStoreSeq_);
            return;
        }
        RetireCount_.store(NO_VAL, MoStoreSeq_);
        RelEntry_* current_relentry_ptr = stolen;
        RelEntry_* keep_head_ptr = nullptr;
        RelEntry_* keep_tail_ptr = nullptr;
        size_t track_count = 0;
        while (current_relentry_ptr)
        {
            RelEntry_* next_relentry_ptr = current_relentry_ptr->NextPtr.load(std::memory_order_relaxed);
            uint64_t cur_retire_epoch = current_relentry_ptr->RetireEpoch.load(MoLoad_);
            bool can_reclaim = false;
            if (cur_retire_epoch == 0)
            {
                uint64_t  now_epoch = GlobalEpoch_.load(MoLoad_);
                current_relentry_ptr->RetireEpoch.store(now_epoch, MoStoreSeq_);
                can_reclaim = false;
            }
            else
            {
                if ((min_epoch != std::numeric_limits<uint64_t>::max() && cur_retire_epoch < min_epoch) || 
                    (min_epoch == std::numeric_limits<uint64_t>::max())
                    )
                {
                    if (DeviceFenceSatisfied_(*current_relentry_ptr))
                    {
                        can_reclaim = true;
                    }   
                }
            }
            if (can_reclaim)
            {
                if ((current_relentry_ptr->KindFinalizer != FinalizerKind_::NONE) && (current_relentry_ptr->Kind == RelEntry_::APCKind::HEAP_NODE))
                {
                    if (current_relentry_ptr->FinalizerPtr)
                    {
                        try 
                        {
                            current_relentry_ptr->FinalizerPtr(current_relentry_ptr->HeapPtr);
                        }
                        catch (...)
                        {
                            if (APCLogger_)
                            {
                                APCLogger_("TryReclaimRetiredWithMinEpoch()", "RelEntry finalizer threw an exception");
                            }
                        }
                    }   
                }
                if (current_relentry_ptr->Kind == RelEntry_::APCKind::HEAP_NODE && current_relentry_ptr->HeapPtr)
                {
                     ::operator delete(current_relentry_ptr->HeapPtr, std::align_val_t{alignof(std::max_align_t)});
                     TotalReclaimedBytes_.fetch_add(current_relentry_ptr->HeapSize, std::memory_order_relaxed);
                     current_relentry_ptr->HeapPtr = nullptr;
                }
                TotalReclaimed_.fetch_add(1, std::memory_order_relaxed);
                delete current_relentry_ptr;
            }
            else
            {
                current_relentry_ptr->NextPtr.store(nullptr, MoStoreUnSeq_);
                if (!keep_head_ptr)
                {
                    keep_head_ptr = current_relentry_ptr;
                    keep_tail_ptr = current_relentry_ptr;
                }
                else
                {
                    keep_tail_ptr->NextPtr.store(current_relentry_ptr, MoStoreUnSeq_);
                    keep_tail_ptr = current_relentry_ptr;
                }
                ++track_count;                
            }
            current_relentry_ptr = next_relentry_ptr;
        }
        if (!keep_head_ptr)
        {
            return;
        }
        RelEntry_* head_relentry_ptr = RetireHead_.load(MoLoad_);
        while (true)
        {
            keep_tail_ptr->NextPtr.store(head_relentry_ptr, MoStoreUnSeq_);
            if (RetireHead_.compare_exchange_strong(head_relentry_ptr, keep_head_ptr, EXsuccess_, EXfailure_))
            {
                RetireCount_.fetch_add(track_count, std::memory_order_acq_rel);
                break;
            }
            else
            {
                TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    bool AdaptivePackedCellContainer::PublishHeapPtrWithAdaptiveBackoff(void* target_publishable_ptr, uint16_t max_retries)
    {
        int publish_attempt = 0;

        while (publish_attempt <= max_retries)
        {
            PublishResult publish_result = PublishHeapPtrPair_(target_publishable_ptr, REL_NONE);
            if (publish_result.ResultStatus == PublishStatus::OK)
            {
                return true;
            }
            packed64_t observed = 0;
            if (BackingPtr && ContainerCapacity_ > 0)
            {
                size_t idx = (
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) % ContainerCapacity_
                );
                observed = BackingPtr[idx].load(MoLoad_);
            }
            if (APCManagerPtr_)
            {
                auto& backoff = APCManagerPtr_->GetManagersAdaptiveBackoff();
                backoff.AdaptiveBackOffPacked(observed);
            }
            ++publish_attempt;
        }
        return false;
    }



    void PackedCellContainerManager::ProcessRemainingWorkOfAPC_(NodeOfAdaptivePackedCellContainer_* batch_head_apc_ptr, uint64_t min_epoch) noexcept
    {
        while (batch_head_apc_ptr)
        {
            NodeOfAdaptivePackedCellContainer_* next_apc = batch_head_apc_ptr->StackNextPtr.load(MoLoad_);
            NodeOfAdaptivePackedCellContainer_* node_ptr = batch_head_apc_ptr;
            if (node_ptr->DeadAPC.load(MoLoad_) || node_ptr->APCContainerPtr == nullptr)
            {
                PushANodeAtHeadInStackOfAdaptivePackedCellContainer_(CleanUpStackHead_, node_ptr);
                batch_head_apc_ptr = next_apc;
                continue;
            }
            AdaptivePackedCellContainer* current_apc_ptr = node_ptr->APCContainerPtr;
            if (current_apc_ptr)
            {
                if (node_ptr->ReclaimationNeededAPC.exchange(NO_VAL, std::memory_order_acq_rel))
                {
                    try 
                    {
                        current_apc_ptr->TryReclaimRetiredWithMinEpoch(min_epoch);
                    }
                    catch (...)
                    {
                        if (Logger_)
                        {
                            Logger_("BM", "TryReclaimRetiredWithMinEpoch threw");
                        }
                    }
                }
                if (node_ptr->RequestedBranchedAPC.exchange(NO_VAL, std::memory_order_acq_rel))
                {
                    try 
                    {
                        current_apc_ptr->TryCreateBranchIfNeeded();
                    }
                    catch (...)
                    {
                        if (Logger_)
                        {
                            Logger_("BM", "TryCreateBranchIfNeeded threw");
                        }
                    }
                }
            }
            batch_head_apc_ptr = next_apc;   
        }
    }

}
