#pragma once
#include "PackedCell.hpp"
#include "MasterClockConf.hpp"
#include "APCHelpers.hpp"

namespace PredictedAdaptedEncoding
{


class PackedCellBranchPlugin final
{
public:
    static constexpr size_t METACELL_COUNT = PackedCell64_t::METACELL_COUNT_FIRST;
    static constexpr uint32_t BRANCH_MAGIC = 0x41504342u;//big-endian
    static constexpr uint32_t EOF_HEADER = 0x72616600;//big-endian
    static constexpr uint32_t BRANCH_VERSION = 1u;
    static constexpr uint32_t BRANCH_SENTINAL = LayoutBoundsOfSingleRelNodeClass::BRANCH_SENTINAL;
    static constexpr packed64_t APC_SENTENAL = UINT64_MAX;
    static constexpr uint8_t TOTAL_LAYOUT_SECTION_IN_APC_CONTAINER_NODE = 8;
    
    enum class APCNodeComputeKind : uint32_t
    {
        NONE = 0u,
        GENERATOR_UINT32 = 1u,
        SQUARE_UINT32 = 2u,
        ADD_UINT32 = 3u,
        DIV_UINT32 = 4u,
        BIDIRECTIONAL_PREDECTIVE = 6u,
        GENERIC_VECTOR = 7u
    };


    enum class ControlEnumOfAPCSegment : uint32_t
    {
        NONE = 0u,
        ENABLE_BRANCHING = 1u << 0,
        HAS_REGION_INDEX =  1u << 1,
        SATURATED = 1u << 2,
        SPLIT_INFLIGHT = 1u << 3,
        IS_GRAPH_NODE = 1u << 4,
        IS_SHARED_ROOT = 1u << 5,
        IS_SHARED_MAMBER = 1u << 6,
        HAS_SHARED_NEXT = 1u << 7,
        HAS_SHARED_PREVIOUS = 1u << 8,
        HAS_LAYOUT_DIR = 1u << 9,
        HAS_EDGE_TABLE = 1u << 10,
        HAS_WEIGHT_TABLE = 1u << 11,
        LAYOUT_MUTATION_INFLIGHT = 1u << 12
    };

    static constexpr uint32_t PAYLOAD_BOUND_START = static_cast<uint32_t>(MetaIndexOfAPCNode::MESSAGE_FEEDFORWARD_BEGAIN);
    static constexpr uint32_t PAYLOAD_BOUND_END = static_cast<uint32_t>(MetaIndexOfAPCNode::FREE_END);


    bool ValidMeteIdx(MetaIndexOfAPCNode idx) const noexcept
    {
        return PackedCellContainerPtr_ && static_cast<size_t>(idx) < BranchCapacity_ && static_cast<size_t>(idx) < METACELL_COUNT;
    }

    packed64_t ReadFullMetaCell(MetaIndexOfAPCNode idx) noexcept
    {
        if (ValidMeteIdx(idx))
        {
            return PackedCellContainerPtr_[static_cast<size_t>(idx)].load(MoLoad_);
        }
        return APC_SENTENAL;
    }
    
private:
    std::atomic<packed64_t>* PackedCellContainerPtr_{nullptr};
    size_t BranchCapacity_{0};
    MasterClockConf* MasterClockConfPtr_{nullptr};

    packed64_t PackValue32InPackedCellwithClock16_(
        val32_t value32,
        tag8_t priority,
        PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
        tag8_t rel_mask = REL_NONE,
        RelOffsetMode32 reloffset_mode32 = RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
        PackedCellDataType dtype = PackedCellDataType::UnsignedPCellDataType
    ) noexcept
    {
        if (MasterClockConfPtr_)
        {
            return MasterClockConfPtr_->ComposeValue32WithCurrentThreadStamp16(value32, rel_mask, priority, locality, reloffset_mode32, dtype);
        }
        strl16_t strl_moded32 = MakeSTRLMode32_t(priority, locality, rel_mask, reloffset_mode32, dtype);
        return PackedCell64_t::ComposeValue32u_64(value32, NO_VAL, strl_moded32);
    }

