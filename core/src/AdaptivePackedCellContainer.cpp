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
            if (relentry_ptr->APCDeviceFence.HandleDeviceFencePtr && relentry_ptr->APCDeviceFence.IsSignaled)
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

    PublishResult AdaptivePackedCellContainer::TryPublishPairedCellCLK48_(size_t start_idx, uint64_t ptr_value, tag8_t relmask) noexcept
    {
        if (!IfAnyValid_())
        {
            return {PublishStatus::INVALID, SIZE_MAX};
        }
        size_t head = start_idx % Capacity_;
        size_t tail = (head + 1) % Capacity_;
        //read cur slot
        packed64_t cur_head = BackingPtr[head].load(MoLoad_);
        packed64_t cur_tail = BackingPtr[tail].load(MoLoad_);
        tag8_t locality_head = PackedCell64_t::ExtractLocalityFromPacked(cur_head);
        tag8_t locality_tail = PackedCell64_t::ExtractLocalityFromPacked(cur_tail);

        if (locality_head != ST_IDLE || locality_tail != ST_IDLE)
        {
            return {PublishStatus::FULL, SIZE_MAX};
        }
        
        packed64_t claimed_head = PackedCell64_t::SetLocalityInPacked(cur_head, ST_CLAIMED);
        packed64_t claimed_tail = PackedCell64_t::SetLocalityInPacked(cur_tail, ST_CLAIMED);

        uint64_t ptr_low48 = ptr_value & MaskBits(CLK_B48);
        uint16_t ptr_high16 = static_cast<uint16_t>((ptr_value >> CLK_B48) & MaskBits(PTR_HIGH16));
        packed64_t expected_head = cur_head;
        if (!BackingPtr[head].compare_exchange_strong(expected_head, claimed_head, EXsuccess_, EXfailure_))
        {
            TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
            return {PublishStatus::FULL, SIZE_MAX};
        }
        
        packed64_t expected_tail = cur_tail;
        if (!BackingPtr[tail].compare_exchange_strong(expected_tail, claimed_tail, EXsuccess_, EXfailure_))
        {
            //idle the lead 
            packed64_t idle_head = PackedCell64_t::ComposeCLKVal48X_64(0u, PackedCell64_t::ExtractSTRL(cur_head));
            BackingPtr[head].store(idle_head, MoStoreSeq_);
            BackingPtr[head].notify_all();
            TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
            return {PublishStatus::FULL, SIZE_MAX};
        }
        
        strl16_t sr_tail = MakeSTRL4_t(DEFAULT_INTERNAL_PRIORITY, ST_PUBLISHED, relmask, RELOFFSET_TAIL_PTR, static_cast<unsigned>(PackedMode::MODE_CLKVAL48));

        packed64_t tail_packed = PackedCell64_t::ComposeCLK48u_64(ptr_low48, sr_tail);

        packed64_t head_packed = PackedCell64_t::ComposeCLK48u_64(ptr_high16, sr_tail);
        head_packed = PackedCell64_t::SetRelOffsetInPacked(head_packed, REL_OFFSET_HEAD_PTR);
        BackingPtr[tail].store(tail_packed, MoStoreSeq_);
        BackingPtr[head].store(head_packed, MoStoreSeq_);
        BackingPtr[head].notify_all();
        Occupancy_.fetch_add(1, std::memory_order_acq_rel);
        return {PublishStatus::OK, head};
    }

    std::optional<uint64_t>AdaptivePackedCellContainer::ExtractPairedSlot48Ptr_(size_t idx) const noexcept
    {
        if (!IfIdxValid_(idx))
        {
            return std::nullopt;
        }
        packed64_t packed_data = BackingPtr[idx].load(MoLoad_);
        tag8_t locality = PackedCell64_t::ExtractLocalityFromPacked(packed_data);
        tag8_t pctype = PackedCell64_t::ExtractPCellTypeFromPacked(packed_data);
        if (locality != ST_PUBLISHED || pctype != static_cast<unsigned>(PackedMode::MODE_CLKVAL48))
        {
            return std::nullopt;
        }
        
        tag8_t rel_offset = PackedCell64_t::ExtractRelOffsetFromPacked(packed_data);
        size_t head_idx, tail_idx;
        if (rel_offset == REL_OFFSET_HEAD_PTR)
        {
            head_idx = idx;
            tail_idx = (idx + 1) % Capacity_;
        }
        else if (rel_offset == RELOFFSET_TAIL_PTR)
        {
            tail_idx = idx;
            head_idx = idx - 1;
            if (idx == 0)
            {
                head_idx = Capacity_ - 1;
            }
        }
        else
        {
            return std::nullopt;
        }
        
        packed64_t head_packed = BackingPtr[head_idx].load(MoLoad_);
        packed64_t tail_packed = BackingPtr[tail_idx].load(MoLoad_);
        if (PackedCell64_t::ExtractLocalityFromPacked(head_packed) != ST_PUBLISHED || PackedCell64_t::ExtractLocalityFromPacked(tail_packed))
        {
            return std::nullopt;
        }
        uint64_t head_ptr_value = PackedCell64_t::ExtractClk48(head_packed);
        uint64_t tail_ptr_value_low48 = PackedCell64_t::ExtractClk48(tail_packed);

        uint64_t high16_ptr_value = head_ptr_value & MaskBits(PTR_HIGH16);

        uint64_t raw_pointer = (static_cast<uint64_t>(high16_ptr_value) << CLK_B48) | (tail_ptr_value_low48 & MaskBits(CLK_B48));
        return raw_pointer;
    }

    size_t AdaptivePackedCellContainer::RegisterRelPackedNode_(packed64_t packed_cell) noexcept
    {
        if (RelOffsetCapacity_ == 0)
        {
            return SIZE_MAX;
        }
        size_t old_reloffset_size = RelOffsetAlloc_.load(std::memory_order_relaxed);
        while (true)
        {
            if (old_reloffset_size >= RelOffsetCapacity_)
            {
                return SIZE_MAX;
            }
            if (RelOffsetAlloc_.compare_exchange_weak(old_reloffset_size, old_reloffset_size + 1, EXsuccess_, EXfailure_))
            {
                break;
            }
            TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
        }
        size_t idx = old_reloffset_size;
        RelEntry_* rel_entry_ptr = new RelEntry_(packed_cell);
        std::uintptr_t raw_cell_ptr = reinterpret_cast<std::uintptr_t>(rel_entry_ptr);
        RelOffset_[idx].store(raw_cell_ptr, MoStoreSeq_);
        return idx;
    }

    size_t AdaptivePackedCellContainer::RegisterRelHeapNode_(
        void* heap_ptr, size_t heap_size, PackedCellDataType cell_dtype,
        FinalizerKind_ fk, std::function<void(RelEntry_*)> finalizer,
        DeviceFence_ apc_device_fence
    ) noexcept
    {
        if (RelOffsetCapacity_ == 0)
        {
            return SIZE_MAX;
        }
        size_t old_rel_offset_size = RelOffsetAlloc_.load(std::memory_order_relaxed);
        while (true)
        {
            if (old_rel_offset_size >= RelOffsetCapacity_)
            {
                return SIZE_MAX;
            }
            if (RelOffsetAlloc_.compare_exchange_weak(old_rel_offset_size, old_rel_offset_size + 1, EXsuccess_, EXfailure_))
            {
                break;
            }
            TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
        }
        size_t idx = old_rel_offset_size;
        RelEntry_* rel_entry_ptr = new RelEntry_(heap_ptr, heap_size, cell_dtype);
        rel_entry_ptr->KindFinalizer = fk;
        rel_entry_ptr->FinalizerPtr = std::move(finalizer);
        rel_entry_ptr->APCDeviceFence = std::move(apc_device_fence);
        std::uintptr_t raw_cell_ptr = reinterpret_cast<std::uintptr_t>(rel_entry_ptr);
        RelOffset_[idx].store(raw_cell_ptr, MoStoreSeq_);
        return idx;
        
    }

    AdaptivePackedCellContainer::RelEntryGuard AdaptivePackedCellContainer::AcquireRelEntry_(size_t idx) noexcept
    {
        QSBRGuard qsbr_guard(this);
        if (idx >= RelOffsetCapacity_)
        {
            return RelEntryGuard(nullptr, std::move(qsbr_guard));
        }
        std::uintptr_t raw_ptr = RelOffset_[idx].load(MoLoad_);
        if (!raw_ptr)
        {
            return RelEntryGuard(nullptr, std::move(qsbr_guard));
        }
        RelEntry_* current_entry = reinterpret_cast<RelEntry_*>(raw_ptr);
        return RelEntryGuard(current_entry, std::move(qsbr_guard));
    }

    AdaptivePackedCellContainer::RelEntryGuard AdaptivePackedCellContainer::ClaimAndAcquireRelEntry_(size_t slot_idx, size_t reloffset_idx) noexcept
    {
        if (slot_idx >= Capacity_ || reloffset_idx >= RelOffsetCapacity_)
        {
            return RelEntryGuard(nullptr, QSBRGuard(this));
        }
        packed64_t curr_cell = BackingPtr[slot_idx].load(MoLoad_);
        packed64_t desired_cell = PackedCell64_t::SetLocalityInPacked(curr_cell, ST_CLAIMED);
        packed64_t expected_cell = curr_cell;
        if (!BackingPtr[slot_idx].compare_exchange_strong(expected_cell, desired_cell, EXsuccess_, EXfailure_))
        {
            TotalCasFailure_.fetch_add(1, std::memory_order_relaxed);
            return RelEntryGuard(nullptr, QSBRGuard(this));
        }
        return AcquireRelEntry_(reloffset_idx);
    }

    void AdaptivePackedCellContainer::RetireRelEntryIdx_(size_t idx) noexcept
    {
        if (idx >= RelOffsetCapacity_)
        {
            return;
        }
        std::uintptr_t raw = RelOffset_[idx].load(MoLoad_);
        if (!raw)
        {
            return;
        }
        
        RelEntry_* rel_entry_ptr = reinterpret_cast<RelEntry_*>(raw);
        RelOffset_[idx].store(0u, MoStoreSeq_);
        uint64_t cur_epoch = GlobalEpoch_.load(MoLoad_);
        rel_entry_ptr->RetireEpoch.store(cur_epoch, MoStoreSeq_);
        RetirePushLocked_(rel_entry_ptr);
        TryReclaimRetired_();
    }

    void AdaptivePackedCellContainer::UpdateRegionRelForIdx_(size_t idx, tag8_t rel_mask) noexcept
    {
        if (RegionSize_ == 0)
        {
            return;
        }
        size_t region = idx / RegionSize_;
        RegionRel_[region].fetch_add(rel_mask, std::memory_order_acq_rel);
        size_t w = region / MAX_VAL;
        size_t b =  region % MAX_VAL;
        uint64_t region_mask = (1ull << b);
        for (unsigned i = 0; i < LN_OF_BYTE_IN_BITS; i++)
        {
            if (rel_mask & (1u << i))
            {
                std::atomic_ref<uint64_t>aref(RelBitmaps_[i][w]);
                aref.fetch_add(region_mask, std::memory_order_acq_rel);
            }
        }
    }

    void AdaptivePackedCellContainer::StartBackgroundReclaimerIfNeed()
    {

    }
    


}
