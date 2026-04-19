
#pragma once 
#include <array>
#include <utility>
#include "AdaptivePackedCellContainer/APCHElpers.hpp"
#include "AdaptivePackedCellContainer/AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

namespace PredictedAdaptedEncoding
{

    class APCSegmentsCausalCordinator : public AdaptivePackedCellContainer
    {
    private:
        bool RegionWakeUrgency_(APCPagedNodeRelMaskClasses region_kind, packed64_t packed_cell, tag8_t min_wake_threshold = DEFAULT_INTERNAL_PRIORITY) noexcept;


    public:
        APCSegmentsCausalCordinator() noexcept = default;
        ~APCSegmentsCausalCordinator() = default;

        
    };

    

}