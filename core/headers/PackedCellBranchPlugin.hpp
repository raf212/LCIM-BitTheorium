#pragma once
#include "PackedCell.hpp"
#include "MasterClockConf.hpp"

namespace PredictedAdaptedEncoding
{

class PackedCellBranchPlugin final
{
public:
    static constexpr size_t META_CELLS = 64;
    static constexpr uint32_t BRANCH_MAGIC = 0x41504342u;
    static constexpr uint32_t BRANCH_VERSION = 1u;

    enum class TreePosition : uint32_t
    {
        ROOT = 0,
        LEFT = 1,
        RIGHT = 2,
        TREE_OVERFLOW = 3
    };

    enum class MetaIndexOfAPCBranch : size_t 
    {
        MAGIC_ID = 0,
        VERSION = 1,
        CAPACITY = 2,
        BRANCH_ID = 3,
        PARENT_BRANCH_ID = 4,
        LEFT_CHILD_ID = 5,
        RIGHT_CHILD_ID = 6,
        CURRENT_ACTIVE_THREADS = 7,
        BRANCH_DEPTH = 8,
        BRANCH_PRIORITY = 9,
        CURRENT_TREE_POSITION = 10,
        FLAGS = 11,
        PAYLOAD_BEGIN = 12,
        PAYLOAD_ENS = 13,
        OCCUPANCY_ANAPSHOT = 14,
        LOCAL_CLOCK48 = 15,
        LAST_SPLIT_EPOCH = 16,
        REGION_SIZE = 17,
        REGION_COUNT = 18,
        READY_REL_MASK = 19,
        SPLIT_THRESHOLD_PERCENTAGE = 20,
        MAX_DEPTH = 21,
        RESERVED_UPTO = 63
    };

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
        tag8_t priority = ZERO_PRIORITY
    ) noexcept
    {
        size_t index = static_cast<size_t>(idx);
        if (!ValidMeteIdx_(idx))
        {
            return;
        }
        PackedCellContainerPtr_[index].store(PackValue32InPackedCellwithClock16(value32, priority, PackedCellLocalityTypes::ST_PUBLISHED, REL_NONE), MoStoreSeq_);
        PackedCellContainerPtr_[index].notify_all();
    }

    void WriteOrUpdateMetaClock48(tag8_t priority = ZERO_PRIORITY, std::optional<uint64_t>meta_clock_48) noexcept
    {
        size_t idx = static_cast<size_t>(MetaIndexOfAPCBranch::LOCAL_CLOCK48);
        packed64_t wanted_cell = PackPureClock48AsPackedCell(meta_clock_48, priority, PackedCellLocalityTypes::ST_PUBLISHED);
        PackedCellContainerPtr_[idx].store(wanted_cell, MoStoreSeq_);
        PackedCellContainerPtr_[idx].notify_all();
    }

private:
    std::atomic<packed64_t>* PackedCellContainerPtr_{nullptr};
    size_t BranchCapacity_{0};
    MasterClockConf* MasterClockConfPtr_{nullptr};

    inline bool ValidMeteIdx_(MetaIndexOfAPCBranch idx) const noexcept
    {
        return PackedCellContainerPtr_ && static_cast<size_t>(idx) < BranchCapacity_ && static_cast<size_t>(idx) < META_CELLS;
    }

public:
    PackedCellBranchPlugin() noexcept = default;

    void Bind(std::atomic<packed64_t>* packed_cells, size_t capacity, MasterClockConf* master_clock_ptr) noexcept
    {
        PackedCellContainerPtr_ = packed_cells;
        BranchCapacity_ = capacity;
        MasterClockConfPtr_ = master_clock_ptr;
    }

    bool IsBound() const noexcept
    {
        return PackedCellContainerPtr_ != nullptr && BranchCapacity_ >= META_CELLS;
    }

    size_t PayloadBegain() const noexcept
    {
        return META_CELLS;
    }

    size_t PayloadCapacity() const noexcept
    {
        return BranchCapacity_ > META_CELLS ? (BranchCapacity_ - META_CELLS) : 0u;
    }

    //continue here
    void InitRootBranch(
        uint64_t branch_id,
        size_t total_capacity,
        uint32_t split_threshold_percantage,
        uint32_t max_depth,
        uint32_t region_size = 0,
        uint32_t region_count = 0,
        uint8_t priority = ZERO_PRIORITY
    ) noexcept
    {

    }

};

}
