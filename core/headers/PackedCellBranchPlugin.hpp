#pragma once
#include "PackedCell.hpp"
#include "MasterClockConf.hpp"

namespace PredictedAdaptedEncoding
{
#define MIN_PRODUCER_BLOCK_SIZE 64
#define MIN_REGION_SIZE 64
#define MIN_RETIRE_BATCH_THRESHOLD 16
#define MIN_BACKGROUND_EPOCH_MS 50
#define INITIAL_BRANCH_SPLIT_THRESHOLD_PERCENTAGE 70
#define MINIMUM_BRANCH_CAPACITY 256
#define MAX_BRANCH_DEPTH 10

struct ContainerConf
{

    PackedMode InitialMode = PackedMode::MODE_VALUE32;
    size_t ProducerBlockSize = MIN_PRODUCER_BLOCK_SIZE;
    size_t RegionSize = MIN_REGION_SIZE;
    uint32_t RetireBatchThreshold = MIN_RETIRE_BATCH_THRESHOLD;
    uint32_t BackgroundEpochAdvanceMS = MIN_BACKGROUND_EPOCH_MS;
    bool EnableBranching = true;
    uint32_t BranchSplitThresholdPercentage = INITIAL_BRANCH_SPLIT_THRESHOLD_PERCENTAGE;
    uint32_t BranchMaxDepth = MAX_BRANCH_DEPTH;
    size_t BranchMinChildCapacity = MINIMUM_BRANCH_CAPACITY;
    uint32_t NodeGroupSize = 1u;
};

class PackedCellBranchPlugin final
{
public:
    static constexpr size_t METACELL_COUNT = 64;
    static constexpr uint32_t BRANCH_MAGIC = 0x41504342u;//big-endian
    static constexpr uint32_t EOF_HEADER = 0x72616600;//big-endian
    static constexpr uint32_t BRANCH_VERSION = 1u;
    static constexpr uint32_t BRANCH_SENTINAL = UINT32_MAX;
    static constexpr packed64_t APC_SENTENAL = UINT64_MAX;
    
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

    enum class APCBranchNodeRoleFlags : uint32_t
    {
        NODE_NONE = 0u,
        ACCEPTS_FEEDFORWARD = 1u << 0,
        EMMITS_FEEDFORWARD = 1u << 1,
        ACCETS_FEEDBACKWARD = 1u << 2,
        EMMITS_FEEDBACKWARD = 1u << 3,
        USES_LETERAL_0 = 1u << 4,
        USES_LETERAL_1 = 1u << 5,
        STORES_LOCAL_STATE = 1u << 6,
        STORES_LOCAL_ERROR = 1u  << 7,
        SELF_DATA_NODE = 1u << 8,
        SHARED_NODE_MEMBER = 1u << 9
    };

    enum class APCFlags : uint32_t
    {
        NONE = 0u,
        ENABLE_BRANCHING = 1u << 0,
        HAS_REGION_INDEX =  1u << 1,
        SATURATED = 1u << 2,
        SPLIT_INFLIGHT = 1u << 3,
        IS_GRAPH_NODE = 1u << 4,
        IS_SHARED_ROOT = 1u << 6,
        IS_SHARED_MAMBER = 1u << 7,
        HAS_SHARED_NEXT = 1u << 8,
        HAS_SHARED_PREVIOUS = 1u << 9
    };

    enum class MetaIndexOfAPCNode : size_t
    {
        //identity
        MAGIC_ID = 0,
        VERSION = 1,
        CAPACITY = 2,
        BRANCH_ID = 3,

        //logical-node Identity
        LOGICAL_NODE_ID = 4,
        SHARED_ID = 5,
        SHARED_PREVIOUS_ID = 6,
        SHARED_NEXT_ID = 7,

        //runtime-controle
        BRANCH_DEPTH = 8,
        BRANCH_PRIORITY = 9,
        FLAGS = 10,
        CURRENT_ACTIVE_THREADS = 11,
        OCCUPANCY_SNAPSHOT = 12,
        SAFE_POINT = 13,
        SPLIT_THRESHOLD_PERCENTAGE = 14,
        MAX_DEPTH = 15,

        //payload-Bounds
        PAYLOAD_BEGIN = 16,
        PAYLOAD_END = 17,

