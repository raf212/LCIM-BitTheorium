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




    void AdaptivePackedCellContainer::UpdateRegionRelForIdx_(tag8_t rel_mask) noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        return BranchPluginOfAPC_->OrReadyRelMask(rel_mask);
    }

    
    void AdaptivePackedCellContainer::InitOwned(size_t container_capacity, int node,
        ContainerConf container_cfg,
        size_t alignment
    )
    {
        
        (void)node, (void)alignment;
        FreeAll();
        if (container_capacity == 0)
        {
            throw std::invalid_argument("Capacity == 0");
        }
        if (container_capacity <= PayloadBegin() + 2)
        {
            throw std::invalid_argument("Capacity is too small for APC.");
        }
        
        BackingPtr = new std::atomic<packed64_t>[container_capacity];
        packed64_t idle_cell = PackedCell64_t::MakeInitialPacked(container_cfg.InitialMode);
        for (size_t i = 0; i < container_capacity; i++)
        {
            BackingPtr[i].store(idle_cell, MoStoreUnSeq_);
        }
        ContainerCapacity_ = container_capacity;
        IsContainerOwned_ = true;
        RetireBatchThreshold_ = std::max<unsigned>(1, container_cfg.RetireBatchThreshold);
        InitZeroState_();

        if (container_cfg.RegionSize)
        {
            InitRegionIdx(container_cfg.RegionSize);
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
        BranchPluginOfAPC_->Bind(BackingPtr, container_capacity, MasterClockConfPtr_);
        const uint32_t new_branch_id = GlobalBranchIdAlloc_.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t region_count_u32 = container_cfg.RegionSize == NO_VAL ? NO_VAL : static_cast<uint32_t>(((container_capacity - PayloadBegin()) + container_cfg.RegionSize -1) / container_cfg.RegionSize);

        BranchPluginOfAPC_->InitRootOrChildBranch(
            new_branch_id,
            NO_VAL,
            container_capacity,
            PackedCellBranchPlugin::TreePosition::ROOT,
            container_cfg.BranchSplitThresholdPercentage,
            0u,
            container_cfg.BranchMaxDepth,
            static_cast<uint32_t>(container_cfg.ProducerBlockSize),
            container_cfg.BackgroundEpochAdvanceMS,
            static_cast<uint32_t>(container_cfg.RegionSize),
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
    }

    void AdaptivePackedCellContainer::InitRegionIdx(size_t region_size) noexcept
    {
        if (!IfAnyValid_())
        {
            return;
        }
        if (region_size == 0)
        {
            return;
        }
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        uint32_t current_region_size = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_SIZE);
        bool ok = BranchPluginOfAPC_->UpdateBranchMeta32CAS(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_SIZE, current_region_size, static_cast<uint32_t>(region_size));
        if (!ok)
        {
            return;
        }
        size_t number_of_region = ((GetPayloadCapacity() + region_size - 1) / region_size);
        uint32_t current_number_of_region = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_COUNT);
        ok = BranchPluginOfAPC_->UpdateBranchMeta32CAS(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_COUNT, current_number_of_region, static_cast<uint32_t>(number_of_region));
        if (!ok)
        {
            return;
        }
        RegionRelArray_.reset(
            new std::atomic<uint8_t>[number_of_region]
        );
        RegionEpochArray_.reset(
            new std::atomic<uint64_t>[number_of_region]
        );
        for (size_t region = 0; region < number_of_region; region++)
        {
            RegionRelArray_[region].store(0, MoStoreSeq_);
            RegionEpochArray_[region].store(0, MoStoreSeq_);
        }
        size_t words = (number_of_region + MAX_VAL - 1) / MAX_VAL;
        RelBitmaps_.assign(LN_OF_BYTE_IN_BITS, std::vector<uint64_t>(words, 0ull));
        for (size_t region = 0; region < number_of_region; region++)
        {
            size_t base = region * region_size;
            size_t end = std::min(GetPayloadCapacity(), base + region_size);
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
        if (!BranchPluginOfAPC_)
        {
            return NO_VAL;
        }
        size_t current_block_size = static_cast<size_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::PRODUCER_BLOCK_SIZE));
        thread_local size_t block_base = 0;
        thread_local size_t block_left = 0;
        if (block_left == 0)
        {
            size_t block = std::min<size_t>(current_block_size, GetPayloadCapacity());
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

    void PackedCellContainerManager::ProcessRemainingWorkOfAPC_(NodeOfAdaptivePackedCellContainer_* batch_head_apc_ptr, uint64_t min_epoch) noexcept
    {
        (void)min_epoch;
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
        if (BranchPluginOfAPC_->ShouldSplitNow())
        {
            BranchPluginOfAPC_->TurnOnFlags(static_cast<uint32_t>(PackedCellBranchPlugin::APCFlags::SATURATED));
        }
        else
        {
            BranchPluginOfAPC_->ClearFlags(static_cast<uint32_t>(PackedCellBranchPlugin::APCFlags::SATURATED));
        }
        if (MasterClockConfPtr_)
        {
            BranchPluginOfAPC_->TouchLocalMetaClock48();
        }
    }

    void AdaptivePackedCellContainer::TryCreateBranchIfNeeded() noexcept
    {
        if (!BranchPluginOfAPC_ || !APCManagerPtr_)
        {
            return;
        }
        if (!BranchPluginOfAPC_->HasThisFlag(PackedCellBranchPlugin::APCFlags::ENABLE_BRANCHING))
        {
            return;
        }
        if (!BranchPluginOfAPC_->ShouldSplitNow())
        {
            return;
        }
        if (!BranchPluginOfAPC_->TryMarkSplitInFlight())
        {
            return;
        }

        auto clear_flag = [&]() noexcept
        {
            BranchPluginOfAPC_->ClearFlags(static_cast<uint32_t>(PackedCellBranchPlugin::APCFlags::SPLIT_INFLIGHT));
        };
        
        PackedCellBranchPlugin::TreePosition branch_tree_position = PackedCellBranchPlugin::TreePosition::TREE_OVERFLOW;
        //const reads
        const size_t child_capacity = SuggestedChildCapacity_();
        const uint32_t left_child_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::LEFT_CHILD_ID);
        const uint32_t right_child_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::RIGHT_CHILD_ID);
        const uint32_t region_size_of_current_branch = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_SIZE);
        const uint32_t branch_split_threshold = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::SPLIT_THRESHOLD_PERCENTAGE);
        const uint32_t max_depth_of_container = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::MAX_DEPTH);
        const uint32_t producer_block_size = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::PRODUCER_BLOCK_SIZE);
        const uint32_t background_epoch_ms = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::BACKGROUND_EPOCH_ADVANCE_MS);
        const uint32_t retire_branch_thrashold = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::RETIRE_BRANCH_THRASHOLD);
        const PackedMode probable_initial_branch_mode = static_cast<PackedMode>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::DEFINED_MODE_OF_CURRENT_APC));
        ContainerConf child_container_conf = {};
        child_container_conf.InitialMode = probable_initial_branch_mode;
        child_container_conf.ProducerBlockSize = producer_block_size;
        child_container_conf.RegionSize = static_cast<size_t>(region_size_of_current_branch);
        child_container_conf.RetireBatchThreshold = retire_branch_thrashold;
        child_container_conf.BackgroundEpochAdvanceMS = background_epoch_ms;
        child_container_conf.EnableBranching = true;
        child_container_conf.BranchSplitThresholdPercentage = branch_split_threshold;
        child_container_conf.BranchMaxDepth = max_depth_of_container;
        child_container_conf.BranchMinChildCapacity = child_capacity;
        //end

        if (left_child_id == BranchPluginOfAPC_->BRANCH_SENTINAL)
        {
            branch_tree_position = PackedCellBranchPlugin::TreePosition::LEFT;
        }
        else if (right_child_id == BranchPluginOfAPC_->BRANCH_SENTINAL)
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
            if (child_capacity <= MINIMUM_BRANCH_CAPACITY)
            {
                clear_flag();
                return;
            }

            child_container = new AdaptivePackedCellContainer();

            child_container->SetManagerForGlobalAPC(APCManagerPtr_);
            child_container->InitOwned(child_capacity, UsedNode_, child_container_conf);

            const uint32_t child_brunch_id = child_container->GetBranchId();
            const uint32_t parent_id_current_brunch_id = GetBranchId();
            const uint32_t parent_depth_current_depth = BranchPluginOfAPC_->ReadMetaCellValue32(
                PackedCellBranchPlugin::MetaIndexOfAPCBranch::BRANCH_DEPTH
            );
            uint32_t region_count = static_cast<uint32_t>(
                region_size_of_current_branch == NO_VAL ? NO_VAL : ((child_capacity + region_size_of_current_branch -1) / region_size_of_current_branch)
            );
            child_container->GetBranchPlugin()->InitRootOrChildBranch(
                child_brunch_id,
                parent_id_current_brunch_id,
                child_capacity,
                branch_tree_position,
                branch_split_threshold,
                parent_depth_current_depth + 1u,
                max_depth_of_container,
                producer_block_size,
                background_epoch_ms,
                region_size_of_current_branch,
                region_count,
                static_cast<uint8_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::BRANCH_PRIORITY)),
                ZERO_PRIORITY
            );
            bool attached = false;
            if (branch_tree_position == PackedCellBranchPlugin::TreePosition::LEFT)
            {
                attached = BranchPluginOfAPC_->TrySetLeftChild(child_brunch_id);
            }
            else if (branch_tree_position == PackedCellBranchPlugin::TreePosition::RIGHT)
            {
                attached = BranchPluginOfAPC_->TrySetRightChild(child_brunch_id);
            }
            if (!attached)
            {
                child_container->FreeAll();
                delete child_container;
                clear_flag();
                return;
            }
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

    size_t AdaptivePackedCellContainer::SuggestedChildCapacity_() const noexcept
    {
        const size_t payload_capacity = GetPayloadCapacity();
        const size_t min_child_capacity = 256u;
        const size_t child_payload_size = std::max<size_t>(min_child_capacity, payload_capacity / 2);
        return child_payload_size + PayloadBegin();
    }

    

}
