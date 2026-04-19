
#pragma once 
#include <array>
#include <utility>
#include "AdaptivePackedCellContainer/AdaptivePackedCellContainer.hpp"

namespace PredictedAdaptedEncoding
{

    class APCSegmentsCausalCordinator : public AdaptivePackedCellContainer
    {
    public:
        APCSegmentsCausalCordinator() noexcept = default;
        ~APCSegmentsCausalCordinator() = default;

        bool TryUpdateLastAcceptedClock16ForRegion(APCPagedNodeRelMaskClasses region_kind, clk16_t new_clock16) noexcept;
        bool TryUpdateLastEmittedClock16ForRegion(APCPagedNodeRelMaskClasses region_kind, clk16_t new_clock16) noexcept;
        bool IsCellCausallyAccepatable(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind, bool is_emitting = false) noexcept;
        uint32_t ComputePriorityCausalScore(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind, bool is_emitting = false) noexcept;
        
    };

    

}