        //timing
        LOCAL_CLOCK48 = 18,
        LAST_SPLIT_EPOCH = 19,

        //region summery
        REGION_SIZE = 20,
        REGION_COUNT = 21,
        READY_REL_MASK = 22,
        PRODUCER_BLOCK_SIZE = 23,
        BACKGROUND_EPOCH_ADVANCE_MS =  24,
        DEFINED_MODE_OF_CURRENT_APC = 25,
        RETIRE_BRANCH_THRASHOLD = 26,
        PRODUCER_CURSOR_PLACEMENT = 27,
        CONSUMER_CURSORE_PLACEMENT = 28,
        CURRENTLY_OWNED = 29,
        TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH = 30,
        NODE_GROUP_SIZE = 31,
        NODE_AUX_PARAM_U32 = 32,

        //graph ports 
        FEEDFORWARD_IN_TARGET_ID = 33,
        FEEDFORWARD_OUT_TARGET_ID = 34,
        FEEDBACKWARD_IN_TARGET_ID = 35,
        FEEDBACKWARD_OUT_TARGET_ID = 36,
        LATERAL_0_TARGET_ID = 37,
        LATERAL_1_TARGET_ID = 38,
        NODE_ROLE_FLAGS = 39,
        LAST_ACCEPTED_FEED_FORWARD_CLOCK16 = 40,
        LAST_EMITTED_FEED_FORWARD_CLOCK16 = 41,
        LAST_ACCEPTED_FEED_BACKWARD_CLOCK16 = 42,
        LAST_EMITTED_FEED_BACKWARD_CLOCK16 = 43,
        NODE_COMPUTE_KIND = 44,

        RESERVED_45 = 45,
        EOF_APC_HEADER = 63
    };


    inline bool ValidMeteIdx(MetaIndexOfAPCNode idx) const noexcept
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

    inline packed64_t PackValue32InPackedCellwithClock16_(
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

    inline uint32_t ReadAPCFlags_() noexcept
    {
        return (ReadMetaCellValue32(MetaIndexOfAPCNode::FLAGS));
    }

    inline bool UpdateFlagsOfBranch_(uint32_t flags_to_turn_on = NO_VAL, uint32_t flags_to_turn_off = NO_VAL) noexcept
    {
        while (true)
        {
            const uint32_t current_flags = ReadAPCFlags_();
            uint32_t next_flags = current_flags;
            next_flags |= flags_to_turn_on;
            next_flags &= ~flags_to_turn_off;
            if (next_flags == current_flags)
            {
                return true;
            }
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::FLAGS, current_flags, next_flags))
            {
                return true;
            }
        }
    }

public:
    //checked top


