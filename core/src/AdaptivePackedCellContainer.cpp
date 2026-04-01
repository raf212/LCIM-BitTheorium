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
        if (!IfAPCBranchValid() || number_of_slots == 0)
        {
            return SIZE_MAX;
        }

        if (GetPayloadEnd() <= PayloadBegin())
        {
            return SIZE_MAX;
        }
        while (true)
        {
            const uint32_t current_producer_sequence = GetProducerCursorPlacement();
            if (current_producer_sequence == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                return SIZE_MAX;
            }
            uint32_t base_producer_sequense = current_producer_sequence;
            if (base_producer_sequense < PayloadBegin())
            {
                base_producer_sequense = PayloadBegin();
            }
            const uint64_t desired64 = static_cast<uint64_t>(base_producer_sequense) + static_cast<uint64_t>(number_of_slots);
            uint32_t desiered_producer_sequence = 0;
            if (desired64 >= static_cast<uint64_t>(GetPayloadEnd()))
            {
                desiered_producer_sequence = static_cast<uint32_t>(GetPayloadEnd());
            }
            else
            {
                desiered_producer_sequence = static_cast<uint32_t>(desired64);
            }
            bool ok = UpdateProducerCursorPlacement(desiered_producer_sequence);
            if (ok)
            {
                return static_cast<size_t>(base_producer_sequense);
            }
        }
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
    




    void AdaptivePackedCellContainer::UpdateRegionRelForIdx_(tag8_t rel_mask) noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        return BranchPluginOfAPC_->OrReadyRelMask(rel_mask);
    }

    
    void AdaptivePackedCellContainer::InitOwned(size_t container_capacity,
        ContainerConf container_cfg
    )
    {
        
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
        InitZeroState_();
        RefreshAPCMeta_();
    }

    void AdaptivePackedCellContainer::InitZeroState_() noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        BranchPluginOfAPC_->ForceOccupancyUpdateAndReturn(0);
        BranchPluginOfAPC_->MakeAPCBranchOwned();
        BranchPluginOfAPC_->ResetTotalCASFailureForThisBranch();
        UpdateProducerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
        UpdateConsumerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
    }

    void AdaptivePackedCellContainer::FreeAll() noexcept
    {
        if (!IfAPCBranchValid())
        {
            return;
        }
        
        try 
        {
            PackedCellContainerManager::Instance().UnRegisterAdaptivePackedCellContainer(this);
        }
        catch (...)
        {

        }
        AdaptiveBackoffOfAPCPtr_ = nullptr;
        MasterClockConfPtr_ = nullptr;        
        if (BackingPtr)
        {
            if (BranchPluginOfAPC_->IsBranchOwnedByFlag())
            {
                delete[] BackingPtr;
            }
            BackingPtr = nullptr;
        }

        RegionRelArray_.reset();
        RegionEpochArray_.reset();
        RelBitmaps_.clear();
        BranchPluginOfAPC_->ReleseOwneshipFlag();
        BranchPluginOfAPC_.reset();
    }

    void AdaptivePackedCellContainer::InitRegionIdx(size_t region_size) noexcept
    {
        if (!IfAPCBranchValid())
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
        bool ok = BranchPluginOfAPC_->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_SIZE, current_region_size, static_cast<uint32_t>(region_size));
        if (!ok)
        {
            return;
        }
        size_t number_of_region = ((GetPayloadCapacity() + region_size - 1) / region_size);
        uint32_t current_number_of_region = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_COUNT);
        ok = BranchPluginOfAPC_->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::REGION_COUNT, current_number_of_region, static_cast<uint32_t>(number_of_region));
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
                const size_t absolute_idx = PayloadBegin() + i;
                accum |= PackedCell64_t::ExtractFullRelFromPacked(BackingPtr[absolute_idx].load(MoLoad_));
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
            return SIZE_MAX;
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
        if (!IfAPCBranchValid())
        {
            return;
        }
        BranchPluginOfAPC_->ForceOccupancyUpdateAndReturn(NO_VAL);
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
            child_container->InitOwned(child_capacity, child_container_conf);

            const uint32_t child_brunch_id = child_container->GetBranchId();

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

    AdaptivePackedCellContainer* PackedCellContainerManager::GetAPCPtrFromBranchId(uint32_t branch_id) noexcept
    {
        if (branch_id == NO_VAL || branch_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
        {
            return nullptr;
        }
        NodeOfAdaptivePackedCellContainer_* cur_node_of_apc_ptr = RegistryHeadOfAPCNodesPtr_.load(MoLoad_);
        while (cur_node_of_apc_ptr)
        {
            AdaptivePackedCellContainer* apc_ptr = cur_node_of_apc_ptr->APCContainerPtr;
            if (apc_ptr && !cur_node_of_apc_ptr->DeadAPC.load(MoLoad_))
            {
                if (apc_ptr->GetBranchId() == branch_id)
                {
                    return apc_ptr;
                }
            }
            cur_node_of_apc_ptr = cur_node_of_apc_ptr->RegistryNextPtr;
        }
        return nullptr;
    }

    uint32_t AdaptivePackedCellContainer::ProducerORConsumerCursorSetAndGet_(std::optional<uint32_t> cursor_placement, int32_t increment_or_decrement_of_cursor, 
        bool* did_changed_easy_return, const PackedCellBranchPlugin::MetaIndexOfAPCBranch cursors_meta_idx
    ) noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            if (did_changed_easy_return)
            {
                *did_changed_easy_return = false;
            }
            return PackedCellBranchPlugin::BRANCH_SENTINAL;
        }
        if (GetPayloadEnd() <= PayloadBegin())
        {
            if (did_changed_easy_return)
            {
                *did_changed_easy_return = false;
            }
            return PackedCellBranchPlugin::BRANCH_SENTINAL;
        }
        
        while (true)
        {
            const uint32_t current_cursor_placement = BranchPluginOfAPC_->ReadMetaCellValue32(cursors_meta_idx);
            uint32_t desired_cursor_place = current_cursor_placement;
            if (cursor_placement.has_value())
            {
                desired_cursor_place = cursor_placement.value();
            }
            else if (increment_or_decrement_of_cursor != 0)
            {
                if (current_cursor_placement == PackedCellBranchPlugin::BRANCH_SENTINAL)
                {
                    if (did_changed_easy_return)
                    {
                        *did_changed_easy_return = false;
                    }
                    return current_cursor_placement;
                }
                const int64_t winded_cursor = static_cast<int64_t>(current_cursor_placement) + static_cast<int64_t>(increment_or_decrement_of_cursor);
                if (winded_cursor < static_cast<int64_t>(PayloadBegin()))
                {
                    desired_cursor_place = PayloadBegin();
                }
                else if (winded_cursor >= static_cast<int64_t>(GetPayloadEnd()))
                {
                    desired_cursor_place = static_cast<uint32_t>(GetPayloadEnd() - 1u);
                }
                else
                {
                    desired_cursor_place = static_cast<uint32_t>(winded_cursor);
                }
            }
            else
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = false;
                }
                return current_cursor_placement;
            }
            
            if (desired_cursor_place < PayloadBegin())
            {
                desired_cursor_place = PayloadBegin();
            }
            else if (desired_cursor_place >= GetPayloadEnd())
            {
                desired_cursor_place = static_cast<uint32_t>(GetPayloadEnd() - 1u);
            }

            if (desired_cursor_place == current_cursor_placement)
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = false;
                }
                return current_cursor_placement;
            }
            if (BranchPluginOfAPC_->JustUpdateValueOfMeta32(cursors_meta_idx, current_cursor_placement, desired_cursor_place))
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = true;
                }
                return desired_cursor_place;
            }
        }
    }
    
    size_t AdaptivePackedCellContainer::OccupancyAddOrSubAndGetAfterChange(int delta) noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return SIZE_MAX;
        }

        if (delta == 0)
        {
            return static_cast<size_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT));
        }
        while (true)
        {
            packed64_t current_occupancy_cell = BranchPluginOfAPC_->ReadFullMetaCell(PackedCellBranchPlugin::MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT);
            val32_t current_occupancy = PackedCell64_t::ExtractValue32(current_occupancy_cell);
            if (current_occupancy == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                return PackedCellBranchPlugin::BRANCH_SENTINAL;
            }
            
            int64_t next_occupancy_winded = static_cast<int64_t>(current_occupancy) + static_cast<int64_t>(delta);
            if (next_occupancy_winded < 0)
            {
                next_occupancy_winded = 0;
            }
            constexpr int64_t high_val = static_cast<int64_t>(PackedCellBranchPlugin::BRANCH_SENTINAL - 1u);
            if (next_occupancy_winded > high_val)
            {
                next_occupancy_winded = high_val;
            }
            
            uint32_t next_occupancy = static_cast<uint32_t>(next_occupancy_winded);
            if (BranchPluginOfAPC_->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT, current_occupancy, next_occupancy))
            {
                return static_cast<size_t>(next_occupancy);
            }
            if (APCManagerPtr_)
            {
                auto& backoff = APCManagerPtr_->GetManagersAdaptiveBackoff();
                backoff.AdaptiveBackOffPacked(current_occupancy_cell);
            }
            
        }
    }


}
