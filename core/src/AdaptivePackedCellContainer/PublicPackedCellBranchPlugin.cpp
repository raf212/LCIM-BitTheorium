#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{

    val32_t PackedCellBranchPlugin::ReadMetaCellValue32(MetaIndexOfAPCNode idx) noexcept
    {
        if (!ValidMeteIdx(idx) || idx == MetaIndexOfAPCNode::LOCAL_CLOCK48)
        {
            return NO_VAL;
        }
        size_t index = static_cast<size_t>(idx);
        return PackedCell64_t::ExtractValue32(PackedCellContainerPtr_[index].load(MoLoad_));
    }

    void PackedCellBranchPlugin::TouchLocalMetaClock48(packed64_t* updated_full_clock_cell_easy_return_ptr) noexcept
    {
        if (!MasterClockConfPtr_)
        {
            return;
        }
        MasterClockConfPtr_->TouchAtomicPackedCellClockForCurrentThread(
            PackedCellContainerPtr_[static_cast<size_t>(MetaIndexOfAPCNode::LOCAL_CLOCK48)], updated_full_clock_cell_easy_return_ptr
        );
    }

    packed64_t PackedCellBranchPlugin::PackPureClock48AsPackedCell(
        std::optional<uint64_t> clock48,
        tag8_t priority,
        PackedCellLocalityTypes locality,
        tag8_t rel_mask,
        RelOffsetMode48 reloffset,
        PackedCellDataType dtype
    ) noexcept
    {
        if ((reloffset != RelOffsetMode48::RELOFFSET_PURE_TIMER))
        {
            return PackedCell64_t::ComposeCLK48u_64(NO_VAL, MakeStrl4ForMode48_t(MAX_PRIORITY, PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY, rel_mask, reloffset, dtype));
        }
        
        if (MasterClockConfPtr_)
        {
            size_t master_clock_slot_id = MasterClockConfPtr_->EnsureOrAssignThreadIdForMasterClock();
            if (master_clock_slot_id != SIZE_MAX)
            {
                return MasterClockConfPtr_->ComposeClockCell48WithMasterClock(master_clock_slot_id, clock48, rel_mask, priority, locality, reloffset, dtype);
            }
        }
        
        strl16_t strl_clock48 = MakeStrl4ForMode48_t(priority, locality, rel_mask, reloffset, dtype);
        if (clock48)
        {
            return PackedCell64_t::ComposeCLK48u_64(clock48.value(), strl_clock48);
        }
        Timer48 now_timer;
        return PackedCell64_t::ComposeCLK48u_64((now_timer.NowTicks() & MaskBits(CLK_B48)), strl_clock48);
    }

    void PackedCellBranchPlugin::WriteOrUpdateMetaClock48(tag8_t priority, std::optional<uint64_t>meta_clock_48 ) noexcept
    {
        size_t idx = static_cast<size_t>(MetaIndexOfAPCNode::LOCAL_CLOCK48);
        packed64_t wanted_cell = PackPureClock48AsPackedCell(meta_clock_48, priority, PackedCellLocalityTypes::ST_PUBLISHED);
        PackedCellContainerPtr_[idx].store(wanted_cell, MoStoreSeq_);
        PackedCellContainerPtr_[idx].notify_all();
    }

    bool PackedCellBranchPlugin::JustUpdateValueOfMeta32(
        MetaIndexOfAPCNode idx,
        uint32_t expected_value,
        uint32_t desired_value,
        bool refresh_clock16
    ) noexcept
    {
        if (!ValidMeteIdx(idx) || idx == MetaIndexOfAPCNode::LOCAL_CLOCK48)
        {
            return false;
        }
        const size_t index = static_cast<size_t>(idx);
        packed64_t expected_packed = PackedCellContainerPtr_[index].load(MoLoad_);
        if (PackedCell64_t::ExtractValue32(expected_packed) != expected_value)
        {
            return false;
        }
        if (PackedCell64_t::ExtractLocalityFromPacked(expected_packed) == PackedCellLocalityTypes::ST_CLAIMED)
        {
            return false;
        }
        strl16_t current_strl = PackedCell64_t::ExtractSTRL(expected_packed);
        clk16_t current_clock16 = PackedCell64_t::ExtractClk16(expected_packed);
        packed64_t desired_packed = PackedCell64_t::ComposeValue32u_64(desired_value, current_clock16, current_strl);
        if (refresh_clock16 && MasterClockConfPtr_)
        {
            desired_packed = MasterClockConfPtr_->ComposeValue32WithCurrentThreadStamp16(
                desired_value,
                PackedCell64_t::ExtractRelMaskFromPacked(expected_packed),
                PackedCell64_t::ExtractPriorityFromPacked(expected_packed),
                PackedCell64_t::ExtractLocalityFromPacked(expected_packed),
                static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(expected_packed)),
                PackedCell64_t::ExtractPCellDataTypeFromPacked(expected_packed)
            );
        }
        
        return PackedCellContainerPtr_[index].compare_exchange_strong(
            expected_packed,
            desired_packed,
            OnExchangeSuccess,
            OnExchangeFailure
        );
        
    }

    void PackedCellBranchPlugin::InitLogicalNodeIdentity(
        uint32_t logical_node_id,
        uint32_t shared_id,
        bool is_root_shared
    ) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LOGICAL_NODE_ID, logical_node_id, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::SHARED_ID, shared_id, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::SHARED_PREVIOUS_ID, BRANCH_SENTINAL, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::SHARED_NEXT_ID, BRANCH_SENTINAL, ZERO_PRIORITY);
        if (is_root_shared)
        {
            TurnOnMultipleSegmentFlagsAtOnce_(static_cast<uint32_t>(ControlEnumOfAPCSegment::IS_GRAPH_NODE) | static_cast<uint32_t>(ControlEnumOfAPCSegment::IS_SHARED_ROOT));
            ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment::IS_SHARED_MAMBER);
        }
        else
        {
            TurnOnMultipleSegmentFlagsAtOnce_(static_cast<uint32_t>(ControlEnumOfAPCSegment::IS_GRAPH_NODE) | static_cast<uint32_t>(ControlEnumOfAPCSegment::IS_SHARED_MAMBER));
            ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment::IS_SHARED_ROOT);    
        }
        
    }

    void PackedCellBranchPlugin::InitNodeSemantics(
        APCNodeComputeKind compute_kind_of_node,
        uint32_t aux_param_uint32
    ) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::NODE_COMPUTE_KIND, static_cast<uint32_t>(compute_kind_of_node), ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::NODE_AUX_PARAM_U32, aux_param_uint32, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_FORWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_BACKWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_EMITTED_FEED_FORWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_EMITTED_FEED_BACKWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDFORWARD_IN_TARGET_ID, BRANCH_SENTINAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDFORWARD_OUT_TARGET_ID, BRANCH_SENTINAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDBACKWARD_IN_TARGET_ID, BRANCH_SENTINAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDBACKWARD_OUT_TARGET_ID, BRANCH_SENTINAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LATERAL_0_TARGET_ID, BRANCH_SENTINAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LATERAL_1_TARGET_ID, BRANCH_SENTINAL);
    }



    void PackedCellBranchPlugin::InitRootOrChildBranch(
        uint32_t branch_id,
        uint32_t logical_node_id,
        uint32_t shared_id,
        size_t total_capacity,
        const ContainerConf& container_configuration,
        bool is_root_shared,
        APCNodeComputeKind node_compute_kind,
        uint32_t aux_param_uint32,
        uint32_t branch_depth,
        uint8_t branch_priority,
        uint8_t write_cell_priority

    ) noexcept
    {
        if (!IsBound())
        {
            return;
        }

        const uint32_t region_count = container_configuration.RegionSize == 0 ? 0 : static_cast<uint32_t>((std::max<size_t>(total_capacity, METACELL_COUNT) - METACELL_COUNT + container_configuration.RegionSize - 1) / container_configuration.RegionSize);
        
        WriteBrenchMeta32_(MetaIndexOfAPCNode::MAGIC_ID, BRANCH_MAGIC, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::VERSION, BRANCH_VERSION, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CAPACITY, static_cast<uint32_t>(std::min<size_t>(total_capacity, BRANCH_SENTINAL)), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::BRANCH_ID, static_cast<uint32_t>(std::min<uint32_t>(branch_id, BRANCH_SENTINAL)), write_cell_priority);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::BRANCH_DEPTH, branch_depth, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::BRANCH_PRIORITY, branch_priority, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS, container_configuration.EnableBranching ? static_cast<uint32_t>(ControlEnumOfAPCSegment::ENABLE_BRANCHING) : static_cast<uint32_t>(ControlEnumOfAPCSegment::NONE), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CURRENT_ACTIVE_THREADS, NO_VAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT, NO_VAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE, container_configuration.BranchSplitThresholdPercentage, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::MAX_DEPTH, container_configuration.BranchMaxDepth, write_cell_priority);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::PAYLOAD_END, static_cast<uint32_t>(std::min<size_t>(total_capacity, BRANCH_SENTINAL)), write_cell_priority);

        WriteOrUpdateMetaClock48(write_cell_priority, NO_VAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_SPLIT_EPOCH, NO_VAL, write_cell_priority);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::REGION_SIZE, static_cast<uint32_t>(container_configuration.RegionSize), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::REGION_COUNT, region_count, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::READY_REL_MASK, NO_VAL, write_cell_priority);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::PRODUCER_BLOCK_SIZE, static_cast<uint32_t>(container_configuration.ProducerBlockSize), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::BACKGROUND_EPOCH_ADVANCE_MS, static_cast<uint32_t>(container_configuration.BackgroundEpochAdvanceMS), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::DEFINED_MODE_OF_CURRENT_APC, static_cast<uint32_t>(container_configuration.InitialMode), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::RETIRE_BRANCH_THRASHOLD, container_configuration.RetireBatchThreshold, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT, static_cast<uint32_t>(METACELL_COUNT), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CONSUMER_CURSORE_PLACEMENT, static_cast<uint32_t>(METACELL_COUNT), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CURRENTLY_OWNED, NO_VAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, NO_VAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::NODE_GROUP_SIZE, container_configuration.NodeGroupSize, write_cell_priority);

        ////-----/////
        InitLogicalNodeIdentity(logical_node_id, shared_id, is_root_shared);
        InitNodeSemantics(node_compute_kind, aux_param_uint32);
        InitDefaultAPCSegmentedNodeLayout_();
        WriteBrenchMeta32_(MetaIndexOfAPCNode::EOF_APC_HEADER, EOF_HEADER, write_cell_priority);
    }

    bool PackedCellBranchPlugin::TryIncrementOrDecrementActiveThreadCount(int8_t change_count) noexcept
    {
        ///for now
        if (change_count < 0)
        {
            change_count = -1;
        }
        else if (change_count > 0)
        {
            change_count = 1;
        }
        ///
        
        
        while (true)
        {
            uint32_t current_thread_count = ReadMetaCellValue32(MetaIndexOfAPCNode::CURRENT_ACTIVE_THREADS);
            if (current_thread_count == UINT32_MAX)
            {
                return false;
            }
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::CURRENT_ACTIVE_THREADS, current_thread_count, current_thread_count + change_count))
            {
                return true;
            }
        }
    }

    bool PackedCellBranchPlugin::TryBindPortTarget(MetaIndexOfAPCNode port_meta_idx, uint32_t target_branch_id) noexcept
    {
        if (target_branch_id == BRANCH_SENTINAL)
        {
            return false;
        }
        while (true)
        {
            const uint32_t current_meta_value = ReadMetaCellValue32(port_meta_idx);
            if (current_meta_value == target_branch_id)
            {
                return true;
            }
            if (current_meta_value != BRANCH_SENTINAL)
            {
                return false;
            }
            if (JustUpdateValueOfMeta32(port_meta_idx, current_meta_value, target_branch_id))
            {
                return true;
            }
        }
    }

    bool PackedCellBranchPlugin::ShouldSplitNow() noexcept
    {
        const val32_t split_threshold = ReadMetaCellValue32(MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE);
        const val32_t current_occumancy = ReadMetaCellValue32(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT);
        const val32_t max_depth = ReadMetaCellValue32(MetaIndexOfAPCNode::MAX_DEPTH);
        const val32_t depth_of_current_branch = ReadMetaCellValue32(MetaIndexOfAPCNode::BRANCH_DEPTH);
        if (depth_of_current_branch >= max_depth)
        {
            return false;
        }
        const size_t payload_capacity = PayloadCapacity();
        if (payload_capacity == 0)
        {
            return false;
        }
        
        return ((static_cast<uint64_t>(current_occumancy) * 100u) / payload_capacity) >= split_threshold;
        
    }

    bool PackedCellBranchPlugin::TryMarkSplitInFlight() noexcept
    {
        while (true)
        {
            const uint32_t current_flags = ReadMetaCellValue32(MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS);
            if (current_flags == BRANCH_SENTINAL)
            {
                return false;
            }

            bool is_already_in_flight = HasThisFlag(ControlEnumOfAPCSegment::SPLIT_INFLIGHT);
            if (is_already_in_flight)
            {
                return false;
            }
            return TurnOnASegmentFlag(ControlEnumOfAPCSegment::SPLIT_INFLIGHT);
        }
    }

    uint32_t PackedCellBranchPlugin::TotalCASFailForThisBranchIncreaseAndGet(uint32_t increment) noexcept
    {
        while (true)
        {
            val32_t current_total_cas_failure = ReadMetaCellValue32(MetaIndexOfAPCNode::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH);
            if (current_total_cas_failure == BRANCH_SENTINAL)
            {
                return BRANCH_SENTINAL;
            }
            
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, current_total_cas_failure, current_total_cas_failure + increment))
            {
                return current_total_cas_failure + increment;
            }   
        }
    }




    std::optional<LayoutBoundsOfSingleRelNodeClass> PackedCellBranchPlugin::ReadLayoutBounds(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept
    {
        auto maybe_begin_end = GetMetaBoundsPairForRegionMask_(desired_rel_mask);
        if (!maybe_begin_end)
        {
            return std::nullopt;
        }
        const auto [begin_meta, end_meta] = *maybe_begin_end;
        LayoutBoundsOfSingleRelNodeClass current_bounds{};
        current_bounds.BeginIndex = ReadMetaCellValue32(begin_meta);
        current_bounds.EndIndex = ReadMetaCellValue32(end_meta);
        current_bounds.LAYOUT_CLASS = desired_rel_mask;
        current_bounds.SetOrResetPercentage(ReadMetaCellValue32(MetaIndexOfAPCNode::PAYLOAD_END) - METACELL_COUNT);        
        return current_bounds;
    }

    bool PackedCellBranchPlugin::SetLayOutBounds(APCPagedNodeRelMaskClasses desired_rel_mask, uint32_t begin, uint32_t end) noexcept
    {
        if (begin > end || begin < METACELL_COUNT || end > PayloadEndRead())
        {
            return false;
        }

        auto maybe_begain_end = GetMetaBoundsPairForRegionMask_(desired_rel_mask);
        if (!maybe_begain_end)
        {
            return false;
        }

        // std::pair begin_end = *maybe_begain_end; kept for learning

        const auto [begin_meta, end_meta] = *maybe_begain_end;

        const uint32_t current_begain = ReadMetaCellValue32(begin_meta);
        const uint32_t current_end = ReadMetaCellValue32(end_meta);

        return JustUpdateValueOfMeta32(begin_meta, current_begain, begin) && JustUpdateValueOfMeta32(end_meta, current_end, end);
        
    }

    bool PackedCellBranchPlugin::TrySetLayoutMutationInFlight() noexcept
    {
        while (true)
        {
            const uint32_t current_flags = ReadMetaCellValue32(MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS);
            if (current_flags == BRANCH_SENTINAL)
            {
                return false;
            }

            if (HasThisFlag(ControlEnumOfAPCSegment::LAYOUT_MUTATION_INFLIGHT))
            {
                return false;
            }
            return TurnOnASegmentFlag(ControlEnumOfAPCSegment::LAYOUT_MUTATION_INFLIGHT);            
        }
    }

    // bool PackedCellBranchPlugin::TryExtendASegmentInOwnAPC(APCPagedNodeRelMaskClasses desired_rel_mask, uint32_t wanted_amount, ContainerConf::APCSegmentExtendOrder desired_apc_order) noexcept
    // {
    //     if (wanted_amount == 0)
    //     {
    //         return true;
    //     }
    //     if (!IsBound() || desired_rel_mask == APCPagedNodeRelMaskClasses::NANNULL)
    //     {
    //         return false;
    //     }
    //     if (!TrySetLayoutMutationInFlight())
    //     {
    //         return false;
    //     }
        
        
        
    // }


}