    inline packed64_t PackPureClock48AsPackedCell(
        std::optional<uint64_t> clock48 = std::nullopt,
        tag8_t priority = ZERO_PRIORITY,
        PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
        tag8_t rel_mask = REL_NONE,
        RelOffsetMode48 reloffset = RelOffsetMode48::RELOFFSET_PURE_TIMER,
        PackedCellDataType dtype = PackedCellDataType::UnsignedPCellDataType
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



    void WriteOrUpdateMetaClock48(tag8_t priority = ZERO_PRIORITY, std::optional<uint64_t>meta_clock_48 = std::nullopt) noexcept
    {
        size_t idx = static_cast<size_t>(MetaIndexOfAPCNode::LOCAL_CLOCK48);
        packed64_t wanted_cell = PackPureClock48AsPackedCell(meta_clock_48, priority, PackedCellLocalityTypes::ST_PUBLISHED);
        PackedCellContainerPtr_[idx].store(wanted_cell, MoStoreSeq_);
        PackedCellContainerPtr_[idx].notify_all();
    }

    inline bool JustUpdateValueOfMeta32(
        MetaIndexOfAPCNode idx,
        uint32_t expected_value,
        uint32_t desired_value,
        bool refresh_clock16 = true
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

    PackedCellBranchPlugin() noexcept = default;

    void Bind(std::atomic<packed64_t>* packed_cells, size_t capacity, MasterClockConf* master_clock_ptr) noexcept
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
    inline void InitLogicalNodeIdentity(
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
            TurnOnFlags(static_cast<uint32_t>(APCFlags::IS_GRAPH_NODE) | static_cast<uint32_t>(APCFlags::IS_SHARED_ROOT));
        }
        else
        {
            TurnOnFlags(static_cast<uint32_t>(APCFlags::IS_GRAPH_NODE) | static_cast<uint32_t>(APCFlags::IS_SHARED_MAMBER));
        }
        
    }

    inline void InitNodeSemantics(
        uint32_t node_role_flags,
        APCNodeComputeKind compute_kind_of_node,
        uint32_t aux_param_uint32 = NO_VAL
    ) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::NODE_ROLE_FLAGS, node_role_flags, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::NODE_COMPUTE_KIND, static_cast<uint32_t>(compute_kind_of_node), ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::NODE_AUX_PARAM_U32, aux_param_uint32, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_FORWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_BACKWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_EMITTED_FEED_FORWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LAST_EMITTED_FEED_BACKWARD_CLOCK16, NO_VAL, ZERO_PRIORITY);
    }

    void InitRootOrChildBranch(
        uint32_t branch_id,
        uint32_t logical_node_id,
        uint32_t shared_id,
        size_t total_capacity,
        const ContainerConf& container_configuration,
        bool is_root_shared = true,
        uint32_t node_role_flags = static_cast<uint32_t>(APCBranchNodeRoleFlags::NODE_NONE),
        APCNodeComputeKind node_compute_kind = APCNodeComputeKind::NONE,
        uint32_t aux_param_uint32 = NO_VAL,
        uint32_t branch_depth = NO_VAL,
        uint8_t branch_priority = ZERO_PRIORITY,
        uint8_t write_cell_priority = ZERO_PRIORITY

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
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FLAGS, container_configuration.EnableBranching ? static_cast<uint32_t>(APCFlags::ENABLE_BRANCHING) : static_cast<uint32_t>(APCFlags::NONE), write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::CURRENT_ACTIVE_THREADS, NO_VAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT, NO_VAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::SAFE_POINT, NO_VAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE, container_configuration.BranchSplitThresholdPercentage, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::MAX_DEPTH, container_configuration.BranchMaxDepth, write_cell_priority);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::PAYLOAD_BEGIN, static_cast<uint32_t>(METACELL_COUNT), write_cell_priority);
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


        //node
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDFORWARD_IN_TARGET_ID, BRANCH_SENTINAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDFORWARD_OUT_TARGET_ID, BRANCH_SENTINAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDBACKWARD_IN_TARGET_ID, BRANCH_SENTINAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::FEEDBACKWARD_OUT_TARGET_ID, BRANCH_SENTINAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LATERAL_0_TARGET_ID, BRANCH_SENTINAL, write_cell_priority);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::LATERAL_1_TARGET_ID, BRANCH_SENTINAL, write_cell_priority);
        ////-----/////
        InitLogicalNodeIdentity(logical_node_id, shared_id, is_root_shared);
        InitNodeSemantics(node_role_flags, node_compute_kind, aux_param_uint32);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::EOF_APC_HEADER, EOF_HEADER, write_cell_priority);


    }

    val32_t ReadMetaCellValue32(MetaIndexOfAPCNode idx) noexcept
    {
        if (!ValidMeteIdx(idx) || idx == MetaIndexOfAPCNode::LOCAL_CLOCK48)
        {
            return NO_VAL;
        }
        size_t index = static_cast<size_t>(idx);
        return PackedCell64_t::ExtractValue32(PackedCellContainerPtr_[index].load(MoLoad_));
    }

    void TouchLocalMetaClock48(packed64_t* updated_full_clock_cell_easy_return_ptr = nullptr) noexcept
    {
        if (!MasterClockConfPtr_)
        {
            return;
        }
        MasterClockConfPtr_->TouchAtomicPackedCellClockForCurrentThread(
            PackedCellContainerPtr_[static_cast<size_t>(MetaIndexOfAPCNode::LOCAL_CLOCK48)], updated_full_clock_cell_easy_return_ptr
        );
    }

    bool TryIncrementOrDecrementActiveThreadCount(int8_t change_count) noexcept
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

    uint32_t ForceOccupancyUpdateAndReturn(uint32_t new_occupancy) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT, new_occupancy);
        uint32_t updated_occupancy = ReadMetaCellValue32(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT);
        return updated_occupancy;
        
    }

    void OrReadyRelMask(tag8_t rel_mask) noexcept
    {
        while (true)
        {
            val32_t current_branch_rel_mask = ReadMetaCellValue32(MetaIndexOfAPCNode::READY_REL_MASK);
            uint32_t next = current_branch_rel_mask | static_cast<uint32_t>(rel_mask & RELMASK_MASK);
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::READY_REL_MASK, current_branch_rel_mask, next))
            {
                return;
            }
        }
    }

    bool ShouldSplitNow() noexcept
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

    inline bool TryBindPortTarget(MetaIndexOfAPCNode port_meta_idx, uint32_t target_branch_id) noexcept
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

    inline bool  TryBindShareNext(uint32_t shared_next_id) noexcept
    {
        return TryBindPortTarget(MetaIndexOfAPCNode::SHARED_NEXT_ID, shared_next_id);
    }

    inline bool TryBindSgaredPrevious(uint32_t shared_previous_id) noexcept
    {
        return TryBindPortTarget(MetaIndexOfAPCNode::SHARED_PREVIOUS_ID, shared_previous_id);
    }

    inline bool TurnOnFlags(uint32_t use_or_between_flags = NO_VAL) noexcept
    {
        return UpdateFlagsOfBranch_(use_or_between_flags);
    }

    inline bool HasThisFlag(APCFlags flag) noexcept
    {
        return (ReadAPCFlags_() & static_cast<uint32_t>(flag)) != 0u;
    }

    inline void SetGraphNodeFlag() noexcept
    {
        TurnOnFlags(static_cast<uint32_t>(APCFlags::IS_GRAPH_NODE));
    }

    inline bool IsGraphNode() noexcept
    {
        return HasThisFlag(APCFlags::IS_GRAPH_NODE);
    }

    inline uint32_t ReadCapacity() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::CAPACITY);
    }

    inline uint32_t PayloadBegainRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::PAYLOAD_BEGIN);
    }

    inline uint32_t PayloadEndRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::PAYLOAD_END);
    }

    inline uint32_t RegionCountRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_SIZE);
    }

    inline uint32_t SplitThresholdRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE);
    }

    inline uint32_t MaxDepthRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::MAX_DEPTH);
    }

    inline uint32_t CurrentBranchDepthRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCNode::BRANCH_DEPTH);
    }

    inline bool ClearFlags(uint32_t use_or_between_flags = NO_VAL) noexcept
    {
        return UpdateFlagsOfBranch_(NO_VAL, use_or_between_flags);
    }

    inline size_t PayloadCapacityFromHeader() noexcept
    {
        const uint32_t payload_begain = PayloadBegainRead();
        const uint32_t payload_end  = PayloadEndRead();
        if (payload_end > payload_begain)
        {
            return static_cast<size_t>(payload_end - payload_begain);
        }
        return NO_VAL;
    }

    inline size_t ClampPayloadIndex(size_t idx) noexcept
    {
        const size_t payload_begain = static_cast<size_t>(PayloadBegainRead());
        const size_t payload_end = static_cast<size_t>(PayloadEndRead());
        if (payload_end <= payload_begain)
        {
            return SIZE_MAX;
        }
        if (idx < payload_begain)
        {
            idx = payload_begain;
        }
        if (idx > payload_end)
        {
            idx = payload_begain + ((idx - payload_begain) % (payload_end - payload_begain));
        }
        return idx;
    }

    bool TryMarkSplitInFlight() noexcept
    {
        while (true)
        {
            const uint32_t current_flags = ReadMetaCellValue32(MetaIndexOfAPCNode::FLAGS);
            if (current_flags == BRANCH_SENTINAL)
            {
                return false;
            }

            bool is_already_in_flight = HasThisFlag(APCFlags::SPLIT_INFLIGHT);
            if (is_already_in_flight)
            {
                return false;
            }
            return TurnOnFlags(static_cast<uint32_t>(APCFlags::SPLIT_INFLIGHT));
        }
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

    uint32_t TotalCASFailForThisBranchIncreaseAndGet(uint32_t increment) noexcept
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

    void ResetTotalCASFailureForThisBranch(tag8_t priority = DEFAULT_INTERNAL_PRIORITY) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCNode::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, NO_VAL, priority);
    }
};

}
