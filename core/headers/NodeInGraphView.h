#pragma once
#include "APCSegmentsCausalCordinator.hpp"

namespace PredictedAdaptedEncoding
{
struct PageNodeHelper
{
    enum class APCPortControl : uint8_t
    {
        NULL_SEQUENSE = 0,
        START_SEQUENSE = 1,
        STOP_SEQUENSE = 2
    };

    enum class NodeControlMask : uint8_t
    {
        UNDEFINED_ORDER = 0,
        SELF_ORDER = 1,
        FEEDFORWARD_EVIDANCE = 2,
        FEEDBACKWARD_EVIDANCE = 3,
        RESIDUAL_ERROR = 4,
        LATENT_STATE = 5,
        LATERAL_SIGNAL = 6
    };
    

    static inline std::optional<NodeControlMask> ExtractNodeControl(packed64_t packed_cell) noexcept
    {
        if (!PredictedAdaptedEncoding::APCAndPagedNodeHelpers::IsCellPublishedMode32Generic(packed_cell))
        {
            return std::nullopt;
        }

        if (PackedCell64_t::ExtractPCellDataTypeFromPacked(packed_cell) != PackedCellDataType::CharPCellDataType)
        {
            return std::nullopt;
        }

        return static_cast<NodeControlMask>(PackedCell64_t::ExtractRelMaskFromPacked(packed_cell));
        
    }

    static inline std::optional<APCPortControl> ExtractControlSequense(packed64_t packed_cell) noexcept
    {
        if (ExtractNodeControl(packed_cell) != NodeControlMask::SELF_ORDER)
        {
            return std::nullopt;
        }
        return static_cast<APCPortControl>(PackedCell64_t::ExtractValue32(packed_cell));
    }

    static inline bool IsControlStopCell(packed64_t packed_cell) noexcept
    {
        if (ExtractControlSequense(packed_cell) == APCPortControl::STOP_SEQUENSE)
        {
            return true;
        }
        return false;
    }
};

        

struct GraphPortView
{
    AdaptivePackedCellContainer* SelfPtr = nullptr;
    AdaptivePackedCellContainer* FeedForwardInPtr = nullptr;
    AdaptivePackedCellContainer* FeedBackwardIntPtr = nullptr;
    AdaptivePackedCellContainer* FeedForwardOutPtr = nullptr;
    AdaptivePackedCellContainer* FeedBackwardOutPtr = nullptr;
};

    
}

