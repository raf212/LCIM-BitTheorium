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
    )
    {

        
    }
private:
    std::atomic<packed64_t>* PackedCellPtr_{nullptr};
    size_t BranchCapacity_{0};
    MasterClockConf* MasterClockConfPtr_{nullptr};

    inline bool ValidMeteIdx_(MetaIndexOfAPCBranch idx) const noexcept
    {
        return PackedCellPtr_ && static_cast<size_t>(idx) < BranchCapacity_ && static_cast<size_t>(idx) < META_CELLS;
    }

};

}
