#include "APCSegmentsCausalCordinator.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    class PackedCellContainerManager;
    
    uint32_t AdaptivePackedCellContainer::GetBranchId() noexcept
    {

        return ReadMetaCellValue32(MetaIndexOfAPCNode::BRANCH_ID);

    }

    uint32_t AdaptivePackedCellContainer::GetLogicalId() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::LOGICAL_NODE_ID);
    }

    uint32_t AdaptivePackedCellContainer::GetSharedId() noexcept
    {
            return ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_ID);
    }

    size_t AdaptivePackedCellContainer::ReserveProducerSlots(size_t number_of_slots) noexcept
    {
        if (!IfAPCBranchValid() || number_of_slots == 0)
        {
            return SIZE_MAX;
        }
        const size_t payload_capacity = PayloadCapacityFromHeader();
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
            if (current_producer_cursor == BRANCH_SENTINAL || current_producer_cursor < PayloadBegin() || current_producer_cursor >= GetPayloadEnd())
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
        APCManagerPtr_ = pointer_of_global_apc_manager;
        if (AdaptiveBackoffOfAPCPtr_ == nullptr && APCManagerPtr_ != nullptr)
        {
            AdaptiveBackoffOfAPCPtr_ = &APCManagerPtr_->GetManagersAdaptiveBackoff();
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
        BranchCapacity_ = container_capacity;
        packed64_t idle_cell = PackedCell64_t::MakeInitialPacked(container_cfg.InitialMode);
        for (size_t i = 0; i < container_capacity; i++)
        {
            BackingPtr[i].store(idle_cell, MoStoreUnSeq_);
        }

        try
        {
            if (APCManagerPtr_)
            {
                APCManagerPtr_->StartAPCManager();
                AdaptiveBackoffOfAPCPtr_ = &APCManagerPtr_->GetManagersAdaptiveBackoff();
            }
            else
            {
                AdaptiveBackoffOfAPCPtr_ = nullptr;
            }
            OwnedMasterClockConfPtr_ = std::make_unique<MasterClockConf>(this, LocalTimer48_);
            if (AdaptiveBackoffOfAPCPtr_ )
            {
                AdaptiveBackoffOfAPCPtr_->AttachMasterClockToAadaptiveBackOff(OwnedMasterClockConfPtr_.get());
            }
        }
        catch(...)
        {
            AdaptiveBackoffOfAPCPtr_ = nullptr;
            OwnedMasterClockConfPtr_.reset();
            delete[] BackingPtr;
            BackingPtr = nullptr;
            throw;
        }
        
        const uint32_t new_branch_id = GlobalBranchIdAlloc_.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t logical_node_id = new_branch_id;
        const uint32_t shared_id = new_branch_id;
        container_cfg.NodeGroupSize = 1u;

        InitRootOrChildBranch(
            new_branch_id,
            logical_node_id,
            shared_id,
            container_capacity,
            container_cfg,
            true,
            APCNodeComputeKind::NONE,
            NO_VAL,
            NO_VAL
        );
        InitZeroState_();
        if (container_cfg.RegionSize > 0)
        {
            InitRegionIdx(container_cfg.RegionSize);
        }
        if (APCManagerPtr_)
        {
            APCManagerPtr_->RegisterAPCFromManager_(this);
        }
        RefreshAPCMeta_();
    }

    void AdaptivePackedCellContainer::InitAPCAsNode(
        size_t capacity,
        const ContainerConf& container_configuration,
        APCNodeComputeKind compute_kind,
        uint32_t aux_param_u32
    )
    {
        InitOwned(capacity, container_configuration);
        InitNodeSemantics(compute_kind, aux_param_u32);
        SetGraphNodeFlag();
    }


    void AdaptivePackedCellContainer::InitRegionIdx(size_t region_size) noexcept
    {
        if (!IfAPCBranchValid() || region_size == 0)
        {
            return;
        }
        const uint32_t current_region_size = ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_SIZE);
        if (!JustUpdateValueOfMeta32(
            MetaIndexOfAPCNode::REGION_SIZE,
            current_region_size,
            static_cast<uint32_t>(region_size)
        ))
        {
            return;
        }
        const uint32_t region_count = static_cast<uint32_t>((PayloadCapacityFromHeader() + region_size - 1u) / region_size);
        const uint32_t current_region_count = ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_COUNT);
        if (!JustUpdateValueOfMeta32(
            MetaIndexOfAPCNode::REGION_COUNT,
            current_region_count,
            region_count
        ))
        {
            return;
        }
        RebuildRegionIndexFromPayload_();
        TurnOnASegmentFlag(ControlEnumOfAPCSegment::HAS_REGION_INDEX);
    }

    size_t AdaptivePackedCellContainer::NextProducerSequence() noexcept
    {
        if (!IfAPCBranchValid())
        {
            return SIZE_MAX;
        }

        struct ProducerBlockCacheTLS
        {
            const AdaptivePackedCellContainer* OwnerOfNode = nullptr;
            size_t BlockBase = 0;
            size_t BlockLeft = 0;
        };

        thread_local ProducerBlockCacheTLS cache{};
        if (cache.OwnerOfNode != this)
        {
            cache.OwnerOfNode = this;
            cache.BlockBase = 0;
            cache.BlockLeft = 0;
        }

        const size_t current_block_size = static_cast<size_t>(ReadMetaCellValue32(MetaIndexOfAPCNode::PRODUCER_BLOCK_SIZE));
        if (cache.BlockLeft == 0)
        {
            const size_t block = std::min<size_t>(current_block_size, PayloadCapacityFromHeader());
            const size_t base = ReserveProducerSlots(block);
            if (base == SIZE_MAX)
            {
                return SIZE_MAX;
            }
            cache.BlockBase = base;
            cache.BlockLeft = block;
        }
        const size_t sequence = cache.BlockBase++;
        --cache.BlockLeft;
        return sequence;
    }



    void AdaptivePackedCellContainer::TryCreateBranchIfNeeded(APCPagedNodeRelMaskClasses rel_mask_hint) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return;
        }
        
        if (!HasThisControlEnumFlag(
            ControlEnumOfAPCSegment::ENABLE_BRANCHING
        ))
        {
            return;
        }

        if(!ShouldSplitNow())
        {
            return;
        }

        AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(rel_mask_hint);
        if (grown_apc)
        {
            APCManagerPtr_->RegisterAPCFromManager_(grown_apc);
        }
        
    }
    
    size_t AdaptivePackedCellContainer::OccupancyAddOrSubAndGetAfterChange(int delta) noexcept
    {

        if (delta == 0)
        {
            return static_cast<size_t>(ReadMetaCellValue32(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT));
        }
        while (true)
        {
            packed64_t current_occupancy_cell = ReadFullMetaCell(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT);
            val32_t current_occupancy = PackedCell64_t::ExtractValue32(current_occupancy_cell);
            if (current_occupancy == BRANCH_SENTINAL)
            {
                return BRANCH_SENTINAL;
            }
            
            int64_t next_occupancy_winded = static_cast<int64_t>(current_occupancy) + static_cast<int64_t>(delta);
            if (next_occupancy_winded < 0)
            {
                next_occupancy_winded = 0;
            }
            constexpr int64_t high_val = static_cast<int64_t>(BRANCH_SENTINAL - 1u);
            if (next_occupancy_winded > high_val)
            {
                next_occupancy_winded = high_val;
            }
            
            uint32_t next_occupancy = static_cast<uint32_t>(next_occupancy_winded);
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT, current_occupancy, next_occupancy))
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
            if (!current_apc_ptr)
            {
                break;
            }
            const uint32_t previous_id = current_apc_ptr->ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_PREVIOUS_ID);
            if (previous_id == NO_VAL || previous_id == BRANCH_SENTINAL)
            {
                break;
            }
            AdaptivePackedCellContainer* previous_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(previous_id);
            if (!previous_apc_ptr || previous_apc_ptr == current_apc_ptr || !previous_apc_ptr->IfAPCBranchValid())
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

        const uint32_t next_apc_id = ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_NEXT_ID);

        if (next_apc_id == NO_VAL || next_apc_id == BRANCH_SENTINAL)
        {
            return nullptr;
        }
        AdaptivePackedCellContainer* next_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(next_apc_id);
        if (!next_apc_ptr || next_apc_ptr == this)
        {
            return nullptr;
        }
        return next_apc_ptr;
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
            uint32_t next_apc_id = current_apc_ptr->ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_NEXT_ID);
            if (next_apc_id == NO_VAL || next_apc_id == BRANCH_SENTINAL)
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
        PublishResult local_result = PublishCellByRegionMAskTraverseStartsFromThisAPC(region_kind, packed_cell);
        if (local_result.ResultStatus == PublishStatus::OK)
        {
            return true;
        }

        auto full_layout = ReadAndGetFullRegionLayout_();
        const uint32_t grow_amount = SuggestedInternalAPCExpension_(full_layout ? &(*full_layout) : nullptr, 50);
        if (grow_amount > 0 && TryExtendASegmentInOwnAPC(
            region_kind,
            grow_amount,
            ContainerConf::APCSegmentExtendOrder::PRIORITY
        ))
        {
            local_result = PublishCellByRegionMAskTraverseStartsFromThisAPC(region_kind, packed_cell);
            if (local_result.ResultStatus == PublishStatus::OK)
            {
                return true;
            }
        }

        AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(region_kind, true);
        if (grown_apc)
        {
            if (growth_counter)
            {
                growth_counter->fetch_add(1, std::memory_order_relaxed);
            }
            const PublishResult ok = grown_apc->PublishCellByRegionMAskTraverseStartsFromThisAPC(region_kind, packed_cell);
            if (ok.ResultStatus == PublishStatus::OK)
            {
                return true;
            }
        }
        return false;
    }


    std::optional<packed64_t> AdaptivePackedCellContainer::ConsumeCellByRegionMaskTraverseStartFromThisAPC(APCPagedNodeRelMaskClasses region_kind, size_t& scan_cursor) noexcept
    {
        if (!IfAPCBranchValid())
        {
            return std::nullopt;
        }
        AdaptivePackedCellContainer* root_apc_ptr = FindSharedRootOrThis();
        if (!root_apc_ptr)
        {
            return std::nullopt;
        }
        AdaptivePackedCellContainer* current_apc_ptr = root_apc_ptr;
        bool first = true;
        while (current_apc_ptr)
        {
            size_t local_cursor = first ? scan_cursor : PayloadBegin();
            auto maybe_cell = current_apc_ptr->TryConsumeAndIdleFromRegionLocal_(region_kind, local_cursor);
            if (maybe_cell)
            {
                if (first)
                {
                    scan_cursor = local_cursor;
                }
                return *maybe_cell;
            }
            current_apc_ptr = current_apc_ptr->GetNextSharedSegment();
            first = false;
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
        if (ShouldSplitNow())
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

        if (!HasThisControlEnumFlag(ControlEnumOfAPCSegment::ENABLE_BRANCHING))
        {
            return nullptr;
        }

        if (!TryMarkSplitInFlight())
        {
            return nullptr;
        }

        auto ClearSplitFlag = [this]() noexcept
        {
            ClearOneControlEnumFlagOfAPC(
                ControlEnumOfAPCSegment::SPLIT_INFLIGHT
            );
        };

        AdaptivePackedCellContainer* root_apc_ptr = FindSharedRootOrThis();
        if (!root_apc_ptr)
        {
            ClearSplitFlag();
            return nullptr;
        }
        const uint32_t root_branch_id = root_apc_ptr->GetBranchId();
        const uint32_t root_logical_id = root_apc_ptr->GetLogicalId();
        const uint32_t shared_group_id = (root_apc_ptr->GetSharedId() == NO_VAL || root_apc_ptr->GetSharedId() == BRANCH_SENTINAL) ?
                                            root_branch_id : root_apc_ptr->GetSharedId();
        const uint32_t parents_depth = CurrentBranchDepthRead();
        const uint32_t child_depth = parents_depth + 1u;
        const uint32_t max_depth = MaxDepthRead();
        if (child_depth > max_depth)
        {
            ClearSplitFlag();
            return nullptr;
        }
        
        ContainerConf child_configuration{};
        child_configuration.InitialMode = static_cast<PackedMode>(ReadMetaCellValue32(MetaIndexOfAPCNode::DEFINED_MODE_OF_CURRENT_APC));
        child_configuration.ProducerBlockSize = static_cast<size_t>(ReadMetaCellValue32(MetaIndexOfAPCNode::PRODUCER_BLOCK_SIZE));
        child_configuration.RegionSize = static_cast<size_t>(ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_SIZE));
        child_configuration.RetireBatchThreshold = ReadMetaCellValue32(MetaIndexOfAPCNode::RETIRE_BRANCH_THRASHOLD);
        child_configuration.BackgroundEpochAdvanceMS = ReadMetaCellValue32(MetaIndexOfAPCNode::BACKGROUND_EPOCH_ADVANCE_MS);
        child_configuration.EnableBranching = enable_recursive_branching;
        child_configuration.BranchSplitThresholdPercentage = ReadMetaCellValue32(MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE);
        child_configuration.BranchMaxDepth = ReadMetaCellValue32(MetaIndexOfAPCNode::MAX_DEPTH);
        child_configuration.BranchMinChildCapacity = SuggestedChildCapacity_();

        AdaptivePackedCellContainer* new_child_segment_ptr = nullptr;
        try
        {
            new_child_segment_ptr = new AdaptivePackedCellContainer();
            new_child_segment_ptr->SetManagerForGlobalAPC(APCManagerPtr_);
            new_child_segment_ptr->InitOwned(child_configuration.BranchMinChildCapacity, child_configuration);
        }
        catch(...)
        {
            ClearSplitFlag();
            return nullptr;
        }

        if (!new_child_segment_ptr)
        {
            if (new_child_segment_ptr)
            {
                new_child_segment_ptr->FreeAll();
                delete new_child_segment_ptr;
            }
            ClearSplitFlag();
            return nullptr;
        }
        
        const uint32_t new_child_branch_id = new_child_segment_ptr->GetBranchId();
        if (new_child_branch_id == NO_VAL || new_child_branch_id == BRANCH_SENTINAL || new_child_branch_id == root_branch_id)
        {
            new_child_segment_ptr->FreeAll();
            delete new_child_segment_ptr;
            ClearSplitFlag();
            return nullptr;
        }
        
        new_child_segment_ptr->InitRootOrChildBranch(
            new_child_branch_id,
            root_logical_id,
            shared_group_id,
            new_child_segment_ptr->GetPayloadEnd(),
            child_configuration,
            false,
            static_cast<APCNodeComputeKind>(
                root_apc_ptr->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_COMPUTE_KIND)
            ),
            root_apc_ptr->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_AUX_PARAM_U32),
            child_depth,
            static_cast<uint8_t>(root_apc_ptr->ReadMetaCellValue32(MetaIndexOfAPCNode::BRANCH_PRIORITY))
        );

        new_child_segment_ptr->SetSegmentRegionKind(desired_region_kind);
        auto CopyBranchSagmentMeta = [&](MetaIndexOfAPCNode idx) noexcept
        {
            const uint32_t root_src = root_apc_ptr->ReadMetaCellValue32(idx);
            const uint32_t child_dest = new_child_segment_ptr->ReadMetaCellValue32(idx);
            new_child_segment_ptr->JustUpdateValueOfMeta32(idx, child_dest, root_src);
        };

        CopyBranchSagmentMeta(MetaIndexOfAPCNode::FEEDFORWARD_IN_TARGET_ID);
        CopyBranchSagmentMeta(MetaIndexOfAPCNode::FEEDFORWARD_OUT_TARGET_ID);
        CopyBranchSagmentMeta(MetaIndexOfAPCNode::FEEDBACKWARD_IN_TARGET_ID);
        CopyBranchSagmentMeta(MetaIndexOfAPCNode::FEEDBACKWARD_OUT_TARGET_ID);
        CopyBranchSagmentMeta(MetaIndexOfAPCNode::LATERAL_0_TARGET_ID);
        CopyBranchSagmentMeta(MetaIndexOfAPCNode::LATERAL_1_TARGET_ID);

        AdaptivePackedCellContainer* tail_apc_ptr = root_apc_ptr;
        while (tail_apc_ptr->GetNextSharedSegment())
        {
            tail_apc_ptr = tail_apc_ptr->GetNextSharedSegment();
        }
        if (
            !tail_apc_ptr->TryBindShareNext(new_child_branch_id) || 
            !new_child_segment_ptr->TryBindSharedPrevious(tail_apc_ptr->GetBranchId())
        )
        {
            new_child_segment_ptr->FreeAll();
            delete new_child_segment_ptr;
            ClearSplitFlag();
            return nullptr;
        }
        tail_apc_ptr->TurnOnASegmentFlag(
            ControlEnumOfAPCSegment::HAS_SHARED_NEXT
        );
        new_child_segment_ptr->TurnOnASegmentFlag(
            ControlEnumOfAPCSegment::HAS_SHARED_PREVIOUS
        );
        if (!root_apc_ptr->RebuildSharedChainSegmentMetatdataFromRoot_())
        {
            new_child_segment_ptr->FreeAll();
            delete new_child_segment_ptr;
            ClearSplitFlag();
            return nullptr;
        }
        root_apc_ptr->RefreshAPCMeta_();
        new_child_segment_ptr->RefreshAPCMeta_();
        ClearSplitFlag();
        return new_child_segment_ptr;
    }

    bool AdaptivePackedCellContainer::RebuildSharedChainSegmentMetatdataFromRoot_() noexcept
    {
        AdaptivePackedCellContainer* root_apc_ptr = FindSharedRootOrThis();
        
        const uint32_t shared_group_id = (
            root_apc_ptr->GetSharedId() == NO_VAL || root_apc_ptr->GetSharedId() == BRANCH_SENTINAL
        ) ? root_apc_ptr->GetBranchId() : root_apc_ptr->GetSharedId();
        std::vector<AdaptivePackedCellContainer*> apc_chain;
        AdaptivePackedCellContainer* current_apc = root_apc_ptr;
        while (current_apc)
        {
            apc_chain.push_back(current_apc);
            current_apc = current_apc->GetNextSharedSegment();
        }
        const uint32_t group_size = static_cast<uint32_t>(apc_chain.size());
        for (size_t i = 0; i < apc_chain.size(); i++)
        {
            AdaptivePackedCellContainer* current_chain_index_apc = apc_chain[i];
            const uint32_t expected_group_size = current_chain_index_apc->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_GROUP_SIZE);
            current_chain_index_apc->JustUpdateValueOfMeta32(MetaIndexOfAPCNode::NODE_GROUP_SIZE, expected_group_size, group_size);

            const uint32_t expected_shared_id = current_chain_index_apc->ReadMetaCellValue32(MetaIndexOfAPCNode::SHARED_ID);
            current_chain_index_apc->JustUpdateValueOfMeta32(MetaIndexOfAPCNode::SHARED_ID, expected_shared_id, shared_group_id);

            const uint32_t previous_id = (i == 0) ? BRANCH_SENTINAL : apc_chain[i - 1]->GetBranchId();
            const uint32_t next_id = (i + 1 < apc_chain.size()) ? apc_chain[i + 1]->GetBranchId() : BRANCH_SENTINAL;
            current_chain_index_apc->TryBindSharedPrevious(previous_id);
            current_chain_index_apc->TryBindShareNext(next_id);
            if (i == 0)
            {
                current_chain_index_apc->TurnOnASegmentFlag(ControlEnumOfAPCSegment::IS_SHARED_ROOT);
                current_chain_index_apc->ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment::IS_SHARED_MAMBER);
            }
            else
            {
                current_chain_index_apc->TurnOnASegmentFlag(ControlEnumOfAPCSegment::IS_SHARED_MAMBER);
                current_chain_index_apc->ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment::IS_SHARED_ROOT);
                
            }
            
            if (previous_id == BRANCH_SENTINAL)
            {
                current_chain_index_apc->ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment::HAS_SHARED_PREVIOUS);
            }
            else
            {
                current_chain_index_apc->TurnOnASegmentFlag(ControlEnumOfAPCSegment::HAS_SHARED_PREVIOUS);
            }

            if (next_id == BRANCH_SENTINAL)
            {
                current_chain_index_apc->ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment::HAS_SHARED_NEXT);
            }
            else
            {
                current_chain_index_apc->TurnOnASegmentFlag(ControlEnumOfAPCSegment::HAS_SHARED_NEXT);
            }
        }
        return true;
    }

    void AdaptivePackedCellContainer::ClearAllManagerLinksAndFlags() noexcept
    {
        RegistryNextAPCPtr_.store(nullptr, MoStoreSeq_);
        WorkNextAPCPtr_.store(nullptr, MoStoreSeq_);
        CleanupNextAPCPtr_.store(nullptr, MoStoreSeq_);
        if (!BackingPtr || !OwnedMasterClockConfPtr_)
        {
            return;
        }
        
        const packed64_t idle = OwnedMasterClockConfPtr_->ComposeValue32WithCurrentThreadStamp16(NO_VAL);
        BackingPtr[static_cast<size_t>(MetaIndexOfAPCNode::MANAGER_CONTROL_FLAGS)].store(idle, MoStoreSeq_);
    }


    void AdaptivePackedCellContainer::FreeAll() noexcept
    {
        if (!BackingPtr && !OwnedMasterClockConfPtr_)
        {
            return;
        }
        if (APCManagerPtr_)
        {
            APCManagerPtr_->UnRegisterAPCFromManager_(this);
        }
        
        
        ClearAllManagerLinksAndFlags();

        AdaptiveBackoffOfAPCPtr_ = nullptr;
        OwnedMasterClockConfPtr_.reset();   
        delete[] BackingPtr;
        BackingPtr = nullptr;
        BranchCapacity_ = 0;
        RegionRelArray_.reset();
        RegionEpochArray_.reset();
        RelBitmaps_.clear();
    }

    // uint32_t AdaptivePackedCellContainer::CountLocalExactOccupancy(
    //     APCOccupancyQuery query,
    //     APCPagedNodeRelMaskClasses region_class = APCPagedNodeRelMaskClasses::NANNULL
    // ) noexcept
    // {
    //     if (!IfAPCBranchValid())
    //     {
    //         return NO_VAL;
    //     }

    //     std::optional<LayoutBoundsOfSingleRelNodeClass> maybe_bounds_of_desired_region;
    //     if (region_class != APCPagedNodeRelMaskClasses::NANNULL)
    //     {
    //         maybe_bounds_of_desired_region = ReadLayoutBounds(region_class);
    //         if (!maybe_bounds_of_desired_region || maybe_bounds_of_desired_region->IsEmpty())
    //         {
    //             return NO_VAL;
    //         }
    //     }
        
    //     const size_t begin_idx = maybe_bounds_of_desired_region->BeginIndex;
    //     const size_t end_idx = maybe_bounds_of_desired_region->EndIndex;

        
    // }

}
