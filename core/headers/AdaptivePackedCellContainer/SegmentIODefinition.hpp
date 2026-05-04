#pragma once
#include "PackedCell/CoreCellDefination.hpp"
#include "APCSpikeController/AtomicAdaptiveBackoff.hpp"

namespace PredictedAdaptedEncoding
{


class SegmentIODefinition : public APCStaticsFirst
{
public:
    std::atomic<packed64_t>* BackingPtr{nullptr};
    static constexpr uint32_t BRANCH_SENTINAL = LayoutBoundsOfSingleRelNodeClass::BRANCH_SENTINAL;

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

    enum class ManagerControlFlagBits : uint32_t
    {
        NONE = 0u,
        REGISTERED_APC = 1U << 0,
        DEAD_APC = 1U << 1,
        RECLAIMATION_REQUST_FOR_JUST_THIS_APC = 1u << 2,
        RECLAIMATION_REQUEST_FOR_WHOLE_CHAIN = 1u << 3,
        REQUEST_NEW_SEGMENTATION = 1u << 4,
        IN_WORK_STACK = 1u << 5,
        IN_CLEANUP_STACK = 1u << 6
    };


    bool ValidMeteIdx(MetaIndexOfAPCNode idx) const noexcept
    {
        return BackingPtr && static_cast<size_t>(idx) < BranchCapacity_ && static_cast<size_t>(idx) < METACELL_COUNT;
    }

    packed64_t ReadFullMetaCell(MetaIndexOfAPCNode idx) noexcept
    {
        if (ValidMeteIdx(idx))
        {
            return BackingPtr[static_cast<size_t>(idx)].load(MoLoad_);
        }
        return APC_SENTENAL;
    }
    
protected:
    Timer48 LocalTimer48_;
    AtomicAdaptiveBackoff* AdaptiveBackoffOfAPCPtr_{nullptr};
    std::unique_ptr<MasterClockConf> OwnedMasterClockConfPtr_;
    size_t BranchCapacity_{0};

    packed64_t PackValue32InPackedCellwithClock16_(
        val32_t value32,
        PriorityPhysics priority,
        PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
        APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::NONE,
        RelOffsetMode32 reloffset_mode32 = RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
        PackedCellDataType dtype = PackedCellDataType::UnsignedPCellDataType,
        PackedCellNodeAuthority node_authority = PackedCellNodeAuthority::IDLE_OR_FREE
    ) noexcept
    {
        if (OwnedMasterClockConfPtr_)
        {
            return OwnedMasterClockConfPtr_->ComposeValue32WithCurrentThreadStamp16(value32, page_class, priority, locality, reloffset_mode32, dtype);
        }
        meta16_t strl_moded32 = PackedCell64_t::MakeInCellMetaForMode_32t(priority, node_authority, locality, page_class, reloffset_mode32, dtype);
        return PackedCell64_t::ComposeValue32u_64(value32, UNSIGNED_ZERO, strl_moded32);
    }

    void WriteBrenchMeta32_(
        MetaIndexOfAPCNode idx,
        uint32_t value32,
        PriorityPhysics priority = PriorityPhysics::IDLE,
        APCPagedNodeRelMaskClasses rel_mask4 = APCPagedNodeRelMaskClasses::CONTROL_SLOT
    ) noexcept
    {
        size_t index = static_cast<size_t>(idx);
        if (!ValidMeteIdx(idx))
        {
            return;
        }
        BackingPtr[index].store(PackValue32InPackedCellwithClock16_(value32, priority, PackedCellLocalityTypes::ST_PUBLISHED, rel_mask4), MoStoreSeq_);
        BackingPtr[index].notify_all();
    }


    bool TurnOnMultipleSegmentFlagsAtOnce_(uint32_t use_or_between_flags = UNSIGNED_ZERO) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(use_or_between_flags, UNSIGNED_ZERO, MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS);
    }

    bool UpdateAPCModeFlagsInHeader_(uint32_t flags_to_turn_on = UNSIGNED_ZERO, uint32_t flags_to_turn_off = UNSIGNED_ZERO, MetaIndexOfAPCNode desired_flag_idx = MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS) noexcept;

