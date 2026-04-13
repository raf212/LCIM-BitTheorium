
#pragma once 
#include "PackedCell.hpp"
#include "PackedCellBranchPlugin.hpp"

namespace PredictedAdaptedEncoding
{
    struct AcquirePairedPointerStruct
    {
        uint64_t AssembeledPtr = 0;
        size_t HeadIdx = SIZE_MAX;
        size_t TailIdx = SIZE_MAX;
        packed64_t HeadScreenshot = 0;
        packed64_t TailScreenshot = 0;
        RelOffsetMode32 Position = RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
        bool Ownership = false;
    };

    enum class PublishStatus : uint8_t
    {
        OK = 0,
        FULL = 1,
        INVALID = 2
    };

    struct PublishResult
    {
        PublishStatus ResultStatus{PublishStatus::INVALID};
        size_t Index{SIZE_MAX};
    };

    struct APCPagedNodeRegionBounds
    {
        size_t BeginIdx = SIZE_MAX;
        size_t EndIdx = SIZE_MAX;

        bool IsValidRegion() const noexcept
        {
            return BeginIdx != SIZE_MAX && EndIdx != SIZE_MAX && EndIdx > BeginIdx;
        }

        size_t GetRegionSpan() const noexcept
        {
            return (IsValidRegion() ? (EndIdx - BeginIdx) : 0);
        }
    };

    struct APCAndPagedNodeHelpers
    {
        static APCPagedNodeRelMaskClasses ExtractPagedRelMaskFromPacked (packed64_t packed_cell) noexcept
        {
            return static_cast<APCPagedNodeRelMaskClasses>(PackedCell64_t::ExtractRelMaskFromPacked(packed_cell));
        }

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
            return PackedCell64_t::ExtractPCellDataTypeFromPacked(packed_cell)  == PackedCellTypeBridge<PCDT>::DType;
        }

        static bool DoseCellBelongsToThisPagedRegion(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind) noexcept
        {
            return ExtractPagedRelMaskFromPacked(packed_cell) == region_kind;
        }

        static packed64_t SetRelMaskForPagedNode(packed64_t packed_cell, APCPagedNodeRelMaskClasses rel_mask) noexcept
        {
            return PackedCell64_t::SetRelMaskInPacked(packed_cell, static_cast<tag8_t>(rel_mask));
        }

    };
    


    
}