    void WriteBrenchMeta32_(
        MetaIndexOfAPCNode idx,
        uint32_t value32,
        tag8_t priority = ZERO_PRIORITY,
        tag8_t rel_mask4 = REL_MASK4_NONE
    ) noexcept
    {
        size_t index = static_cast<size_t>(idx);
        if (!ValidMeteIdx(idx))
        {
            return;
        }
        PackedCellContainerPtr_[index].store(PackValue32InPackedCellwithClock16_(value32, priority, PackedCellLocalityTypes::ST_PUBLISHED, rel_mask4), MoStoreSeq_);
        PackedCellContainerPtr_[index].notify_all();
    }

    uint32_t ReadAPCModeFlags_() noexcept
    {
        return (ReadMetaCellValue32(MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS));
    }

    bool TurnOnMultipleSegmentFlagsAtOnce_(uint32_t use_or_between_flags = NO_VAL) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(use_or_between_flags);
    }

    bool UpdateAPCModeFlagsInHeader_(uint32_t flags_to_turn_on = NO_VAL, uint32_t flags_to_turn_off = NO_VAL) noexcept;

    std::optional<std::pair<MetaIndexOfAPCNode, MetaIndexOfAPCNode>> GetMetaBoundsPairForRegionMask_(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept;

    bool WriteBoundsPairToHeader_(const LayoutBoundsOfSingleRelNodeClass layout_bound) noexcept;

    void BuidDefaultLayoutPlan_(CompleteAPCNodeRegionsLayout& full_layout) noexcept;

    void InitDefaultAPCSegmentedNodeLayout_() noexcept;

    std::optional<CompleteAPCNodeRegionsLayout> ReadAndGetFullRegionLayout_() noexcept;

    bool WriteAllRegionsLayoutToHeader_(const CompleteAPCNodeRegionsLayout& full_layout) noexcept;

    bool ClearMultipleControlFlags_(uint32_t use_or_between_flags = NO_VAL) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(NO_VAL, use_or_between_flags);
    }
    
public:
    packed64_t PackPureClock48AsPackedCell(
        std::optional<uint64_t> clock48 = std::nullopt,
        tag8_t priority = ZERO_PRIORITY,
        PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
        tag8_t rel_mask = REL_NONE,
        RelOffsetMode48 reloffset = RelOffsetMode48::RELOFFSET_PURE_TIMER,
        PackedCellDataType dtype = PackedCellDataType::UnsignedPCellDataType
    ) noexcept;

    void WriteOrUpdateMetaClock48(tag8_t priority = ZERO_PRIORITY, std::optional<uint64_t>meta_clock_48 = std::nullopt) noexcept;

    bool JustUpdateValueOfMeta32(
        MetaIndexOfAPCNode idx,
        uint32_t expected_value,
        uint32_t desired_value,
        bool refresh_clock16 = true
    ) noexcept;

    PackedCellBranchPlugin() noexcept = default;

    void BindBranchPluginToAPC(std::atomic<packed64_t>* packed_cells, size_t capacity, MasterClockConf* master_clock_ptr) noexcept
    {
        PackedCellContainerPtr_ = packed_cells;
        BranchCapacity_ = capacity;
        MasterClockConfPtr_ = master_clock_ptr;
    }

    bool IsBound() const noexcept
    {
        return PackedCellContainerPtr_ != nullptr && BranchCapacity_ >= METACELL_COUNT;
    }

    size_t PayloadCapacity() const noexcept
    {
        return BranchCapacity_ > METACELL_COUNT ? (BranchCapacity_ - METACELL_COUNT) : 0u;
    }

    //continue here
    void InitLogicalNodeIdentity(
        uint32_t logical_node_id,
        uint32_t shared_id,
        bool is_root_shared
    ) noexcept;

    void InitNodeSemantics(
        APCNodeComputeKind compute_kind_of_node,
        uint32_t aux_param_uint32 = NO_VAL
    ) noexcept;


    void InitRootOrChildBranch(
        uint32_t branch_id,
        uint32_t logical_node_id,
        uint32_t shared_id,
        size_t total_capacity,
        const ContainerConf& container_configuration,
        bool is_root_shared = true,
        APCNodeComputeKind node_compute_kind = APCNodeComputeKind::NONE,
        uint32_t aux_param_uint32 = NO_VAL,
        uint32_t branch_depth = NO_VAL,
        uint8_t branch_priority = ZERO_PRIORITY,
        uint8_t write_cell_priority = ZERO_PRIORITY

    ) noexcept;

    val32_t ReadMetaCellValue32(MetaIndexOfAPCNode idx) noexcept;

    void TouchLocalMetaClock48(packed64_t* updated_full_clock_cell_easy_return_ptr = nullptr) noexcept;

