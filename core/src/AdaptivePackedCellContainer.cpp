#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    class PackedCellContainerManager;

    uint32_t AdaptivePackedCellContainer::GetBranchId() const noexcept
    {
        if (BranchPluginOfAPC_)
        {
            return BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::BRANCH_ID);
        }
        return NO_VAL;
    }

    size_t AdaptivePackedCellContainer::ReserveProducerSlots(size_t number_of_slots) noexcept
    {
        if (!IfAnyValid_() || number_of_slots == 0)
        {
            return SIZE_MAX;
        }

        size_t base = ProducerCursor_.fetch_add(number_of_slots, std::memory_order_relaxed);
        if (base < PayloadBegin())
        {
            const size_t delta = PayloadBegin() - base;
            base = ProducerCursor_.fetch_add(delta, std::memory_order_relaxed) + delta;
        }
        return base;
    }

    size_t AdaptivePackedCellContainer::GetHashedRendomizedStep_(size_t sequense_number) noexcept
    {
        uint64_t mix_hash = (
            (static_cast<uint64_t>(sequense_number) * ID_HASH_GOLDEN_CONST) ^ (static_cast<uint64_t>(sequense_number >> (VALBITS + 1)))
        );
        size_t step = 1;
        if (GetPayloadCapacity() > 1)
        {
            step = static_cast<size_t>((mix_hash % (GetPayloadCapacity() - 1)) + 1);
        }
        return step;
    }

    void AdaptivePackedCellContainer::SetManagerForGlobalAPC(PackedCellContainerManager* pointer_of_global_apc_manager) noexcept
    {
        if (pointer_of_global_apc_manager)
        {
            try
            {
                pointer_of_global_apc_manager->StartPCCManager();
                APCManagerPtr_ = pointer_of_global_apc_manager;
            }
            catch(...)
            {
                pointer_of_global_apc_manager = nullptr;
            }
        }
    }

    struct AdaptivePackedCellContainer::QSBRGuard
    {
        bool IsQSBRGuardActive{false};
        AdaptivePackedCellContainer* ParentContainer{nullptr};
        

        QSBRGuard(AdaptivePackedCellContainer* apc_ptr = nullptr) noexcept :
            ParentContainer(apc_ptr)
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
        RelEntry_* RelEntryPtr{nullptr};
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
            TryReclaimRetirePairedPtr_();
        }
    }

    void AdaptivePackedCellContainer::UpdateRegionRelForIdx_(size_t idx, tag8_t rel_mask) noexcept
    {
        if (BranchPluginOfAPC_)
        {
            BranchPluginOfAPC_->OrReadyRelMask(rel_mask);
        }
        
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
        if (capacity <= PayloadBegin() + 2)
        {
            throw std::invalid_argument("Capacity is too small for APC.");
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
        BranchPluginOfAPC_ = std::make_unique<PackedCellBranchPlugin>();
        BranchPluginOfAPC_->Bind(BackingPtr, ContainerCapacity_, MasterClockConfPtr_);
        const uint32_t new_branch_id = GlobalBranchIdAlloc_.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t region_count_u32 = APCContainerCfg_.RegionSize == NO_VAL ? NO_VAL : static_cast<uint32_t>((ContainerCapacity_ + APCContainerCfg_.RegionSize -1) / APCContainerCfg_.RegionSize);

        BranchPluginOfAPC_->InitRootOrChildBranch(
            new_branch_id,
            NO_VAL,
            ContainerCapacity_,
            PackedCellBranchPlugin::TreePosition::ROOT,
            APCContainerCfg_.BranchSplitThresholdPercentage,
            0u,
            APCContainerCfg_.BranchMaxDepth,
            static_cast<uint32_t>(APCContainerCfg_.RegionSize),
            region_count_u32,
            ZERO_PRIORITY,
            ZERO_PRIORITY
        );
        ProducerCursor_.store(PayloadBegin(), MoStoreUnSeq_);
        ConsumerCursor_.store(PayloadBegin(), MoStoreUnSeq_);
        RefreshAPCMeta_();
    }

    void AdaptivePackedCellContainer::InitZeroState_() noexcept
    {
        Occupancy_.store(0, MoStoreUnSeq_);
        ProducerCursor_.store(PayloadBegin(), MoStoreUnSeq_);
        ConsumerCursor_.store(PayloadBegin(), MoStoreUnSeq_);
        RetireHead_.store(nullptr, MoStoreSeq_);
        RetireCount_.store(0, MoStoreSeq_);
        GlobalEpoch_.store(1, MoStoreSeq_);
        BranchCreateInFlight_.store(false, MoStoreUnSeq_);
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
        BranchPluginOfAPC_.reset();
        
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
            size_t block = std::min<size_t>(APCContainerCfg_.ProducerBlockSize, GetPayloadCapacity());
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


    void AdaptivePackedCellContainer::RefreshAPCMeta_() noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        BranchPluginOfAPC_->UpdateOccupancySnapshot(
            static_cast<uint32_t>(std::min<size_t>(Occupancy_.load(MoLoad_), UINT32_MAX))
        );
        if (MasterClockConfPtr_)
        {
            BranchPluginOfAPC_->TouchLocalMetaClock48();
        }
    }

    void AdaptivePackedCellContainer::TryCreateBranchIfNeeded() noexcept
    {
        if (!APCContainerCfg_.EnableBranching || !BranchPluginOfAPC_ || !APCManagerPtr_)
        {
            return;
        }
        if (!BranchPluginOfAPC_->ShouldSplitNow())
        {
            return;
        }
        
        bool expected = false;
        if (!BranchCreateInFlight_.compare_exchange_strong(expected, true, EXsuccess_, EXfailure_))
        {
            return;
        }

        auto clear_flag = [&]() noexcept
        {
            BranchCreateInFlight_.store(false, MoStoreSeq_);
        };
        
        PackedCellBranchPlugin::TreePosition branch_tree_position = PackedCellBranchPlugin::TreePosition::TREE_OVERFLOW;
        if (BranchPluginOfAPC_->ReadMetaCellValue32(
            PackedCellBranchPlugin::MetaIndexOfAPCBranch::RIGHT_CHILD_ID ) == PackedCellBranchPlugin::BRANCH_SENTINAL
        )
        {
            branch_tree_position = PackedCellBranchPlugin::TreePosition::RIGHT;
        }
        else
        {
            clear_flag();
            return;
        }

        AdaptivePackedCellContainer* child_container = nullptr;

        try
        {
            const size_t child_capacity = SuggestedChildCapacity_();
            if (child_capacity <= PayloadBegin() + 2)
            {
                clear_flag();
                return;
            }

            child_container = new AdaptivePackedCellContainer();
            ContainerConf child_container_conf = APCContainerCfg_;
            child_container->SetManagerForGlobalAPC(APCManagerPtr_);
            child_container->InitOwned(child_capacity, UsedNode_, child_container_conf);

            const uint32_t child_brunch_id = child_container->GetBranchId();
            const uint32_t parent_id_current_brunch_id = GetBranchId();
            const uint32_t parent_depth_current_depth = BranchPluginOfAPC_->ReadMetaCellValue32(
                PackedCellBranchPlugin::MetaIndexOfAPCBranch::BRANCH_DEPTH
            );
            uint32_t region_count = static_cast<uint32_t>(
                APCContainerCfg_.RegionSize == NO_VAL ? NO_VAL : ((child_capacity + APCContainerCfg_.RegionSize -1) / APCContainerCfg_.RegionSize)
            );
            child_container->GetBranchPlugin()->InitRootOrChildBranch(
                child_brunch_id,
                parent_id_current_brunch_id,
                child_capacity,
                branch_tree_position,
                APCContainerCfg_.BranchSplitThresholdPercentage,
                parent_depth_current_depth + 1u,
                APCContainerCfg_.BranchMaxDepth,
                static_cast<uint32_t>(APCContainerCfg_.RegionSize),
                region_count,
                static_cast<uint8_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::BRANCH_PRIORITY)),
                ZERO_PRIORITY
            );
            if (!BranchPluginOfAPC_->TryAttachChildAPC(branch_tree_position, child_brunch_id))
            {
                child_container->FreeAll();
                delete child_container;
                clear_flag();
                return;
            }
            RelEntry_* rel_entry = new RelEntry_(
                child_container,
                child_brunch_id,
                parent_id_current_brunch_id,
                PayloadBegin()
            );
            rel_entry->RetireEpoch.store(GlobalEpoch_.load(MoLoad_), MoStoreSeq_);
            RetirePushLocked_(rel_entry);
            APCManagerPtr_->RegisterAdaptivePackedCellContainer(child_container);
            APCManagerPtr_->RequestForReclaimationOfTheAdaptivePackedCellContainer(child_container);

            BranchPluginOfAPC_->WriteOrUpdateMetaClock48();
            
        }
        catch(...)
        {
            if (child_container)
            {
                try
                {
                    child_container->FreeAll();
                }
                catch(...)
                {
                }
                delete child_container;
            }
        }
        clear_flag();

    }


    

}
