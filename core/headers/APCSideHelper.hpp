#pragma once
#include "PackedCell.hpp"

namespace PredictedAdaptedEncoding
{
    struct APCSideHelper
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
        
        enum class APCPortKind : uint8_t
        {
            SELF_APC = 0,
            FEED_FORWARD_IN = 1,
            FEED_FORWARD_OUT = 2,
            FEED_BACKWARD_IN = 3,
            FEED_BACKWARD_OUT = 4,
            LATERAL_0 = 5,
            LATERAL_1 = 6
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
            return PackedCell64_t::ExtractPCellDataTypeFromPacked(packed_cell)  == PackedCellTypeBridge<PCDT>::DType;
        }

        static inline std::optional<NodeControlMask> ExtractNodeControl(packed64_t packed_cell) noexcept
        {
            if (!IsCellPublishedMode32Generic(packed_cell))
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

        
}