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
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <memory>
#include <latch> // For std::latch (C++20)

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
    #define MAX_VAL 64u
    #define LN_OF_BYTE_IN_BITS 8u
    #define MASK_8_BIT  0xFFu
    #define RELATION_MASK_5 0x1Fu
    #define RELATION_PRIORITY 0x07u
    #define ID_HASH_GOLDEN_CONST 0x9E3779B97F4A7C15ull 
    #define ATOMIC_THRESHOLD 64u
    #define DEFAULT_INTERNAL_PRIORITY 8u

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

    //STRL->[priority->4 | locality->3 | PackedCell Type->1 | relmask->4 | reloffset->4 ]-> = 16 bit->Bit distribution = [12 | 9 | 8 | 4 | 0 ]
    static constexpr unsigned STRL_DIVISION_CONST = 4u;
    static constexpr unsigned STRL_PRIO_AND_REL_CONST = 4u;//depricated
    static constexpr unsigned PCTYPE_CONST = 1u;
    static constexpr unsigned LOCALITY_CONST = 3u;
    //shifts 
    static constexpr unsigned RELOFFSET_SHIFT = 0u;
    static constexpr unsigned RELMASK_SHIFT = RELOFFSET_SHIFT + STRL_PRIO_AND_REL_CONST;
    static constexpr unsigned PCTYPE_SHIFT = RELMASK_SHIFT + STRL_PRIO_AND_REL_CONST;
    static constexpr unsigned LOCALITY_SHIFT = PCTYPE_SHIFT + PCTYPE_CONST;
    static constexpr unsigned PRIORITY_SHIFT = LOCALITY_SHIFT + LOCALITY_CONST;
    //mask
    static constexpr tag8_t STRL_DIVISION_MASK_4 = static_cast<tag8_t>((1u << STRL_DIVISION_CONST) - 1u); //depricated

    static constexpr tag8_t STRL_PRIO_REL_MASK_4 = static_cast<tag8_t>((1u << STRL_DIVISION_CONST) - 1u);
    static constexpr tag8_t PCTYPE_MASK = static_cast<tag8_t>((1u << PCTYPE_CONST) - 1u);
    static constexpr tag8_t LOCALITY_MASK = static_cast<tag8_t>((1u << LOCALITY_CONST) - 1u);
    
    //priority(4 bit)
    static constexpr tag8_t PRIORITY_MIN = 0;
    static constexpr uint8_t MAX_PRIORITY   = static_cast<tag8_t>(STRL_PRIO_REL_MASK_4);

    // locality (4-bit)
    static constexpr tag8_t ST_IDLE        = 0x0;
    static constexpr tag8_t ST_PUBLISHED   = 0x1;
    static constexpr tag8_t ST_PENDING     = 0x2;
    static constexpr tag8_t ST_CLAIMED     = 0x3;
    static constexpr tag8_t ST_PROCESSING  = 0x4;
    static constexpr tag8_t ST_COMPLETE    = 0x5;
    static constexpr tag8_t ST_RETIRED     = 0x6;
    static constexpr tag8_t ST_EPOCH_BUMP  = 0x7;

    //Relation(4 + 4) = 8 bit
    static constexpr tag8_t REL_NONE        = 0x00;
    static constexpr tag8_t REL_NODE0       = 0x01;
    static constexpr tag8_t REL_NODE1       = 0x02;
    static constexpr tag8_t REL_PAGE        = 0x04;
    static constexpr tag8_t REL_PATTERN     = 0x08;
    static constexpr tag8_t REL_SELF        = 0x01; // reused
    static constexpr tag8_t REL_ALL_LOW_4   = static_cast<tag8_t>(STRL_DIVISION_MASK_4);

    inline constexpr strl16_t MakeSTRL4_t(tag8_t priority, tag8_t locality, tag8_t rel_mask, tag8_t rel_offset, tag8_t pc_type = 0) noexcept
    {
        if (pc_type > 1u)
        {
            pc_type = 0u;
        }
        
        strl16_t prio = static_cast<strl16_t>(priority & STRL_PRIO_REL_MASK_4);
        strl16_t loc = static_cast<strl16_t>(locality & LOCALITY_MASK);
        strl16_t pctype = static_cast<strl16_t>(pc_type & PCTYPE_MASK);
        strl16_t rm = static_cast<strl16_t>(rel_mask & STRL_PRIO_REL_MASK_4);
        strl16_t ro = static_cast<strl16_t>(rel_offset & STRL_PRIO_REL_MASK_4);

        strl16_t strl = static_cast<strl16_t>(
            (prio << (PRIORITY_SHIFT))
            | (loc << LOCALITY_SHIFT)
            | (pctype << PCTYPE_SHIFT)
            | (rm << RELMASK_SHIFT)
            |ro
        );
        return strl;
    }

    inline constexpr tag8_t ExtractPriorityFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> PRIORITY_SHIFT) & STRL_PRIO_REL_MASK_4);
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
        return static_cast<tag8_t>((strl >> RELMASK_SHIFT) & STRL_PRIO_REL_MASK_4);
    }

    inline constexpr tag8_t ExtractRelOffsetFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> RELOFFSET_SHIFT) & STRL_PRIO_REL_MASK_4);
    }

    inline constexpr tag8_t MakeRelByte(tag8_t rel_mask, tag8_t rel_offset) noexcept
    {
        return static_cast<tag8_t>(
            (static_cast<tag8_t>(rel_mask & STRL_DIVISION_MASK_4) << STRL_DIVISION_CONST)
            | static_cast<tag8_t>(rel_offset & STRL_DIVISION_MASK_4)
        );
    }

    inline constexpr int8_t DecodeRelOffsetSigned(tag8_t reloffset) noexcept
    {
        tag8_t r = reloffset & STRL_DIVISION_MASK_4;
        if (r & (1u << (STRL_DIVISION_CONST -1)))
        {
            return static_cast<tag8_t>(static_cast<int8_t>(r) | static_cast<int8_t>(~((1 << STRL_DIVISION_CONST) - 1)));
        }
        return static_cast<int8_t>(r);
    }

    inline constexpr bool ISRelOffsetEscaped(tag8_t reloffset) noexcept
    {
        return ((reloffset & STRL_DIVISION_MASK_4) == STRL_DIVISION_MASK_4);
    }

    inline constexpr bool DoseRelMatch(tag8_t slot_relbyte, tag8_t relmask) noexcept
    {
        tag8_t slot_rm = static_cast<tag8_t>((slot_relbyte >> STRL_DIVISION_CONST) & STRL_DIVISION_MASK_4);
        return ((slot_rm & (relmask & STRL_DIVISION_MASK_4)) != 0);
    }
    
static constexpr tag8_t REL_ALL_LOW5    = 0x1F; // all low-5 relation bits

}