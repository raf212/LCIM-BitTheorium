#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    class PackedCellContainerManager;
    
    uint32_t AdaptivePackedCellContainer::GetBranchId() const noexcept
    {
        if (SegmentIODefinitionPtr_)
        {
            return SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::BRANCH_ID);
        }
        return NO_VAL;
    }

    uint32_t AdaptivePackedCellContainer::GetLogicalId() const noexcept
    {
        if (SegmentIODefinitionPtr_)
        {
            return SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::LOGICAL_NODE_ID);
        }
        return NO_VAL;
    }

    uint32_t AdaptivePackedCellContainer::GetSharedId() const noexcept
    {
        if (SegmentIODefinitionPtr_)
        {
            return SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_ID);
        }
        return NO_VAL;
    }

    size_t AdaptivePackedCellContainer::ReserveProducerSlots(size_t number_of_slots) noexcept
    {
        if (!IfAPCBranchValid() || number_of_slots == 0)
        {
            return SIZE_MAX;
        }
        const size_t payload_capacity = GetPayloadCapacity();
        if (payload_capacity == 0)
        {
            return SIZE_MAX;
        }
        if (number_of_slots > payload_capacity)
        {
            number_of_slots = payload_capacity;
        }
        while (true)
        {
            uint32_t current_producer_cursor = GetProducerCursorPlacement();
            if (current_producer_cursor == SegmentIODefinition::BRANCH_SENTINAL || current_producer_cursor < PayloadBegin() || current_producer_cursor >= GetPayloadEnd())
            {
                current_producer_cursor = PayloadBegin();
            }
            const size_t current_offset = static_cast<size_t>(current_producer_cursor - PayloadBegin()) % payload_capacity;
            const size_t next_offset = (current_offset + number_of_slots) % payload_capacity;
            const uint32_t desired_cursor = static_cast<uint32_t>(PayloadBegin() + next_offset);
            bool changed = false;
            ProducerORConsumerCursorSetAndGet_(
                static_cast<uint32_t>(desired_cursor),
                0,
                &changed,
                MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT
            );
            if (changed)
            {
                return static_cast<size_t>(current_producer_cursor);
            }
        }
    }

    void AdaptivePackedCellContainer::SetManagerForGlobalAPC(PackedCellContainerManager* pointer_of_global_apc_manager) noexcept
    {
        if (pointer_of_global_apc_manager)
        {
            try
            {
                pointer_of_global_apc_manager->StartAPCManager();
                APCManagerPtr_ = pointer_of_global_apc_manager;
            }
            catch(...)
            {
                pointer_of_global_apc_manager = nullptr;
            }
        }
    }


    
    void AdaptivePackedCellContainer::InitOwned(size_t container_capacity,
        ContainerConf container_cfg
    )
    {
        
        FreeAll();
        if (container_capacity <= MINIMUM_BRANCH_CAPACITY)
        {
            throw std::invalid_argument("Capacity is too small for APC.");
        }
        
        BackingPtr = new std::atomic<packed64_t>[container_capacity];
        packed64_t idle_cell = PackedCell64_t::MakeInitialPacked(container_cfg.InitialMode);
        for (size_t i = 0; i < container_capacity; i++)
        {
            BackingPtr[i].store(idle_cell, MoStoreUnSeq_);
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
        SegmentIODefinitionPtr_ = std::make_unique<CausalSegmentController>();
        SegmentIODefinitionPtr_->BindBranchPluginToAPC(BackingPtr, container_capacity, MasterClockConfPtr_);
        const uint32_t new_branch_id = GlobalBranchIdAlloc_.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t logical_node_id = new_branch_id;
        const uint32_t shared_id = NO_VAL;
        SegmentIODefinitionPtr_->InitRootOrChildBranch(
            new_branch_id,
            logical_node_id,
            shared_id,
            container_capacity,
            container_cfg
        );
        InitZeroState_();
        if (container_cfg.RegionSize > 0)
        {
            InitRegionIdx(container_cfg.RegionSize);
        }
        if (APCManagerPtr_)
        {
            APCManagerPtr_->RegisterAdaptivePackedCellContainer(this);
        }
        RefreshAPCMeta_();
    }

    void AdaptivePackedCellContainer::InitAPCAsNode(
        size_t capacity,
        const ContainerConf& container_configuration,
        SegmentIODefinition::APCNodeComputeKind compute_kind,
        uint32_t aux_param_u32
    )
    {
        InitOwned(capacity, container_configuration);
        if (SegmentIODefinitionPtr_)
        {
            SegmentIODefinitionPtr_->InitNodeSemantics(compute_kind, aux_param_u32);
            SegmentIODefinitionPtr_->SetGraphNodeFlag();
        }
    }


    void AdaptivePackedCellContainer::InitRegionIdx(size_t region_size) noexcept
    {
        if (!IfAPCBranchValid() || region_size == 0)
        {
            return;
        }
        uint32_t current_region_size = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_SIZE);
        bool ok = SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(MetaIndexOfAPCNode::REGION_SIZE, current_region_size, static_cast<uint32_t>(region_size));
        if (!ok)
        {
            return;
        }
        size_t number_of_region = ((GetPayloadCapacity() + region_size - 1) / region_size);
        uint32_t current_number_of_region = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_COUNT);
        ok = SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(MetaIndexOfAPCNode::REGION_COUNT, current_number_of_region, static_cast<uint32_t>(number_of_region));
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
        if (!SegmentIODefinitionPtr_)
        {
            return SIZE_MAX;
        }
        size_t current_block_size = static_cast<size_t>(SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::PRODUCER_BLOCK_SIZE));
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



    void AdaptivePackedCellContainer::TryCreateBranchIfNeeded(APCPagedNodeRelMaskClasses rel_mask_hint) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return;
        }
        
        if (!SegmentIODefinitionPtr_->HasThisFlag(
            SegmentIODefinition::ControlEnumOfAPCSegment::ENABLE_BRANCHING
        ))
        {
            return;
        }

        if(!SegmentIODefinitionPtr_->ShouldSplitNow())
        {
            return;
        }

        AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(rel_mask_hint);
        if (grown_apc)
        {
            APCManagerPtr_->RequestForReclaimationOfTheAdaptivePackedCellContainer(grown_apc);
        }
        
    }



    AdaptivePackedCellContainer* PackedCellContainerManager::GetAPCPtrFromBranchId(uint32_t branch_id) noexcept
    {
        if (branch_id == NO_VAL || branch_id == SegmentIODefinition::BRANCH_SENTINAL)
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


    
    size_t AdaptivePackedCellContainer::OccupancyAddOrSubAndGetAfterChange(int delta) noexcept
    {
        if (!SegmentIODefinitionPtr_)
        {
            return SIZE_MAX;
        }

        if (delta == 0)
        {
            return static_cast<size_t>(SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT));
        }
        while (true)
        {
            packed64_t current_occupancy_cell = SegmentIODefinitionPtr_->ReadFullMetaCell(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT);
            val32_t current_occupancy = PackedCell64_t::ExtractValue32(current_occupancy_cell);
            if (current_occupancy == SegmentIODefinition::BRANCH_SENTINAL)
            {
                return SegmentIODefinition::BRANCH_SENTINAL;
            }
            
            int64_t next_occupancy_winded = static_cast<int64_t>(current_occupancy) + static_cast<int64_t>(delta);
            if (next_occupancy_winded < 0)
            {
                next_occupancy_winded = 0;
            }
            constexpr int64_t high_val = static_cast<int64_t>(SegmentIODefinition::BRANCH_SENTINAL - 1u);
            if (next_occupancy_winded > high_val)
            {
                next_occupancy_winded = high_val;
            }
            
            uint32_t next_occupancy = static_cast<uint32_t>(next_occupancy_winded);
            if (SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT, current_occupancy, next_occupancy))
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




    AdaptivePackedCellContainer* AdaptivePackedCellContainer::FindSharedRootOrThis() noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return this;
        }
        AdaptivePackedCellContainer* current_apc_ptr = this;
        while (current_apc_ptr)
        {
            const uint32_t previous_id = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_PREVIOUS_ID);
            if (previous_id == NO_VAL || previous_id == SegmentIODefinition::BRANCH_SENTINAL)
            {
                break;
            }
            AdaptivePackedCellContainer* previous_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(previous_id);
            if (!previous_apc_ptr || previous_apc_ptr == current_apc_ptr)
            {
                break;
            }
            current_apc_ptr = previous_apc_ptr;
        }
        return current_apc_ptr ? current_apc_ptr : this;
        
    }

    AdaptivePackedCellContainer* AdaptivePackedCellContainer::GetNextSharedSegment() noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return nullptr;
        }

        const uint32_t next_apc_id = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_NEXT_ID);

        if (next_apc_id == NO_VAL || next_apc_id == SegmentIODefinition::BRANCH_SENTINAL)
        {
            return nullptr;
        }
        return APCManagerPtr_->GetAPCPtrFromBranchId(next_apc_id);
    }

    bool AdaptivePackedCellContainer::IsAPCSharedChainEmpty() noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return true;
        }
        AdaptivePackedCellContainer* current_apc_ptr = FindSharedRootOrThis();
        while (current_apc_ptr)
        {
            if (current_apc_ptr->OccupancyAddOrSubAndGetAfterChange() > NO_VAL)
            {
                return false;
            }
            if (!current_apc_ptr->IfAPCBranchValid())
            {
                break;
            }
            SegmentIODefinition* current_branch_plugin = current_apc_ptr->GetBranchPlugin();
            uint32_t next_apc_id = current_branch_plugin->ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_NEXT_ID);
            if (next_apc_id == NO_VAL || next_apc_id == SegmentIODefinition::BRANCH_SENTINAL)
            {
                break;
            }
            AdaptivePackedCellContainer* next_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(next_apc_id);
            if (!next_apc_ptr || next_apc_ptr == current_apc_ptr)
            {
                break;
            }
            current_apc_ptr = next_apc_ptr;
        }
        return true;
    }

    bool AdaptivePackedCellContainer::TryPublishRegionalSharedGrowthOnce(APCPagedNodeRelMaskClasses region_kind, packed64_t packed_cell, std::atomic<uint64_t>* growth_counter) noexcept
    {
        const PublishResult local_result = PublishCellByRegionMAskTraverseStartsFromThisAPC(region_kind, packed_cell);
        if (local_result.ResultStatus == PublishStatus::OK)
        {
            return true;
        }
        
        AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(region_kind);
        if (grown_apc)
        {
            if (growth_counter)
            {
                growth_counter->fetch_add(1, std::memory_order_relaxed);
            }
            return grown_apc->PublishCellByRegionMAskTraverseStartsFromThisAPC(region_kind, packed_cell).ResultStatus == PublishStatus::OK;
        }
        return false;
        
    }


    std::optional<packed64_t> AdaptivePackedCellContainer::ConsumeCellByRegionMaskTraverseStartFromThisAPC(APCPagedNodeRelMaskClasses region_kind, size_t& scan_cursor) noexcept
    {
        auto maybe_packed_cell = TryConsumeAndIdleFromRegionLocal_(region_kind, scan_cursor);
        if (maybe_packed_cell)
        {
            return *maybe_packed_cell;
        }

        AdaptivePackedCellContainer* current_apc = GetNextSharedSegment();
        while (current_apc)
        {
            size_t sibling_cursor = PayloadBegin();
            auto maybe_shared_packed_cell = current_apc->TryConsumeAndIdleFromRegionLocal_(region_kind, sibling_cursor);
            if (maybe_shared_packed_cell)
            {
                return *maybe_shared_packed_cell;
            }
            current_apc = current_apc->GetNextSharedSegment();
        }
        return std::nullopt;
    }

    PublishResult AdaptivePackedCellContainer::PublishCellByRegionMAskTraverseStartsFromThisAPC(APCPagedNodeRelMaskClasses region_kind, packed64_t cell_to_publish, uint16_t max_tries) noexcept
    {
        if (!IfAPCBranchValid())
        {
            const PublishResult invalid{};
            return invalid;
        }
        
        const PublishResult local_result = TryPublishToRegionLocal_(region_kind, cell_to_publish, true, max_tries);
        if (local_result.ResultStatus == PublishStatus::OK)
        {
            return local_result;
        }

        AdaptivePackedCellContainer* curren_or_next_container_ptr = GetNextSharedSegment();
        while (curren_or_next_container_ptr)
        {
            const PublishResult sibling_result_publish = curren_or_next_container_ptr->TryPublishToRegionLocal_(region_kind, cell_to_publish, true, max_tries);
            if (sibling_result_publish.ResultStatus == PublishStatus::OK)
            {
                return sibling_result_publish;
            }
            curren_or_next_container_ptr = curren_or_next_container_ptr->GetNextSharedSegment();
        }
        if (SegmentIODefinitionPtr_->ShouldSplitNow())
        {
            AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(region_kind, true);
            if (grown_apc)
            {
                return grown_apc->TryPublishToRegionLocal_(region_kind, cell_to_publish, true, max_tries);
            }
        }
        return local_result;
    }

    AdaptivePackedCellContainer* AdaptivePackedCellContainer::GrowSharedNodeByRegionKind(APCPagedNodeRelMaskClasses desired_region_kind, bool enable_recursive_branching) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return nullptr;
        }

        if (!SegmentIODefinitionPtr_->HasThisFlag(SegmentIODefinition::ControlEnumOfAPCSegment::ENABLE_BRANCHING))
        {
            return nullptr;
        }

        if (!SegmentIODefinitionPtr_->TryMarkSplitInFlight())
        {
            return nullptr;
        }

        auto clear_flags = [&]() noexcept
        {
            SegmentIODefinitionPtr_->ClearOneControlEnumFlagOfAPC(
                SegmentIODefinition::ControlEnumOfAPCSegment::SPLIT_INFLIGHT
            );
        };
        AdaptivePackedCellContainer* new_shared_container = nullptr;
        ContainerConf child_configuration{};
        child_configuration.InitialMode = static_cast<PackedMode>(SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::DEFINED_MODE_OF_CURRENT_APC));
        child_configuration.ProducerBlockSize = static_cast<size_t>(SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::PRODUCER_BLOCK_SIZE));
        child_configuration.RegionSize = static_cast<size_t>(SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_SIZE));
        child_configuration.RetireBatchThreshold = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::RETIRE_BRANCH_THRASHOLD);
        child_configuration.BackgroundEpochAdvanceMS = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::BACKGROUND_EPOCH_ADVANCE_MS);
        child_configuration.EnableBranching = enable_recursive_branching;
        child_configuration.BranchSplitThresholdPercentage = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE);
        child_configuration.BranchMaxDepth = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::MAX_DEPTH);
        child_configuration.BranchMinChildCapacity = SuggestedChildCapacity_();

        try
        {
            new_shared_container = new AdaptivePackedCellContainer();
            new_shared_container->SetManagerForGlobalAPC(APCManagerPtr_);
            new_shared_container->InitOwned(child_configuration.BranchMinChildCapacity, child_configuration);
        }
        catch(...)
        {
            clear_flags();
            return nullptr;
        }

        if (!new_shared_container)
        {
            clear_flags();
            return nullptr;
        }
        
        SegmentIODefinition* new_branch_plugin = new_shared_container->GetBranchPlugin();
        if (!new_branch_plugin)
        {
            new_shared_container->FreeAll();
            delete new_shared_container;
            clear_flags();
            return nullptr;
        }

        const uint32_t this_branch_id = GetBranchId();
        const uint32_t this_logical_id = GetLogicalId();
        const uint32_t this_shared_id = (GetSharedId() == NO_VAL) ? this_branch_id : GetSharedId();

        new_branch_plugin->InitLogicalNodeIdentity(this_logical_id, this_shared_id, false);

        new_branch_plugin->InitNodeSemantics(
            static_cast<SegmentIODefinition::APCNodeComputeKind>(SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_COMPUTE_KIND)),
            SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_AUX_PARAM_U32)
        );

        new_branch_plugin->SetSegmentRegionKind(desired_region_kind);

        const auto copy_meta = [&](MetaIndexOfAPCNode idx) noexcept
        {
            const uint32_t original_value = SegmentIODefinitionPtr_->ReadMetaCellValue32(idx);
            const uint32_t current_value = new_branch_plugin->ReadMetaCellValue32(idx);
            new_branch_plugin->JustUpdateValueOfMeta32(idx, current_value, original_value);
        };
        copy_meta(MetaIndexOfAPCNode::FEEDFORWARD_IN_TARGET_ID);
        copy_meta(MetaIndexOfAPCNode::FEEDFORWARD_OUT_TARGET_ID);
        copy_meta(MetaIndexOfAPCNode::FEEDBACKWARD_IN_TARGET_ID);
        copy_meta(MetaIndexOfAPCNode::FEEDBACKWARD_OUT_TARGET_ID);
        copy_meta(MetaIndexOfAPCNode::LATERAL_0_TARGET_ID);
        copy_meta(MetaIndexOfAPCNode::LATERAL_1_TARGET_ID);

        AdaptivePackedCellContainer* tail_apc = FindSharedRootOrThis();
        AdaptivePackedCellContainer* prev_apc = nullptr;
        while (tail_apc)
        {
            prev_apc = tail_apc;
            tail_apc = tail_apc->GetNextSharedSegment();
        }

        if (!prev_apc)
        {
            new_shared_container->FreeAll();
            delete new_shared_container;
            clear_flags();
            return nullptr;
        }
        
        if (
            !prev_apc->GetBranchPlugin()->TryBindShareNext(new_shared_container->GetBranchId()) ||
            !new_branch_plugin->TryBindSharedPrevious(prev_apc->GetBranchId())
        )
        {
            new_shared_container->FreeAll();
            delete new_shared_container;
            clear_flags();
            return nullptr;
        }

        const uint32_t current_group_size = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        uint32_t next_group_size = ((current_group_size == NO_VAL) ? 1 : current_group_size) + 1; 

        SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(
            MetaIndexOfAPCNode::NODE_GROUP_SIZE,
            current_group_size,
            next_group_size
        );
        const uint32_t new_group_size_expected = new_branch_plugin->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        new_branch_plugin->JustUpdateValueOfMeta32(
            MetaIndexOfAPCNode::NODE_GROUP_SIZE,
            new_group_size_expected,
            next_group_size
        );
        
        RefreshAPCMeta_();
        new_shared_container->RefreshAPCMeta_();
        clear_flags();
        return new_shared_container;
    }


}