    bool TryIncrementOrDecrementActiveThreadCount(int8_t change_count) noexcept;

    bool TryMarkSplitInFlight() noexcept;

    bool ShouldSplitNow() noexcept;

    bool TryBindPortTarget(MetaIndexOfAPCNode port_meta_idx, uint32_t target_branch_id) noexcept;

    uint32_t TotalCASFailForThisBranchIncreaseAndGet(uint32_t increment) noexcept;

    bool SetLayOutBounds(APCPagedNodeRelMaskClasses desired_rel_mask, uint32_t begain, uint32_t end) noexcept;

    std::optional<LayoutBoundsOfSingleRelNodeClass> ReadLayoutBounds(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept;

    bool TrySetLayoutMutationInFlight() noexcept;

    bool TryExtendASegmentInOwnAPC(APCPagedNodeRelMaskClasses desired_rel_mask, uint32_t wanted_amount, ContainerConf::APCSegmentExtendOrder desired_apc_order) noexcept;


    uint32_t ForceOccupancyUpdateAndReturn(uint32_t new_occupancy) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT, new_occupancy);
        uint32_t updated_occupancy = ReadMetaCellValue32(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT);
        return updated_occupancy;
        
    }

    bool  TryBindShareNext(uint32_t shared_next_id) noexcept
    {
        return TryBindPortTarget(MetaIndexOfAPCNode::SHARED_NEXT_ID, shared_next_id);
    }

    bool TryBindSharedPrevious(uint32_t shared_previous_id) noexcept
    {
        return TryBindPortTarget(MetaIndexOfAPCNode::SHARED_PREVIOUS_ID, shared_previous_id);
    }

    bool TurnOnASegmentFlag(ControlEnumOfAPCSegment desired_segment_flag) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(static_cast<uint32_t>(desired_segment_flag));
    }

    bool HasThisFlag(ControlEnumOfAPCSegment flag) noexcept
    {
        return (ReadAPCModeFlags_() & static_cast<uint32_t>(flag)) != 0u;
    }

    void SetGraphNodeFlag() noexcept
    {
        TurnOnASegmentFlag(ControlEnumOfAPCSegment::IS_GRAPH_NODE);
    }

    bool IsGraphNode() noexcept
    {
        return HasThisFlag(ControlEnumOfAPCSegment::IS_GRAPH_NODE);
    }

    uint32_t ReadCapacity() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::CAPACITY);
    }

    uint32_t PayloadEndRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::PAYLOAD_END);
    }

    uint32_t RegionCountRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_COUNT);
    }

    uint32_t SplitThresholdRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE);
    }

    uint32_t MaxDepthRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::MAX_DEPTH);
    }

    uint32_t CurrentBranchDepthRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::BRANCH_DEPTH);
    }

    bool ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment desired_control_flag) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(NO_VAL, static_cast<uint32_t>(desired_control_flag));
    }

    size_t PayloadCapacityFromHeader() noexcept
    {
        const uint32_t payload_begain = METACELL_COUNT;
        const uint32_t payload_end  = PayloadEndRead();
        if (payload_end > payload_begain)
        {
            return static_cast<size_t>(payload_end - payload_begain);
        }
        return NO_VAL;
    }

    void MakeAPCBranchOwned() noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CURRENTLY_OWNED, 1u, DEFAULT_INTERNAL_PRIORITY);
    }

    void ReleseOwneshipFlag() noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CURRENTLY_OWNED, NO_VAL, MAX_PRIORITY);
    }

    bool IsBranchOwnedByFlag() noexcept
    {
        uint32_t owned_cell_value = ReadMetaCellValue32(MetaIndexOfAPCNode::CURRENTLY_OWNED);
        if (owned_cell_value > NO_VAL)
        {
            return true;
        }
        return false;
    }

    void ResetTotalCASFailureForThisBranch(tag8_t priority = DEFAULT_INTERNAL_PRIORITY) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, NO_VAL, priority);
    }

    bool SetSegmentRegionKind(APCPagedNodeRelMaskClasses region_kind) noexcept
    {
        const uint32_t current_segment_kind = ReadMetaCellValue32(MetaIndexOfAPCNode::SEGMENT_KIND);
        return JustUpdateValueOfMeta32(MetaIndexOfAPCNode::SEGMENT_KIND, current_segment_kind, static_cast<uint32_t>(region_kind));
    }


};

}