    std::optional<std::pair<MetaIndexOfAPCNode, MetaIndexOfAPCNode>> GetMetaBoundsLegalPairForPageClasses(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept;

    bool WriteBoundsPairToHeader_(const LayoutBoundsOfSingleRelNodeClass layout_bound) noexcept;

    void BuidDefaultLayoutPlan_(CompleteAPCNodeRegionsLayout& full_layout) noexcept;

    void InitDefaultAPCSegmentedNodeLayout_() noexcept;


    bool WriteAllRegionsLayoutToHeader_(const CompleteAPCNodeRegionsLayout& full_layout) noexcept;

    bool TurnOnReadyBitForDesiredPagedNode_(APCPagedNodeRelMaskClasses desired_region_class) noexcept;

    bool ClearTheDesiredPagedNodeReadyBit_(APCPagedNodeRelMaskClasses desired_region_class) noexcept;

    bool ClearMultipleControlFlags_(uint32_t use_or_between_flags = UNSIGNED_ZERO) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(UNSIGNED_ZERO, use_or_between_flags);
    }

    bool ForceZeroOccupancy_() noexcept;
    
public:
    packed64_t PackPureClock48AsPackedCell(
        std::optional<uint64_t> clock48 = std::nullopt,
        PriorityPhysics priority = PriorityPhysics::IDLE,
        PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
        APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::CLOCK_PURE_TIME,
        RelOffsetMode48 reloffset = RelOffsetMode48::RELOFFSET_PURE_TIMER,
        PackedCellDataType dtype = PackedCellDataType::UnsignedPCellDataType,
        PackedCellNodeAuthority node_authority = PackedCellNodeAuthority::IDLE_OR_FREE
    ) noexcept;

    void WriteOrUpdateMetaClock48(PriorityPhysics priority = PriorityPhysics::IDLE, std::optional<uint64_t>meta_clock_48 = std::nullopt) noexcept;

    bool JustUpdateValueOfMeta32(
        MetaIndexOfAPCNode idx,
        uint32_t expected_value,
        uint32_t desired_value,
        bool refresh_clock16 = true
    ) noexcept;

    SegmentIODefinition() noexcept = default;


    bool IsBound() const noexcept
    {
        return BackingPtr != nullptr && BranchCapacity_ >= METACELL_COUNT;
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
        uint32_t aux_param_uint32 = UNSIGNED_ZERO
    ) noexcept;


    void InitRootOrChildBranch(
        uint32_t branch_id,
        uint32_t logical_node_id,
        uint32_t shared_id,
        size_t total_capacity,
        const ContainerConf& container_configuration,
        bool is_root_shared = true,
        APCNodeComputeKind node_compute_kind = APCNodeComputeKind::NONE,
        uint32_t aux_param_uint32 = UNSIGNED_ZERO,
        uint32_t branch_depth = UNSIGNED_ZERO,
        uint8_t branch_priority = ZERO_PRIORITY,
        PriorityPhysics write_cell_priority = PriorityPhysics::IDLE

    ) noexcept;

    val32_t ReadMetaCellValue32(MetaIndexOfAPCNode idx) noexcept;

    void TouchLocalMetaClock48() noexcept;

    bool TryIncrementOrDecrementActiveThreadCount(int8_t change_count) noexcept;

    bool TryMarkSplitInFlight() noexcept;

    bool ShouldSplitNow() noexcept;

    bool TryBindPortTarget(MetaIndexOfAPCNode port_meta_idx, uint32_t target_branch_id) noexcept;

    uint32_t TotalCASFailForThisBranchIncreaseAndGet(uint32_t increment) noexcept;

    bool SetLayOutBounds(APCPagedNodeRelMaskClasses desired_rel_mask, uint32_t begain, uint32_t end) noexcept;

