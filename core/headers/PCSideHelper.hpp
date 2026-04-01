#include "PackedCell.hpp"

namespace PredictedAdaptedEncoding
{
    static inline bool IsCellPublishedMode32Generic (packed64_t packed_cell) noexcept
    {
        return PackedCell64_t::ExtractModeOfPackedCellFromPacked(packed_cell) == PackedMode::MODE_VALUE32 && 
            PackedCell64_t::ExtractLocalityFromPacked(packed_cell) == PackedCellLocalityTypes::ST_PUBLISHED &&
            static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(packed_cell)) == RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
    }
    
}