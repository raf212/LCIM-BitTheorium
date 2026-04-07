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
//STRL->[priority->4 | locality->3 | PackedCell Type->1 | relmask->4 | reloffset->2 | celldatatype->2 ]-> = 16 bit->Bit distribution = [12 | 9 | 8 | 4 | 2 | 0 ]
//clk16 =>16 Bits


namespace PredictedAdaptedEncoding {
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
    #define ID_HASH_GOLDEN_CONST 0x9E3779B97F4A7C15ull 
    #define DEFAULT_INTERNAL_PRIORITY 8u
    #define DEFAULT_PAIRED_HEAD_HALF_PRIORITY 10u
    #define MAX_TRIES 128
    #define SIZE_OF_MODE_48 6u // 6 * 8 = 48

    static constexpr ::std::memory_order MoLoad_      = ::std::memory_order_acquire;
    static constexpr ::std::memory_order MoStoreSeq_  = ::std::memory_order_release;
    static constexpr ::std::memory_order MoStoreUnSeq_= ::std::memory_order_relaxed;
    static constexpr ::std::memory_order OnExchangeSuccess   = ::std::memory_order_acq_rel;
    static constexpr ::std::memory_order OnExchangeFailure   = ::std::memory_order_relaxed;

    static constexpr unsigned CLK_B48 = 48u;
    static constexpr unsigned VALBITS  = 32u;
    static constexpr unsigned CLK_B16  = 16u;
    static constexpr unsigned STRL_B16  = 16u;
    static constexpr unsigned STBITS   = 8u;
    static constexpr unsigned TOTAL_LOW = 48u;

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
    
    static constexpr tag8_t PRIORITY_MIN = 0;
    static constexpr uint8_t MAX_PRIORITY   = static_cast<tag8_t>(PRIORITY_MASK);

    enum class PackedCellLocalityTypes : tag8_t
    {
        ST_IDLE = 0,
        ST_PUBLISHED = 1,
        ST_CLAIMED = 2,
        ST_EXCEPTION_BIT_FAULTY = 3
    };

    //Relation(4 + 4) = 8 bit
    static constexpr tag8_t REL_NONE        = 0x00;
    static constexpr tag8_t REL_NODE0       = 0x00;
    static constexpr tag8_t REL_NODE1       = 0x01;
    static constexpr tag8_t REL_PAGE        = 0x04;
    static constexpr tag8_t REL_PATTERN     = 0x08;
    static constexpr tag8_t REL_SELF        = 0x01; // reused
    static constexpr tag8_t REL_ALL_LOW_4   = static_cast<tag8_t>(RELMASK_MASK);
    static constexpr tag8_t REL_MASK4_NONE = 0;

    enum class PackedCellDataType : unsigned
    {
        UnsignedPCellDataType = 0,
        IntPCellDataType = 1,
        FloatPCellDataType = 2,
        CharPCellDataType = 3
    };

    enum class PackedMode : tag8_t
    {
        MODE_VALUE32 = 0,
        MODE_CLKVAL48 = 1
    };

    enum class RelOffsetMode32 : tag8_t
    {
        RELOFFSET_GENERIC_VALUE = 0,
        RELOFFSET_TAIL_PTR = 1,
        REL_OFFSET_HEAD_PTR = 2,
        RESERVED = 3
    };

    enum class RelOffsetMode48 : tag8_t
    {
        RELOFFSET_GENERIC_VALUE = 0,
        RELOFFSET_PURE_TIMER = 1,
        RELOFFSET_STANDALONE_PTR = 2,
        RESERVED = 3
    };

    template<typename PCDT>
    struct PackedCellTypeBridge
    {
        static_assert(std::is_trivially_copyable_v<PCDT>, "Packed Cell allowes Trivially copyable 32/48 bit unsigned, int, float, char");
        using Decayed = std::remove_cv_t<std::remove_reference_t<PCDT>>;
        static constexpr bool IS_CHAR_LIKE = std::is_same_v<Decayed, char> || std::is_same_v<Decayed, signed char> || std::is_same_v<Decayed, unsigned char>;
        static constexpr bool IS_FLOAT_LIKE = std::is_floating_point_v<Decayed>;
        static constexpr bool IS_SIGNED_LIKE = std::is_integral_v<Decayed> && std::is_signed_v<Decayed> && !IS_CHAR_LIKE;
        static constexpr bool IS_UNSIGNED_LIKE = std::is_integral_v<Decayed> && std::is_unsigned_v<Decayed> && !IS_CHAR_LIKE;


