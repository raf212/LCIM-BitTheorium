#pragma once
#include "PackedCell.hpp"

namespace PredictedAdaptedEncoding
{
    constexpr unsigned STOP_CODE = 1u;
    
    enum class APCPortKind : uint8_t
    {
        SELF_APC = 0,
        FEED_FORWARD_IN = 1,
        FEED_FORWARD_OUT = 2,
        FEED_BACKWARD_IN = 3,
        FEED_BACKWARD_OUT = 4
    };

    static inline bool IsCellPublishedMode32Generic (packed64_t packed_cell) noexcept
    {
        return PackedCell64_t::ExtractModeOfPackedCellFromPacked(packed_cell) == PackedMode::MODE_VALUE32 && 
            PackedCell64_t::ExtractLocalityFromPacked(packed_cell) == PackedCellLocalityTypes::ST_PUBLISHED &&
            static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(packed_cell)) == RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
    }
    
    template<typename PCDT>
    static inline bool IsMode32TypedPublishedCell(packed64_t packed_cell) noexcept
    {
        if (!IsCellPublishedMode32Generic(packed_cell))
        {
            return false;
        }
        return PackedCell64_t::ExtractPCellDataTypeFromPacked(packed_cell)  == PackedCell64_t::PackedCellTypeBridge<PCDT>::DType;
    }

    static inline bool IsControlStopCell(packed64_t packed_cell) noexcept
    {
        if (!IsCellPublishedMode32Generic(packed_cell))
        {
            return false;
        }
        return PackedCell64_t::ExtractPCellDataTypeFromPacked(packed_cell) == PackedCellDataType::CharPCellDataType &&
            PackedCell64_t::ExtractRelMaskFromPacked(packed_cell) == REL_SELF &&
            PackedCell64_t::ExtractValue32(packed_cell) == STOP_CODE;
    }
        
}