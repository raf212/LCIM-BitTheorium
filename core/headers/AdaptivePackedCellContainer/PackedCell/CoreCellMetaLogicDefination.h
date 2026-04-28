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
#include "Defination.h"

#if defined(_MSC_VER)
    #include <intrin.h>
#endif
//(PNLTCOD)META16->[priority->3| Future Paged Node = 2 | locality->2 | PackedCell Type->1 | relmask->4 | reloffset->2 | celldatatype->2 ]-> = 16 bit->Bit distribution = [12 | 9 | 8 | 4 | 2 | 0 ]
//clk16 =>16 Bits


namespace PredictedAdaptedEncoding {
    using packed64_t = uint64_t;
    using val32_t    = uint32_t;
    using clk16_t    = uint16_t;
    using tag8_t     = uint8_t;
    using meta16_t   = uint16_t;



    static constexpr ::std::memory_order MoLoad_      = ::std::memory_order_acquire;
    static constexpr ::std::memory_order MoStoreSeq_  = ::std::memory_order_release;
    static constexpr ::std::memory_order MoStoreUnSeq_= ::std::memory_order_relaxed;
    static constexpr ::std::memory_order OnExchangeSuccess   = ::std::memory_order_acq_rel;
    static constexpr ::std::memory_order OnExchangeFailure   = ::std::memory_order_relaxed;

    static constexpr unsigned CLK_B48 = 48u;
    static constexpr unsigned VALBITS  = 32u;
    static constexpr unsigned CLK_B16  = 16u;
    static constexpr unsigned META16_B16  = 16u;
    static constexpr unsigned STBITS   = 8u;
    static constexpr unsigned TOTAL_LOW = 48u;

    static constexpr unsigned PRIO_LEN = 3u;
    static constexpr unsigned NODE_AUTH_LEN = 2u;
    static constexpr unsigned LOCALITY_LEN = 2u;// will be 2u
    static constexpr unsigned PCTYPE_LEN = 1u;
    static constexpr unsigned RELMASK_LEN = 4u;
    static constexpr unsigned RELOFFSET_LEN = 2u;
    static constexpr unsigned PCELL_DATATYPE_LEN = 2u;

    //shifts 
    static constexpr unsigned PCELL_DETATYPE_SHIFT = 0u;
    static constexpr unsigned RELOFFSET_SHIFT = PCELL_DETATYPE_SHIFT + PCELL_DATATYPE_LEN;
    static constexpr unsigned RELMASK_SHIFT = RELOFFSET_SHIFT + RELOFFSET_LEN;
    static constexpr unsigned CELL_MODE_SHIFT = RELMASK_SHIFT + RELMASK_LEN;
    static constexpr unsigned LOCALITY_SHIFT = CELL_MODE_SHIFT + PCTYPE_LEN;
    static constexpr unsigned NODE_AUTH_SHIFT = LOCALITY_SHIFT + LOCALITY_LEN;
    static constexpr unsigned PRIORITY_SHIFT = NODE_AUTH_SHIFT + NODE_AUTH_LEN;
    static_assert(PRIORITY_SHIFT + PRIO_LEN == META16_B16, "PNLTCOD must be 16 bits");
    //mask
    static constexpr tag8_t PCELL_DATATYPE_MASK = static_cast<tag8_t>((1u << PCELL_DATATYPE_LEN) - 1u);
    static constexpr tag8_t RELOFFSET_MASK = static_cast<tag8_t>((1u << RELOFFSET_LEN) - 1u);
    static constexpr tag8_t RELMASK_MASK = static_cast<tag8_t>((1u << RELMASK_LEN) - 1u);
    static constexpr tag8_t CELL_MODE_MASK = static_cast<tag8_t>((1u << PCTYPE_LEN) - 1u);
    static constexpr tag8_t LOCALITY_MASK = static_cast<tag8_t>((1u << LOCALITY_LEN) - 1u);
    static constexpr tag8_t NODE_AUTH_MASK = static_cast<tag8_t>((1u << NODE_AUTH_LEN) - 1u);
    static constexpr tag8_t PRIORITY_MASK = static_cast<tag8_t>((1u << PRIO_LEN) - 1u);
    
