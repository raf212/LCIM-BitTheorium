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
    unsigned RetireBatchThreshold = MIN_RETIRE_BATCH_THRESHOLD;
    unsigned BackgroundEpochAdvanceMS = MIN_BACKGROUND_EPOCH_MS;
    bool EnableBranching = true;
    uint32_t BranchSplitThresholdPercentage = INITIAL_BRANCH_SPLIT_THRESHOLD_PERCENTAGE;
    uint32_t BranchMaxDepth = MAX_BRANCH_DEPTH;
    size_t BranchMinChildCapacity = MINIMUM_BRANCH_CAPACITY;
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

    enum class TreePosition : uint32_t
    {
        ROOT = 0,
        LEFT = 1,
        RIGHT = 2,
        TREE_OVERFLOW = 3
    };

    enum class APCFlags : uint32_t
    {
        NONE = 0u,
        ENABLE_BRANCHING = 1u << 0,
        HAS_REGION_INDEX =  1u << 1,
        SATURATED = 1u << 2,
        SPLIT_INFLIGHT = 1u << 3,
        HAS_LEFT_CHILD = 1u << 4,
        HAS_RIGHT_CHILD = 1u << 5,
        IS_GRAPH_NODE = 1u << 6,
        IS_LINKED_NODE = 1u << 7
    };

    enum class MetaIndexOfAPCBranch : size_t 
    {
        //identity
        MAGIC_ID = 0,
        VERSION = 1,
        CAPACITY = 2,
        BRANCH_ID = 3,
        PARENT_BRANCH_ID = 4,
        LEFT_CHILD_ID = 5,
        RIGHT_CHILD_ID = 6,
        NUMBER_OF_CHILD = 30,
        CURRENT_TREE_POSITION = 7,
        DEFINED_MODE_OF_CURRENT_APC = 25,
        // runtime control
        BRANCH_DEPTH = 8,
        BRANCH_PRIORITY = 9,
        FLAGS = 10,
        CURRENT_ACTIVE_THREADS = 11,
        OCCUPANCY_SNAPSHOT = 12,
        SAFE_POINT = 13,
        SPLIT_THRESHOLD_PERCENTAGE = 14,
        MAX_DEPTH = 15,
        RETIRE_BRANCH_THRASHOLD = 26,
        PRODUCER_CURSOR_PLACEMENT = 27,
        CONSUMER_CURSORE_PLACEMENT = 28,
        TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH = 32,
        //payload bounds
        PAYLOAD_BEGIN = 16,
        PAYLOAD_END = 17,
        PRODUCER_BLOCK_SIZE = 23,
        BACKGROUND_EPOCH_ADVANCE_MS =  24,
        //timing
        LOCAL_CLOCK48 = 18,
        LAST_SPLIT_EPOCH = 19,
        //region summery
        REGION_SIZE = 20,
        REGION_COUNT = 21,
        READY_REL_MASK = 22,
        //ownership status(leter can be converted into ownership bit flags enum class)
        CURRENTLY_OWNED = 29,

        //free exception 
        HALFWAY_MAGIC = 31,
        RESERVED_33 = 33,
        EOF_APC_HEADER = 63
    };

    inline bool ValidMeteIdx(MetaIndexOfAPCBranch idx) const noexcept
    {
        return PackedCellContainerPtr_ && static_cast<size_t>(idx) < BranchCapacity_ && static_cast<size_t>(idx) < METACELL_COUNT;
    }

    packed64_t ReadFullMetaCell(MetaIndexOfAPCBranch idx) noexcept
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
        MetaIndexOfAPCBranch idx,
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
        return (ReadMetaCellValue32(MetaIndexOfAPCBranch::FLAGS));
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
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCBranch::FLAGS, current_flags, next_flags))
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
        size_t idx = static_cast<size_t>(MetaIndexOfAPCBranch::LOCAL_CLOCK48);
        packed64_t wanted_cell = PackPureClock48AsPackedCell(meta_clock_48, priority, PackedCellLocalityTypes::ST_PUBLISHED);
        PackedCellContainerPtr_[idx].store(wanted_cell, MoStoreSeq_);
        PackedCellContainerPtr_[idx].notify_all();
    }

    inline bool JustUpdateValueOfMeta32(
        MetaIndexOfAPCBranch idx,
        uint32_t expected_value,
        uint32_t desired_value,
        bool refresh_clock16 = true
    ) noexcept
    {
        if (!ValidMeteIdx(idx) || idx == MetaIndexOfAPCBranch::LOCAL_CLOCK48)
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
    void InitRootOrChildBranch(
        uint32_t branch_id,
        uint32_t parent_bramch_id,
        size_t total_capacity,
        TreePosition current_tree_position,
        uint32_t split_threshold_percantage = INITIAL_BRANCH_SPLIT_THRESHOLD_PERCENTAGE,
        uint32_t current_depth = NO_VAL,
        uint32_t max_depth = MAX_BRANCH_DEPTH,
        uint32_t producer_minimum_block_size = MIN_PRODUCER_BLOCK_SIZE,
        uint32_t background_epoch_ms = MIN_BACKGROUND_EPOCH_MS,
        uint32_t region_size = MIN_REGION_SIZE,
        uint32_t region_count = NO_VAL,
        uint8_t branch_priority = ZERO_PRIORITY,
        uint8_t priority = ZERO_PRIORITY,
        uint32_t initial_flags = static_cast<uint32_t>(APCFlags::ENABLE_BRANCHING),
        uint32_t probable_initial_defined_branch_mode = static_cast<uint32_t>(PackedMode::MODE_VALUE32),
        uint32_t initial_retire_brunch_thrashold = MIN_RETIRE_BATCH_THRESHOLD

    ) noexcept
    {
        if (!IsBound())
        {
            return;
        }
        
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::MAGIC_ID, BRANCH_MAGIC, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::VERSION, BRANCH_VERSION, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::CAPACITY, static_cast<uint32_t>(std::min<size_t>(total_capacity, BRANCH_SENTINAL)), priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::BRANCH_ID, static_cast<uint32_t>(std::min<uint32_t>(branch_id, BRANCH_SENTINAL)), priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::PARENT_BRANCH_ID, static_cast<uint32_t>(std::min<uint32_t>(parent_bramch_id, BRANCH_SENTINAL)), priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::LEFT_CHILD_ID, BRANCH_SENTINAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::RIGHT_CHILD_ID, BRANCH_SENTINAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::CURRENT_TREE_POSITION, static_cast<uint32_t>(current_tree_position), priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::DEFINED_MODE_OF_CURRENT_APC, probable_initial_defined_branch_mode, priority);

        WriteBrenchMeta32_(MetaIndexOfAPCBranch::BRANCH_DEPTH, current_depth, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::BRANCH_PRIORITY, branch_priority, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::FLAGS, initial_flags, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::CURRENT_ACTIVE_THREADS, NO_VAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT, NO_VAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::SAFE_POINT, NO_VAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::SPLIT_THRESHOLD_PERCENTAGE, split_threshold_percantage, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::MAX_DEPTH, max_depth, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::RETIRE_BRANCH_THRASHOLD, initial_retire_brunch_thrashold, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::PRODUCER_CURSOR_PLACEMENT, BRANCH_SENTINAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::CONSUMER_CURSORE_PLACEMENT, BRANCH_SENTINAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, NO_VAL, priority);

        WriteBrenchMeta32_(MetaIndexOfAPCBranch::PAYLOAD_BEGIN, static_cast<uint32_t>(METACELL_COUNT), priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::PAYLOAD_END, static_cast<uint32_t>(std::min<size_t>(total_capacity, BRANCH_SENTINAL)), priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::PRODUCER_BLOCK_SIZE, producer_minimum_block_size, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::BACKGROUND_EPOCH_ADVANCE_MS, background_epoch_ms, priority);

        WriteOrUpdateMetaClock48(priority, NO_VAL);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::LAST_SPLIT_EPOCH, NO_VAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::REGION_SIZE, region_size, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::REGION_COUNT, region_count, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::READY_REL_MASK, NO_VAL, priority);
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::EOF_APC_HEADER, EOF_HEADER, priority);

        WriteBrenchMeta32_(MetaIndexOfAPCBranch::CURRENTLY_OWNED, NO_VAL, priority);

    }

    val32_t ReadMetaCellValue32(MetaIndexOfAPCBranch idx) noexcept
    {
        if (!ValidMeteIdx(idx) || idx == MetaIndexOfAPCBranch::LOCAL_CLOCK48)
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
            PackedCellContainerPtr_[static_cast<size_t>(MetaIndexOfAPCBranch::LOCAL_CLOCK48)], updated_full_clock_cell_easy_return_ptr
        );
    }

    bool TryAttachChildAPC(TreePosition side, uint32_t child_id) noexcept
    {
        MetaIndexOfAPCBranch current_tree_position = MetaIndexOfAPCBranch::EOF_APC_HEADER;
        if (side == TreePosition::LEFT)
        {
            current_tree_position = MetaIndexOfAPCBranch::LEFT_CHILD_ID;
        }
        else if (side == TreePosition::RIGHT)
        {
            current_tree_position = MetaIndexOfAPCBranch::RIGHT_CHILD_ID;
        }
        else
        {
            return false;
        }
        
        return JustUpdateValueOfMeta32(current_tree_position, BRANCH_SENTINAL, child_id);
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
            uint32_t current_thread_count = ReadMetaCellValue32(MetaIndexOfAPCBranch::CURRENT_ACTIVE_THREADS);
            if (current_thread_count == UINT32_MAX)
            {
                return false;
            }
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCBranch::CURRENT_ACTIVE_THREADS, current_thread_count, current_thread_count + change_count))
            {
                return true;
            }
        }
    }

    uint32_t ForceOccupancyUpdateAndReturn(uint32_t new_occupancy) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT, new_occupancy);
        uint32_t updated_occupancy = ReadMetaCellValue32(MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT);
        return updated_occupancy;
        
    }

    void OrReadyRelMask(tag8_t rel_mask) noexcept
    {
        while (true)
        {
            val32_t current_branch_rel_mask = ReadMetaCellValue32(MetaIndexOfAPCBranch::READY_REL_MASK);
            uint32_t next = current_branch_rel_mask | static_cast<uint32_t>(rel_mask & RELMASK_MASK);
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCBranch::READY_REL_MASK, current_branch_rel_mask, next))
            {
                return;
            }
        }
    }

    bool ShouldSplitNow() noexcept
    {
        const val32_t split_threshold = ReadMetaCellValue32(MetaIndexOfAPCBranch::SPLIT_THRESHOLD_PERCENTAGE);
        const val32_t current_occumancy = ReadMetaCellValue32(MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT);
        const val32_t max_depth = ReadMetaCellValue32(MetaIndexOfAPCBranch::MAX_DEPTH);
        const val32_t depth_of_current_branch = ReadMetaCellValue32(MetaIndexOfAPCBranch::BRANCH_DEPTH);
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

    inline uint32_t ReadCapacity() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCBranch::CAPACITY);
    }

    inline uint32_t PayloadBegainRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCBranch::PAYLOAD_BEGIN);
    }

    inline uint32_t PayloadEndRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCBranch::PAYLOAD_END);
    }

    inline uint32_t RegionCountRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCBranch::REGION_SIZE);
    }

    inline uint32_t SplitThresholdRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCBranch::SPLIT_THRESHOLD_PERCENTAGE);
    }

    inline uint32_t MaxDepthRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCBranch::MAX_DEPTH);
    }

    inline uint32_t CurrentBranchDepthRead() noexcept
    {
        return ReadMetaCellValue32(MetaIndexOfAPCBranch::BRANCH_DEPTH);
    }

    inline bool HasThisFlag(APCFlags flag) noexcept
    {
        return (ReadAPCFlags_() & static_cast<uint32_t>(flag)) != 0u;
    }


    inline bool TurnOnFlags(uint32_t use_or_between_flags = NO_VAL) noexcept
    {
        return UpdateFlagsOfBranch_(use_or_between_flags);
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

    bool TrySetLeftChild (uint32_t child_id) noexcept
    {
        bool ok = TryAttachChildAPC(TreePosition::LEFT, child_id);
        if (ok)
        {
            return TurnOnFlags(static_cast<uint32_t>(APCFlags::HAS_LEFT_CHILD));
        }
        return ok;
    }

    bool TrySetRightChild(uint32_t child_id) noexcept
    {
        bool ok = TryAttachChildAPC(TreePosition::RIGHT, child_id);
        if (ok)
        {
            return TurnOnFlags(static_cast<uint32_t>(APCFlags::HAS_RIGHT_CHILD));
        }
        return ok;
    }

    bool TryMarkSplitInFlight() noexcept
    {
        while (true)
        {
            const uint32_t current_flags = ReadMetaCellValue32(MetaIndexOfAPCBranch::FLAGS);
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
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::CURRENTLY_OWNED, 1u, DEFAULT_INTERNAL_PRIORITY);
    }

    void ReleseOwneshipFlag() noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::CURRENTLY_OWNED, NO_VAL, MAX_PRIORITY);
    }

    bool IsBranchOwnedByFlag() noexcept
    {
        uint32_t owned_cell_value = ReadMetaCellValue32(MetaIndexOfAPCBranch::CURRENTLY_OWNED);
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
            val32_t current_total_cas_failure = ReadMetaCellValue32(MetaIndexOfAPCBranch::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH);
            if (current_total_cas_failure == BRANCH_SENTINAL)
            {
                return BRANCH_SENTINAL;
            }
            
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCBranch::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, current_total_cas_failure, current_total_cas_failure + increment))
            {
                return current_total_cas_failure + increment;
            }   
        }
    }

    void ResetTotalCASFailureForThisBranch(tag8_t priority = DEFAULT_INTERNAL_PRIORITY) noexcept
    {
        WriteBrenchMeta32_(MetaIndexOfAPCBranch::TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH, NO_VAL, priority);
    }
};

}
