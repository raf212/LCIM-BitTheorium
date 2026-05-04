
#pragma once 
#include <array>
#include <utility>
#include "../PackedCell/CoreCellDefination.hpp"

namespace PredictedAdaptedEncoding
{
    enum class MetaIndexOfAPCNode : size_t
    {
        //identity
        MAGIC_ID = 0,
        MANAGER_CONTROL_FLAGS = 1,
        //logical-node Identity
        BRANCH_ID = 3,
        LOGICAL_NODE_ID = 4,
        SHARED_ID = 5,
        SHARED_PREVIOUS_ID = 6,
        SHARED_NEXT_ID = 7,

        //runtime-controle
        BRANCH_DEPTH = 8,
        BRANCH_PRIORITY = 9,
        SEGMENT_CONF_FLAGS = 10,
        CURRENT_ACTIVE_THREADS = 11,
            //occupancy
        OCCUPANCY_SNAPSHOT_OF_CLAIMED_CELLS = 2,
        OCCUPANCY_SNAPSHOT_OF_PUBLISHED_CELLS = 12,
        OCCUPANCY_SNAPSHOT_OF_IDLE_CELLS = 85,
        OCCUPANCY_SNAPSHOT_OF_FAULTY_CELLS = 86,
            //
        SPLIT_THRESHOLD_PERCENTAGE = 13,
        SEGMENT_KIND = 14,
        MAX_DEPTH = 15,

        //payload-Bounds
        TOTAL_CAPACITY_OF_THIS_SEGEMENT = 16,

        //timing
        LOCAL_CLOCK48 = 17,
        LAST_SPLIT_EPOCH = 18,

        //region summery
        REGION_DIR_COUNT = 19,
        REGION_SIZE = 20,
        REGION_COUNT = 21,
        PAGED_NODE_READY_BIT = 22,
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
        NODE_ROLE_FLAGS_RESERVED = 39,
        LAST_ACCEPTED_FEED_FORWARD_CLOCK16 = 40,
        LAST_EMITTED_FEED_FORWARD_CLOCK16 = 41,
        LAST_ACCEPTED_FEED_BACKWARD_CLOCK16 = 42,
        LAST_EMITTED_FEED_BACKWARD_CLOCK16 = 43,
        NODE_COMPUTE_KIND = 44,

        //payload--bounds
        MESSAGE_FEEDFORWARD_BEGAIN = 45,
        MESSAGE_FEEDFORWARD_END = 46,
        MESSAGE_FEEDBACKWARD_BEGAIN = 47,
        MESSAGE_FEEDBACKWARD_END = 48,
        STATE_BEGAINING = 49,
        STATE_END = 50,
        ERROR_BEGAIN = 51,
        ERROR_END = 52,
        EDGE_DESCRIPTIOR_BEGAIN = 53,
        EDGE_DESCRIPTIOR_END = 54,
        WEIGHT_BEGIN = 55,
        WEIGHT_END = 56,
        RESERVED_MESSAGE_1_BEGIN = 57,
        RESERVED_MESSAGE_1_END = 58,
        AUX_BEGAIN = 59,
        AUX_END = 60,
        FREE_BEGAIN = 61,
        FREE_END = 62,
        RESERVED_MESSAGE_2_BEGIN = 63,
        RESERVED_MESSAGE_2_END = 64,
        //end

        EDGE_TABLE_COUNT = 65,
        WEIGHT_TABLE_COUNT = 66,

        REGION_OCCUPANCY_NONE        = 67,
        REGION_OCCUPANCY_FF          = 68,
        REGION_OCCUPANCY_FB          = 69,
        REGION_OCCUPANCY_LATERAL     = 70,
        REGION_OCCUPANCY_STATE       = 71,
        REGION_OCCUPANCY_ERROR       = 72,
        REGION_OCCUPANCY_EDGE        = 73,
        REGION_OCCUPANCY_WEIGHT      = 74,
        REGION_OCCUPANCY_CONTROL     = 75,
        REGION_OCCUPANCY_AUX         = 76,
        REGION_OCCUPANCY_FREE        = 77,
        REGION_OCCUPANCY_MESSAGE_1      = 78,
        REGION_OCCUPANCY_MESSAGE_2      = 79,
        REGION_OCCUPANCY_MESSAGE_3      = 80,
        REGION_OCCUPANCY_MESSAGE_4      = 81,
        REGION_OCCUPANCY_NANNULL        = 82,