    static constexpr uint8_t MAX_PRIORITY   = static_cast<tag8_t>(PRIORITY_MASK);

    enum class PackedCellLocalityTypes : tag8_t
    {
        ST_IDLE = 0,
        ST_PUBLISHED = 1,
        ST_CLAIMED = 2,
        ST_EXCEPTION_BIT_FAULTY = 3
    };


    static constexpr tag8_t REL_ALL_LOW_4   = static_cast<tag8_t>(RELMASK_MASK);
    static constexpr tag8_t REL_MASK4_NONE = 0;

    enum class PackeCellNodeAuthority : tag8_t
    {
        LOCAL_OR_UNDEFINED = 0,
        CAUSAL_LINIAR_SAGMENT = 1,
        NEUROMORPHIC_PAGED_GRAPH = 2,
        BIDIRECTIONAL_NEUROMORPHIC_SYSTEM = 3
    };

    enum class PackedCellDataType : tag8_t
    {
        CharPCellDataType = 0,
        IntPCellDataType = 1,
        FloatPCellDataType = 2,
        UnsignedPCellDataType = 3
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

    enum class PriorityPhysics : tag8_t
    {
        IDLE = 0,
        DEFAULT_PRIORITY = 1,
        IMPORTANT = 2,
        URGENT = 3,
        HANDLE_NOW = 4,
        STRUCTURAL_DEPENDENCY = 5,
        TIME_DEPENDENCY = 6,
        ERROR_DEPENDENCY = 7
    };

    enum class APCPagedNodeRelMaskClasses : tag8_t
    {
        NONE = 0x0,
        FEEDFORWARD_MESSAGE  = 0x1,
        FEEDBACKWARD_MESSAGE = 0x2,
        LATERAL_MESAGE = 0x3,
        STATE_SLOT = 0x4,
        ERROR_SLOT = 0x5,
        EDGE_DESCRIPTOR = 0x6,
        WEIGHT_SLOT = 0x7,
        CONTROL_SLOT = 0x8,
        AUX_SLOT = 0x9,
        FREE_SLOT = 0xA,
        SELF_REFARANCE = 0xB,
        CLOCK_PURE_TIME = 0xC,
        RESERVED_14     = 0xD,
        COMPLEX_STORAGE = 0xE,
        NANNULL     = 0xF
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

    inline constexpr meta16_t MakeInCellMetaFromUnsigned_16t_(
        tag8_t priority, tag8_t node_authority,
        tag8_t locality, tag8_t rel_mask, 
        tag8_t rel_offset, tag8_t pc_type, 
        tag8_t pc_datatype
    ) noexcept
    {

        meta16_t cell_priority = static_cast<meta16_t>(static_cast<tag8_t>(priority) & PRIORITY_MASK);
        meta16_t cell_authority = static_cast<meta16_t>(static_cast<tag8_t>(node_authority) & NODE_AUTH_MASK); 
        meta16_t cell_locality = static_cast<meta16_t>(static_cast<tag8_t>(locality) & LOCALITY_MASK);
        meta16_t cell_mode = static_cast<meta16_t>(static_cast<tag8_t>(pc_type) & CELL_MODE_MASK);
        meta16_t relation_mask = static_cast<meta16_t>(rel_mask & RELMASK_MASK);
        meta16_t relation_offset = static_cast<meta16_t>(static_cast<tag8_t>(rel_offset) & RELOFFSET_MASK);
        meta16_t cell_data_type = static_cast<meta16_t>(static_cast<unsigned>(pc_datatype) & PCELL_DATATYPE_MASK);

        meta16_t cell_meta = static_cast<meta16_t>(
            (cell_priority  << (PRIORITY_SHIFT))
            | (cell_authority << (NODE_AUTH_SHIFT))
            | (cell_locality << LOCALITY_SHIFT)
            | (cell_mode << CELL_MODE_SHIFT)
            | (relation_mask << RELMASK_SHIFT)
            | (relation_offset << RELOFFSET_SHIFT)
            | cell_data_type
        );
        return cell_meta;
    }

