#pragma once
#include <functional>
#include "SchemaOrchestratorForRegion.hpp"

namespace BidirectionalInMemGraph
{
    class GHGFLayerModel
    {
    public:
        using SD = SchemaDefinition;
        static constexpr uint32_t DEFAULT_BATCH_CAPACITY = 32u;

        enum class GHGFNodeRole : uint8_t
        {
            OBSERVATION = 0,
            VALUE = 1,
            VOLATILE = 2
        };

        enum class GHGFStateRow : uint8_t
        {
            MEAN = 0,
            EXPECTED_MEAN = 1,
            PRECISION = 2,
            EXPECTED_PRECISION = 3,
            CONDITIONAL_EXPECTED_PRECISION = 4,
            OBSERVED = 5,
            CURRENT_VARIANCE = 6,
            EFFECTIVE_PRECISION = 7
        };
        static constexpr uint8_t STATE_ROW_COUNT_HEIGHT = static_cast<uint8_t>(GHGFStateRow::EFFECTIVE_PRECISION) + 1;

        enum class GHGFErrorRow : uint8_t
        {
            VALUE_PREDICTION_ERROR = 0,
            VOLATILE_PREDICTION_ERROR = 1
        };
        static constexpr uint8_t ERROR_ROW_COUNT_HEIGHT = static_cast<uint8_t>(GHGFErrorRow::VOLATILE_PREDICTION_ERROR) + 1;

        enum class GHGFErrorValueIndexing : uint8_t
        {
            TONIC_VOLATILE = 0,
            TONIC_DRIFT = 1,
            AUTO_CONNECTION = 2
        };
        static constexpr uint8_t FIRST_COUPLING_INDEX = static_cast<uint8_t>(GHGFErrorValueIndexing::AUTO_CONNECTION) + 1;

        static constexpr uint8_t WEIGHT_ROW_HEIGHT = 1u;

        struct GHGFStorageProfile final
        {
            uint32_t BatchCapacity = UNSIGNED_ZERO;
            uint32_t RequiredAPCCells = UNSIGNED_ZERO;
            uint32_t ParameterCount = UNSIGNED_ZERO;
            uint16_t ActiveRegionMask = UNSIGNED_ZERO;
            uint8_t MaxDirectParentPerAxis = APCDataStructure::DEFAULT_DIRECTED_PARENT_PER_AXIS;

            SD::FabricRegionConfig FabricConfig{};
            SD::RegionSchemaTable DefaultSchemaTable{};
            bool IsValid = false;
        };

        static constexpr uint32_t CouplingIndex(uint8_t relation_ordinal, uint8_t max_direct_parent_per_axis) noexcept
        {
            if (relation_ordinal >= max_direct_parent_per_axis)
            {
                return UNSIGNED_ZERO;
            }

            return FIRST_COUPLING_INDEX + static_cast<uint32_t>(max_direct_parent_per_axis) + relation_ordinal;
        }

        static constexpr bool MakeDefaultGHGFStorageProfile(
            GHGFStorageProfile& profile,
            uint32_t batch_capacity = DEFAULT_BATCH_CAPACITY,
            uint8_t max_direct_parent_per_axis = APCDataStructure::DEFAULT_DIRECTED_PARENT_PER_AXIS
        ) noexcept
        {
            profile = GHGFStorageProfile{};
            SD::MakeDisabledSchemaTable(profile.DefaultSchemaTable);

            if (
                batch_capacity == UNSIGNED_ZERO ||
                max_direct_parent_per_axis == UNSIGNED_ZERO
            )
            {
                return false;
            }

            const uint32_t parameter_count = FIRST_COUPLING_INDEX + (AXIS_COUNT * static_cast<uint32_t>(max_direct_parent_per_axis));

            if (!SD::AttachPrivateFloat32ToTable_(
                profile.DefaultSchemaTable,
                MacroColumnOfAPC::STATE_SLOT,
                STATE_ROW_COUNT_HEIGHT,
                batch_capacity,
                SD::SchemaFlags::BATCHED_LAST_DIM
            ))
            {
                return false;
            }
            
            if (!SD::AttachPrivateFloat32ToTable_(
                profile.DefaultSchemaTable,
                MacroColumnOfAPC::ERROR_SLOT,
                ERROR_ROW_COUNT_HEIGHT,
                batch_capacity,
                SD::SchemaFlags::BATCHED_LAST_DIM
            ))
            {
                return false;
            }

            if (!SD::AttachPrivateFloat32ToTable_(
                profile.DefaultSchemaTable,
                MacroColumnOfAPC::WEIGHT_SLOT,
                WEIGHT_ROW_HEIGHT,
                batch_capacity,
                SD::SchemaFlags::NONE
            ))
            {
                return false;
            }

            profile.BatchCapacity = batch_capacity;
            profile.MaxDirectParentPerAxis = max_direct_parent_per_axis;
            profile.ParameterCount = parameter_count;
            profile.ActiveRegionMask = SD::GetActiveMaskOfRegionTable_(profile.DefaultSchemaTable);
            profile.RequiredAPCCells = SD::RequiredCellsForSchemaTable_(profile.DefaultSchemaTable);

            if (
                profile.ActiveRegionMask == UNSIGNED_ZERO ||
                profile.RequiredAPCCells == UNSIGNED_ZERO
            )
            {
                return false;
            }
            
            profile.FabricConfig.ActiveRegionMask = profile.ActiveRegionMask;
            profile.FabricConfig.Reserved = UNSIGNED_ZERO;
            profile.FabricConfig.BatchCapacity = profile.BatchCapacity;

            profile.IsValid = true;

            return true;
        }

    };
    
}