        RETIRE_EPOCH_LOW32     = 83,
        RETIRE_EPOCH_HIGH32    = 84,

        RESERVED_87 = 87,
        EOF_APC_HEADER = 95
    };


    class APCDataStructure
    {
    public:
        static constexpr size_t METACELL_COUNT = 96;
        static constexpr uint32_t BRANCH_MAGIC = 0x41504342u;//big-endian
        static constexpr uint32_t EOF_HEADER = 0x72616600;//big-endian
        static constexpr uint32_t BRANCH_VERSION = 1u;
        static constexpr packed64_t PACKED_CELL_SENTENAL = UINT64_MAX;
        static constexpr uint32_t APC_MAX_LENGTH_OR_COUNTER = UINT16_MAX - 1;
        static constexpr uint32_t APC_INDEX_SENTINAL = UINT16_MAX;
        static constexpr uint64_t MASK_LOW_16 = MaskLowNBits(16);


        static inline packed64_t Compose3Unsigned16bitIndependentInMode48(
            uint16_t low16_bits,
            uint16_t mid_16_bits,
            uint16_t high_16_bits,
            meta16_t meta16
        ) noexcept
        {
            if(static_cast<PackedMode>(PackedCell64_t::ExtractCellModeFromMETA16_U_(meta16)) != PackedMode::MODE_CLKVAL48)
            {
                return PackedCell64_t::MakeFaultyCell();
            }
            if (static_cast<RelOffsetMode48>(PackedCell64_t::ExtractRelOffsetFromMETA16_U_(meta16)) != RelOffsetMode48::THREE_16_BIT_SUB_DIVISION)
            {
                return PackedCell64_t::MakeFaultyCell();
            }

            packed64_t packed_cell = (PackUnsigned16x3ToMode48_(low16_bits, mid_16_bits, high_16_bits) & MaskLowNBits(CLK_B48));
            packed_cell = PackedCell64_t::SetMETA16InPacked(packed_cell, meta16);
            return packed_cell;
        }

        static inline std::optional<uint16_t> ExtractLow16FromPackedCellMode48(packed64_t packed_cell) noexcept
        {
            if (PackedCell64_t::ExtractRelOffset48FromPacked(packed_cell) != RelOffsetMode48::THREE_16_BIT_SUB_DIVISION)
            {
                return std::nullopt;
            }
            const uint64_t raw_value48 = PackedCell64_t::ExtractClk48(packed_cell);
            if (raw_value48 == PackedCell64_t::PACKED_CELL_SENTINAL)
            {
                return std::nullopt;
            }
            return ExtractLow16FromUnsigned48_(raw_value48);
        }

    protected:
        static constexpr unsigned PACK3XU16TOMODE48_SHIFT_LOW = 0u;
        static constexpr unsigned PACK3XU16TOMODE48_SHIFT_MID = 16u;
        static constexpr unsigned PACK3XU16TOMODE48_SHIFT_HIGH = 32u;
        
        static inline bool DoseU32FitsInU16_(uint32_t value) noexcept
        {
            return value <= APC_MAX_LENGTH_OR_COUNTER;
        }

        static inline uint64_t PackUnsigned16x3ToMode48_(
            uint16_t low16_bits,
            uint16_t mid_16_bits,
            uint16_t high_16_bits
        ) noexcept
        {
            return (uint64_t{low16_bits} << PACK3XU16TOMODE48_SHIFT_LOW)    |
                (uint64_t{mid_16_bits} << PACK3XU16TOMODE48_SHIFT_MID)      |
                (uint64_t(high_16_bits) << PACK3XU16TOMODE48_SHIFT_HIGH);
        }

        static inline uint16_t ExtractLow16FromUnsigned48_(uint64_t raw48) noexcept
        {
            return static_cast<uint16_t>((raw48 >> PACK3XU16TOMODE48_SHIFT_LOW) & MASK_LOW_16);
        }

        static inline uint16_t ExtractMid16FromUnsigned48_(uint64_t raw48) noexcept
        {
            return static_cast<uint16_t>((raw48 >> PACK3XU16TOMODE48_SHIFT_MID) & MASK_LOW_16);
        }

        static inline uint16_t ExtractHigh16FromUnsigned48_(uint64_t raw48) noexcept
        {
            return static_cast<uint16_t>((raw48 >> PACK3XU16TOMODE48_SHIFT_HIGH) & MASK_LOW_16);
        }

    };
    




  
    
}