        static constexpr PackedCellDataType DType  = 
            IS_FLOAT_LIKE           ? PackedCellDataType::FloatPCellDataType    :
            IS_SIGNED_LIKE          ? PackedCellDataType::IntPCellDataType      :
            IS_UNSIGNED_LIKE        ? PackedCellDataType::UnsignedPCellDataType :
            IS_CHAR_LIKE            ? PackedCellDataType::CharPCellDataType     :
                                    PackedCellDataType::UnsignedPCellDataType   ;
        static constexpr bool FITS_MODE_32 = (sizeof(Decayed) <= sizeof(val32_t));
        static constexpr bool FITS_MODE_48 = (sizeof(Decayed) <= SIZE_OF_MODE_48);
        static constexpr bool IS_SUPPORTED_TYPE = IS_FLOAT_LIKE || IS_SIGNED_LIKE || IS_UNSIGNED_LIKE || IS_CHAR_LIKE;

    };

    template<typename PCDT>
    inline constexpr PackedCellDataType BridgeOfPackedCellDataType_v = PackedCellTypeBridge<PCDT>::DType;

    inline constexpr strl16_t MakeSTRL4_t(tag8_t priority, PackedCellLocalityTypes locality, tag8_t rel_mask, tag8_t rel_offset, PackedMode pc_type = PackedMode::MODE_VALUE32, PackedCellDataType pc_datatype = PackedCellDataType::UnsignedPCellDataType) noexcept
    {

        strl16_t prio = static_cast<strl16_t>(priority & PRIORITY_MASK);
        strl16_t loc = static_cast<strl16_t>(static_cast<tag8_t>(locality) & LOCALITY_MASK);
        strl16_t pctype = static_cast<strl16_t>(static_cast<tag8_t>(pc_type) & PCTYPE_MASK);
        strl16_t rm = static_cast<strl16_t>(rel_mask & RELMASK_MASK);
        strl16_t ro = static_cast<strl16_t>(static_cast<tag8_t>(rel_offset) & RELOFFSET_MASK);
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

    inline constexpr strl16_t MakeSTRLMode32_t(tag8_t priority, PackedCellLocalityTypes locality, tag8_t rel_mask, RelOffsetMode32 rel_offset, PackedCellDataType pc_datatype = PackedCellDataType::UnsignedPCellDataType) noexcept
    {
        return MakeSTRL4_t(priority, locality, rel_mask, static_cast<tag8_t>(rel_offset), PackedMode::MODE_VALUE32, pc_datatype);
    }

    inline constexpr strl16_t MakeStrl4ForMode48_t (tag8_t priority, PackedCellLocalityTypes locality, tag8_t rel_mask, RelOffsetMode48 rel_offset, PackedCellDataType pc_datatype = PackedCellDataType::UnsignedPCellDataType) noexcept
    {
        return MakeSTRL4_t(priority, locality, rel_mask, static_cast<tag8_t>(rel_offset), PackedMode::MODE_CLKVAL48, pc_datatype);
    }

    inline constexpr tag8_t ExtractPriorityFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<tag8_t>((strl >> PRIORITY_SHIFT) & PRIORITY_MASK);
    }
    
    inline constexpr PackedCellLocalityTypes ExtractLocalityFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<PackedCellLocalityTypes>((strl >> LOCALITY_SHIFT) & LOCALITY_MASK);
    }

    inline constexpr PackedMode ExtractPCellTypeFromSTRL(strl16_t strl) noexcept
    {
        return static_cast<PackedMode>((strl >> PCTYPE_SHIFT) & PCTYPE_MASK);
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