    std::optional<LayoutBoundsOfSingleRelNodeClass> ReadLayoutBounds(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept;
    std::optional<CompleteAPCNodeRegionsLayout> ReadAndGetFullRegionLayout_() noexcept;


    bool TrySetLayoutMutationInFlight() noexcept;

    bool TryExtendASegmentInOwnAPC(APCPagedNodeRelMaskClasses desired_rel_mask, uint32_t wanted_amount, ContainerConf::APCSegmentExtendOrder desired_apc_order) noexcept;

    clk16_t ReadLastAcceptedClok16ForThisSegment(APCPagedNodeRelMaskClasses region_kind) noexcept;
    clk16_t ReadLastEmittedClok16ForThisSegment(APCPagedNodeRelMaskClasses region_kind) noexcept;

    bool WriteExactMetaCellJustNewValue(MetaIndexOfAPCNode idx, uint32_t value) noexcept;

    uint32_t ReadRegionOccupancy(APCPagedNodeRelMaskClasses desired_region_class) noexcept
    {
        return ReadMetaCellValue32(APCAndPagedNodeHelpers::GetOccupancyMetIndexByRegionClass(desired_region_class));
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
        return UpdateAPCModeFlagsInHeader_(static_cast<uint32_t>(desired_segment_flag), UNSIGNED_ZERO, MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS);
    }

    bool HasThisControlEnumFlag(ControlEnumOfAPCSegment flag) noexcept
    {
        return (ReadMetaCellValue32(MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS) & static_cast<uint32_t>(flag)) != 0u;
    }

    bool ClearOneControlEnumFlagOfAPC(ControlEnumOfAPCSegment desired_control_flag) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(UNSIGNED_ZERO, static_cast<uint32_t>(desired_control_flag), MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS);
    }

    bool TurnOnAManagerControlFlag(ManagerControlFlagBits desired_manager_control_flag) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(static_cast<uint32_t>(desired_manager_control_flag), UNSIGNED_ZERO, MetaIndexOfAPCNode::MANAGER_CONTROL_FLAGS);
    }

    bool ClearOneManagerControlFlag(ManagerControlFlagBits desired_manager_control_flag) noexcept
    {
        return UpdateAPCModeFlagsInHeader_(UNSIGNED_ZERO, static_cast<uint32_t>(desired_manager_control_flag), MetaIndexOfAPCNode::MANAGER_CONTROL_FLAGS);
    }

    bool HasThisManageControlFlag(ManagerControlFlagBits desired_manager_contgrol_flag) noexcept
    {
        return (ReadMetaCellValue32(MetaIndexOfAPCNode::MANAGER_CONTROL_FLAGS) & static_cast<uint32_t>(desired_manager_contgrol_flag)) != UNSIGNED_ZERO;
    }
    
    void SetGraphNodeFlag() noexcept
    {
        TurnOnASegmentFlag(ControlEnumOfAPCSegment::IS_GRAPH_NODE);
    }

    bool IsGraphNode() noexcept
    {
        return HasThisControlEnumFlag(ControlEnumOfAPCSegment::IS_GRAPH_NODE);
    }

    uint32_t GetTotalCapacityForThisAPC() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::TOTAL_CAPACITY_OF_THIS_SEGEMENT);
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

    size_t PayloadCapacityFromHeader() noexcept
    {
        const uint32_t payload_begain = METACELL_COUNT;
        const uint32_t payload_end  = GetTotalCapacityForThisAPC();
        if (payload_end > payload_begain)
        {
            return static_cast<size_t>(payload_end - payload_begain);
        }
        return UNSIGNED_ZERO;
    }

    void MakeAPCBranchOwned() noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CURRENTLY_OWNED, 1u, PriorityPhysics::IMPORTANT);
    }

    void ReleseOwneshipFlag() noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CURRENTLY_OWNED, UNSIGNED_ZERO, PriorityPhysics::ERROR_DEPENDENCY);
    }

    bool IsBranchOwnedByFlag() noexcept
    {
        uint32_t owned_cell_value = ReadMetaCellValue32(MetaIndexOfAPCNode::CURRENTLY_OWNED);
        if (owned_cell_value > UNSIGNED_ZERO)
        {
            return true;
        }
        return false;
    }

    void ResetTotalCASFailureForThisBranch(PriorityPhysics priority = PriorityPhysics::IDLE) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, UNSIGNED_ZERO, priority);
    }

    bool SetSegmentRegionKind(APCPagedNodeRelMaskClasses region_kind) noexcept
    {
        const uint32_t current_segment_kind = ReadMetaCellValue32(MetaIndexOfAPCNode::SEGMENT_KIND);
        return JustUpdateValueOfMeta32(MetaIndexOfAPCNode::SEGMENT_KIND, current_segment_kind, static_cast<uint32_t>(region_kind));
    }

    std::atomic<packed64_t>* GetAPCBackinghPtr() noexcept
    {
        if (BackingPtr)
        {
            return BackingPtr;
        }
        return nullptr;
    }


};

}
