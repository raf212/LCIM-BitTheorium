
#pragma once 
#include <array>
#include <utility>
#include "PackedCell.hpp"
#include "SegmentIODefinition.hpp"

namespace PredictedAdaptedEncoding
{

    class CausalSegmentController : public SegmentIODefinition
    {
    public:
        CausalSegmentController() noexcept = default;
        ~CausalSegmentController() = default;

        bool TryUpdateLastAcceptedClock16ForRegion(APCPagedNodeRelMaskClasses region_kind, clk16_t new_clock16) noexcept;
        bool TryUpdateLastEmittedClock16ForRegion(APCPagedNodeRelMaskClasses region_kind, clk16_t new_clock16) noexcept;
        bool IsCellCausallyAccepatable(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind, bool is_emitting = false) noexcept;
        uint32_t ComputePriorityCausalScore(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind, bool is_emitting = false) noexcept;
        
    };

    

}