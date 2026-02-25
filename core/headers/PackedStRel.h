#pragma once

#include <type_traits>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <optional>
#include <vector>
#include <algorithm>
#include <limits>
#include <cassert>
#include <cstddef>
#include <array>
#include <thread>
#include <stdexcept>
#include <memory>
#include <bit>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace AtomicCScompact {
    using packed64_t = uint64_t;
    using val32_t    = uint32_t;
    using clk16_t    = uint16_t;
    using tag8_t     = uint8_t;
    using strl16_t   = uint16_t;

    #define NO_VAL 0u
    #define ZERO_PRIORITY 0u
    #define MAX_VAL 64u
    #define LN_OF_BYTE_IN_BITS 8u
    #define MASK_8_BIT  0xFFu
    #define RELATION_MASK_5 0x1Fu
    #define RELATION_PRIORITY 0x07u
    #define ID_HASH_GOLDEN_CONST 0x9E3779B97F4A7C15ull 
    #define ATOMIC_THRESHOLD 64u
    #define DEFAULT_INTERNAL_PRIORITY 8u
    #define PTR_HIGH16 16u
    #define DEFAULT_PAIRED_HEAD_HALF_PRIORITY 10u

    static constexpr unsigned MASK16B_HIGH8B_0 = 0xFF00u;

    static constexpr ::std::memory_order MoLoad_      = ::std::memory_order_acquire;
    static constexpr ::std::memory_order MoStoreSeq_  = ::std::memory_order_release;
    static constexpr ::std::memory_order MoStoreUnSeq_= ::std::memory_order_relaxed;
    static constexpr ::std::memory_order EXsuccess_   = ::std::memory_order_acq_rel;
    static constexpr ::std::memory_order EXfailure_   = ::std::memory_order_relaxed;

    static constexpr unsigned CLK_B48 = 48u;
    static constexpr unsigned VALBITS  = 32u;
    static constexpr unsigned CLK_B16  = 16u;
    static constexpr unsigned STRL_B16  = 16u;
    static constexpr unsigned STBITS   = 8u;
    static constexpr unsigned RELBITS  = 8u;
    static constexpr unsigned TOTAL_LOW = 48u;
    static constexpr unsigned MASK_OF_RELBIT = 5u;
    static constexpr unsigned CLK48TO16_PACKED_ERROR = 16u;

    //STRL->[priority->4 | locality->3 | PackedCell Type->1 | relmask->4 | reloffset->2 | celldatatype->2 ]-> = 16 bit->Bit distribution = [12 | 9 | 8 | 4 | 2 | 0 ]
    static constexpr unsigned PRIO_LEN = 4u;
    static constexpr unsigned LOCALITY_LEN = 3u;
    static constexpr unsigned PCTYPE_LEN = 1u;
    static constexpr unsigned RELMASK_LEN = 4u;
    static constexpr unsigned RELOFFSET_LEN = 2u;
    static constexpr unsigned PCELL_DATATYPE_LEN = 2u;

    //shifts 
    static constexpr unsigned PCELL_DETATYPE_SHIFT = 0u;
    static constexpr unsigned RELOFFSET_SHIFT = PCELL_DETATYPE_SHIFT + PCELL_DATATYPE_LEN;
    static constexpr unsigned RELMASK_SHIFT = RELOFFSET_SHIFT + RELOFFSET_LEN;
    static constexpr unsigned PCTYPE_SHIFT = RELMASK_SHIFT + RELMASK_LEN;
    static constexpr unsigned LOCALITY_SHIFT = PCTYPE_SHIFT + PCTYPE_LEN;
    static constexpr unsigned PRIORITY_SHIFT = LOCALITY_SHIFT + LOCALITY_LEN;
    //mask
    static constexpr tag8_t PCELL_DATATYPE_MASK = static_cast<tag8_t>((1u << PCELL_DATATYPE_LEN) - 1u);
    static constexpr tag8_t RELOFFSET_MASK = static_cast<tag8_t>((1u << RELOFFSET_LEN) - 1u);
    static constexpr tag8_t RELMASK_MASK = static_cast<tag8_t>((1u << RELMASK_LEN) - 1u);
    static constexpr tag8_t PCTYPE_MASK = static_cast<tag8_t>((1u << PCTYPE_LEN) - 1u);
    static constexpr tag8_t LOCALITY_MASK = static_cast<tag8_t>((1u << LOCALITY_LEN) - 1u);
    static constexpr tag8_t PRIORITY_MASK = static_cast<tag8_t>((1u << PRIO_LEN) - 1u);
    
    //priority(4 bit)
    static constexpr tag8_t PRIORITY_MIN = 0;
    static constexpr uint8_t MAX_PRIORITY   = static_cast<tag8_t>(PRIORITY_MASK);

    // locality (4-bit)
    static constexpr tag8_t ST_IDLE        = 0x0;
    static constexpr tag8_t ST_PUBLISHED   = 0x1;
    static constexpr tag8_t ST_EXCEPTION_BIT_FAULTY = 0x2;
    static constexpr tag8_t ST_CLAIMED     = 0x3;
    static constexpr tag8_t ST_PROCESSING  = 0x4;
    static constexpr tag8_t ST_COMPLETE    = 0x5;
    static constexpr tag8_t ST_RETIRED     = 0x6;
    static constexpr tag8_t ST_EPOCH_BUMP  = 0x7;

    //Relation(4 + 4) = 8 bit
    static constexpr tag8_t REL_NONE        = 0x00;
    static constexpr tag8_t REL_NODE0       = 0x00;
    static constexpr tag8_t REL_NODE1       = 0x01;
    static constexpr tag8_t REL_PAGE        = 0x04;
    static constexpr tag8_t REL_PATTERN     = 0x08;
    static constexpr tag8_t REL_SELF        = 0x01; // reused
    static constexpr tag8_t REL_ALL_LOW_4   = static_cast<tag8_t>(RELMASK_MASK);

    //reloffset encodings 
    static constexpr tag8_t RELOFFSET_GENERIC_VALUE = 0;
    static constexpr tag8_t RELOFFSET_TAIL_PTR = 1;
    static constexpr tag8_t REL_OFFSET_HEAD_PTR = 2;
    static constexpr tag8_t REL_OFFSET_STANDALONE48 = 3;
    enum class PackedCellDataType : unsigned
    {
        UnsignedPCellDataType = 0,
        IntPCellDataType = 1,
        FloatPCellDataType = 2,
        CharPCellDataType = 3
    };

    enum class PackedMode : int
    {
        MODE_VALUE32 = 0,
        MODE_CLKVAL48 = 1
    };

    enum class RelOffsetMode : unsigned
    {
        RELOFFSET_GENERIC_VALUE = 0,
        RELOFFSET_TAIL_PTR = 1,
        REL_OFFSET_HEAD_PTR = 2,
        REL_OFFSET_STANDALONE48 = 3
    };
    
    template <typename pcdt32>
    static inline PackedCellDataType PCellTypeCheckUser()
    {
        static_assert(std::is_trivially_copyable_v<pcdt32>, "Passed value Must be Trivially Copyable ");
        PackedCellDataType expected_pcdt;
        if constexpr (std::is_floating_point_v<pcdt32>)
        {
            expected_pcdt = PackedCellDataType::FloatPCellDataType;
        }
        else if constexpr (std::is_integral_v<pcdt32> && std::is_signed_v<pcdt32> 
            && !std::is_same_v<pcdt32, char> && !std::is_same_v<pcdt32, signed char>
        )
        {
            expected_pcdt = PackedCellDataType::IntPCellDataType;
        }
        else if constexpr (std::is_integral_v<pcdt32> && std::is_unsigned_v<pcdt32> 
            && !std::is_same_v<pcdt32, unsigned char>
        )
        {
            expected_pcdt = PackedCellDataType::UnsignedPCellDataType;
        }
        else if constexpr (std::is_same_v<pcdt32, char> || std::is_same_v<pcdt32, signed char> || std::is_same_v<pcdt32, unsigned char>)
        {
            expected_pcdt = PackedCellDataType::CharPCellDataType;
        }
        else
        {
            expected_pcdt = PackedCellDataType::UnsignedPCellDataType;
        }
        return expected_pcdt;
    }

    inline constexpr strl16_t MakeSTRL4_t(tag8_t priority, tag8_t locality, tag8_t rel_mask, tag8_t rel_offset, tag8_t pc_type = 0, PackedCellDataType pc_datatype = PackedCellDataType::UnsignedPCellDataType) noexcept
    {

        assert(pc_type <= 1u);
        assert(rel_offset <= 3u);
        strl16_t prio = static_cast<strl16_t>(priority & PRIORITY_MASK);
        strl16_t loc = static_cast<strl16_t>(locality & LOCALITY_MASK);
        strl16_t pctype = static_cast<strl16_t>(pc_type & PCTYPE_MASK);
        strl16_t rm = static_cast<strl16_t>(rel_mask & RELMASK_MASK);
        strl16_t ro = static_cast<strl16_t>(rel_offset & RELOFFSET_MASK);
        strl16_t pcdt = static_cast<strl16_t>(static_cast<unsigned>(pc_datatype) & PCELL_DATATYPE_MASK);

        strl16_t strl = static_cast<strl16_t>(
            (prio << (PRIORITY_SHIFT))
            | (loc << LOCALITY_SHIFT)
            | (pctype << PCTYPE_SHIFT)
            | (rm << RELMASK_SHIFT)
            | (ro << RELOFFSET_SHIFT)
            | pcdt
        );
        return strl;
    }

    inline constexpr tag8_t ExtractPriorityFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> PRIORITY_SHIFT) & PRIORITY_MASK);
    }
    
    inline constexpr tag8_t ExtractLocalityFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> LOCALITY_SHIFT) & LOCALITY_MASK);
    }

    inline constexpr tag8_t ExtractPCellTypeFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> PCTYPE_SHIFT) & PCTYPE_MASK);
    }

    inline constexpr tag8_t ExtractRelMaskFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> RELMASK_SHIFT) & RELMASK_MASK);
    }

    inline constexpr tag8_t ExtractRelOffsetFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> RELOFFSET_SHIFT) & RELOFFSET_MASK);
    }

    inline constexpr PackedCellDataType ExtractPCellDataTypeFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<PackedCellDataType>((strl >> PCELL_DETATYPE_SHIFT) & PCELL_DATATYPE_MASK);
    }

    inline constexpr int8_t DecodeRelOffsetSigned(tag8_t reloffset) noexcept
    {
        tag8_t r = reloffset & RELOFFSET_MASK;
        if (r & (1u << (RELOFFSET_LEN - 1)))
        {
            return static_cast<tag8_t>(static_cast<int8_t>(r) | static_cast<int8_t>(~((1 << RELOFFSET_LEN) - 1)));
        }
        return static_cast<int8_t>(r);
    }

    inline constexpr bool DoseRelMatch(tag8_t slot_relbyte, tag8_t relmask) noexcept
    {
        tag8_t slot_rm = static_cast<tag8_t>((slot_relbyte >> RELOFFSET_SHIFT) & RELMASK_MASK);
        return ((slot_rm & (relmask & RELMASK_MASK)) != 0);
    }
    

    template <typename To, typename From>
    inline To BitCastMaybe(const From& from_address)
    {
        To out;
        if constexpr (sizeof(To) == sizeof(From))
        {
            return std::bit_cast<To>(from_address);
        }
        else if constexpr (sizeof(To) < sizeof(From))
        {
            std::memcpy(&out, &from_address, sizeof(To));
            return out;
        }
        else
        {
            std::memset(&out, 0, sizeof(To));
            std::memcpy(&out, &from_address, sizeof(From));
            return out;
        }
    }

}