    inline constexpr meta16_t MakeInCellMetaForMode_32t(
        PriorityPhysics priority = PriorityPhysics::DEFAULT_PRIORITY, 
        PackeCellNodeAuthority authority = PackeCellNodeAuthority::LOCAL_OR_UNDEFINED,
        PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_IDLE,
        APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::FREE_SLOT,
        RelOffsetMode32 rel_offset_32 = RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
        PackedCellDataType cell_data_type = PackedCellDataType::UnsignedPCellDataType
    ) noexcept
    {
        return MakeInCellMetaFromUnsigned_16t_(
            static_cast<tag8_t>(priority),
            static_cast<tag8_t>(authority),
            static_cast<tag8_t>(locality),
            static_cast<tag8_t>(page_class),
            static_cast<tag8_t>(rel_offset_32),
            static_cast<tag8_t>(PackedMode::MODE_VALUE32),
            static_cast<tag8_t>(cell_data_type)
        );
    }

    inline constexpr meta16_t MakeInCellMetaForMode_48t(
        PriorityPhysics priority = PriorityPhysics::DEFAULT_PRIORITY, 
        PackeCellNodeAuthority authority = PackeCellNodeAuthority::LOCAL_OR_UNDEFINED,
        PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_IDLE,
        APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::FREE_SLOT,
        RelOffsetMode48 rel_offset_48 = RelOffsetMode48::RELOFFSET_GENERIC_VALUE,
        PackedCellDataType cell_data_type = PackedCellDataType::UnsignedPCellDataType
    ) noexcept
    {
        return MakeInCellMetaFromUnsigned_16t_(
            static_cast<tag8_t>(priority),
            static_cast<tag8_t>(authority),
            static_cast<tag8_t>(locality),
            static_cast<tag8_t>(page_class),
            static_cast<tag8_t>(rel_offset_48),
            static_cast<tag8_t>(PackedMode::MODE_CLKVAL48),
            static_cast<tag8_t>(cell_data_type)
        );
    }

    inline constexpr tag8_t ExtractPriorityFromMETA16_U_(meta16_t meta16) noexcept
    {
        return static_cast<tag8_t>((meta16 >> PRIORITY_SHIFT) & PRIORITY_MASK);
    }

    inline constexpr tag8_t ExtractCellLocalNodeAuthotityFromMETA16_U_(meta16_t meta16) noexcept
    {
        return static_cast<tag8_t>((meta16 >> NODE_AUTH_SHIFT ) & NODE_AUTH_MASK);
    }
    
    inline constexpr tag8_t ExtractLocalityFromMETA16_U_(meta16_t meta16) noexcept
    {
        return static_cast<tag8_t>((meta16 >> LOCALITY_SHIFT) & LOCALITY_MASK);
    }

    inline constexpr tag8_t ExtractCellModeFromMETA16_U_(meta16_t meta16) noexcept
    {
        return static_cast<tag8_t>((meta16 >> CELL_MODE_SHIFT) & CELL_MODE_MASK);
    }

    inline constexpr tag8_t ExtractRelMaskFromMETA16_U_(meta16_t meta16) noexcept
    {
        return static_cast<tag8_t>((meta16 >> RELMASK_SHIFT) & RELMASK_MASK);
    }

    inline constexpr tag8_t ExtractRelOffsetFromMETA16_U_(meta16_t meta16) noexcept
    {
        return static_cast<tag8_t>((meta16 >> RELOFFSET_SHIFT) & RELOFFSET_MASK);
    }

    inline constexpr PackedCellDataType ExtractValueDataTypeFromMETA16_U_(meta16_t meta16) noexcept
    {
        return static_cast<PackedCellDataType>((meta16 >> PCELL_DETATYPE_SHIFT) & PCELL_DATATYPE_MASK);
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