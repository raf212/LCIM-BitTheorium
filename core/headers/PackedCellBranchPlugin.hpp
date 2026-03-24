#pragma once
#include "PackedCell.hpp"
#include "MasterClockConf.hpp"

namespace PredictedAdaptedEncoding
{

class PackedCellBranchPlugin final
{
public:
    static constexpr size_t METACELL_COUNT = 64;
    static constexpr uint32_t BRANCH_MAGIC = 0x41504342u;//big-endian
    static constexpr uint32_t EOF_HEADER = 0x72616600;//big-endian
    static constexpr uint32_t BRANCH_VERSION = 1u;
    static constexpr uint32_t BRANCH_SENTINAL = UINT32_MAX;

    enum class TreePosition : uint32_t
    {
        ROOT = 0,
        LEFT = 1,
        RIGHT = 2,
        TREE_OVERFLOW = 3
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
        CURRENT_TREE_POSITION = 7,
        // runtime control
        BRANCH_DEPTH = 8,
        BRANCH_PRIORITY = 9,
        FLAGS = 10,
        CURRENT_ACTIVE_THREADS = 11,
        OCCUPANCY_SNAPSHOT = 12,
        SAFE_POINT = 13,
        SPLIT_THRESHOLD_PERCENTAGE = 14,
        MAX_DEPTH = 15,
        //payload bounds
        PAYLOAD_BEGIN = 16,
        PAYLOAD_END = 17,
        //timing
        LOCAL_CLOCK48 = 18,
        LAST_SPLIT_EPOCH = 19,
        //region summery
        REGION_SIZE = 20,
        REGION_COUNT = 21,
        READY_REL_MASK = 22,

        //free exception 
        RESERVED_23 = 23,
        EOF_APC_HEADER = 63
    };
private:
    std::atomic<packed64_t>* PackedCellContainerPtr_{nullptr};
    size_t BranchCapacity_{0};
    MasterClockConf* MasterClockConfPtr_{nullptr};

    inline bool ValidMeteIdx_(MetaIndexOfAPCBranch idx) const noexcept
    {
        return PackedCellContainerPtr_ && static_cast<size_t>(idx) < BranchCapacity_ && static_cast<size_t>(idx) < METACELL_COUNT;
    }
public:
    //checked top
    inline packed64_t PackValue32InPackedCellwithClock16(
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

    void WriteBrenchMeta32(
        MetaIndexOfAPCBranch idx,
        uint32_t value32,
        tag8_t priority = ZERO_PRIORITY,
        tag8_t rel_mask4 = REL_MASK4_NONE
    ) noexcept
    {
        size_t index = static_cast<size_t>(idx);
        if (!ValidMeteIdx_(idx))
        {
            return;
        }
        PackedCellContainerPtr_[index].store(PackValue32InPackedCellwithClock16(value32, priority, PackedCellLocalityTypes::ST_PUBLISHED, rel_mask4), MoStoreSeq_);
        PackedCellContainerPtr_[index].notify_all();
    }

    void WriteOrUpdateMetaClock48(tag8_t priority = ZERO_PRIORITY, std::optional<uint64_t>meta_clock_48 = std::nullopt) noexcept
    {
        size_t idx = static_cast<size_t>(MetaIndexOfAPCBranch::LOCAL_CLOCK48);
        packed64_t wanted_cell = PackPureClock48AsPackedCell(meta_clock_48, priority, PackedCellLocalityTypes::ST_PUBLISHED);
        PackedCellContainerPtr_[idx].store(wanted_cell, MoStoreSeq_);
        PackedCellContainerPtr_[idx].notify_all();
    }

    inline bool CASMeta32(
        MetaIndexOfAPCBranch idx,
        uint32_t expected_value,
        uint32_t desired_value,
        tag8_t priority = ZERO_PRIORITY,
        tag8_t rel_mask = REL_MASK4_NONE
    ) noexcept
    {
        if (!ValidMeteIdx_(idx))
        {
            return false;
        }
        const size_t index = static_cast<size_t>(idx);
        packed64_t expected_packed = PackedCellContainerPtr_[index].load(MoLoad_);
        if (PackedCell64_t::ExtractValue32(expected_packed) != expected_value)
        {
            return false;
        }

        packed64_t desired_packed = PackValue32InPackedCellwithClock16(desired_value, priority, PackedCellLocalityTypes::ST_PUBLISHED, rel_mask, RelOffsetMode32::RELOFFSET_GENERIC_VALUE, PackedCellDataType::UnsignedPCellDataType);
        
        return PackedCellContainerPtr_[index].compare_exchange_strong(expected_packed, desired_packed, EXsuccess_, EXfailure_);
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

    size_t PayloadBegain() const noexcept
    {
        return METACELL_COUNT;
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
        uint32_t split_threshold_percantage,
        uint32_t current_depth,
        uint32_t max_depth,
        uint32_t region_size = 0,
        uint32_t region_count = 0,
        uint8_t branch_priority = ZERO_PRIORITY,
        uint8_t priority = ZERO_PRIORITY
    ) noexcept
    {
        if (!IsBound())
        {
            return;
        }
        
        WriteBrenchMeta32(MetaIndexOfAPCBranch::MAGIC_ID, BRANCH_MAGIC, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::VERSION, BRANCH_VERSION, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::CAPACITY, static_cast<uint32_t>(std::min<size_t>(total_capacity, UINT32_MAX)), priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::BRANCH_ID, static_cast<uint32_t>(std::min<uint32_t>(branch_id, UINT32_MAX)), priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::PARENT_BRANCH_ID, static_cast<uint32_t>(std::min<uint32_t>(parent_bramch_id, UINT32_MAX)), priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::LEFT_CHILD_ID, BRANCH_SENTINAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::RIGHT_CHILD_ID, BRANCH_SENTINAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::CURRENT_TREE_POSITION, static_cast<uint32_t>(current_tree_position), priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::BRANCH_DEPTH, current_depth, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::BRANCH_PRIORITY, branch_priority, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::FLAGS, NO_VAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::CURRENT_ACTIVE_THREADS, NO_VAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT, NO_VAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::SAFE_POINT, NO_VAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::SPLIT_THRESHOLD_PERCENTAGE, split_threshold_percantage, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::MAX_DEPTH, max_depth, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::PAYLOAD_BEGIN, static_cast<uint32_t>(METACELL_COUNT), priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::PAYLOAD_END, static_cast<uint32_t>(std::min<size_t>(total_capacity, UINT32_MAX)), priority);
        WriteOrUpdateMetaClock48(priority, NO_VAL);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::LAST_SPLIT_EPOCH, NO_VAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::REGION_SIZE, region_size, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::REGION_COUNT, region_count, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::READY_REL_MASK, NO_VAL, priority);
        WriteBrenchMeta32(MetaIndexOfAPCBranch::EOF_APC_HEADER, EOF_HEADER, priority);

    }

    val32_t ReadMetaCellValue(MetaIndexOfAPCBranch idx) noexcept
    {
        if (!ValidMeteIdx_(idx) || idx == MetaIndexOfAPCBranch::LOCAL_CLOCK48)
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
        
        return CASMeta32(current_tree_position, BRANCH_SENTINAL, child_id);
    }

};

}
