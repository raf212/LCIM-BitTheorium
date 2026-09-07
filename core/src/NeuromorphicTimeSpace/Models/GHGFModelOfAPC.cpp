#pragma once
#include "NeuromorphicTimeSpace/Models/GHGFModelOfAPC.hpp"
#include <span>

namespace BidirectionalInMemGraph
{
    bool GHGFModelConstructor::InitializeGHGFFabric(
        uint32_t slot_count,
        const GHGFLayerModel::GHGFStorageProfile& profile
    ) noexcept
    {
        if (!GHGFLayerModel::IsValidStoregeProfile(profile))
        {
            return false;
        }

        DefaultRegionTable_ = profile.DefaultSchemaTable;
        HasDefaultRegionTable_ = true;

        return InitializeFabricWithPtrTable(
            slot_count,
            profile.RequiredAPCCells,
            profile.FabricConfig,
            profile.MaxDirectParentPerAxis
        );
        
    }

    bool GHGFModelConstructor::InitializeAGHGFGNode(
        AdaptivePackedCellContainer& apc,
        GHGFLayerModel::GHGFNodeRole role,
        const GHGFLayerModel::GHGFStorageProfile& profile
    ) noexcept
    {
        using GMC = GM::StorageConst;

        if (!GM::IsValidStoregeProfile(profile) || !apc.IsActiveAPC())
        {
            return false;
        }

        std::optional<RegionView<float>> state_view = apc.BuildAViewOverRegion<float>(MacroColumnOfAPC::STATE_SLOT);
        std::optional<RegionView<float>> error_view = apc.BuildAViewOverRegion<float>(MacroColumnOfAPC::ERROR_SLOT);
        std::optional<RegionView<float>> weight_view = apc.BuildAViewOverRegion<float>(MacroColumnOfAPC::WEIGHT_SLOT);
        
        if (
            !state_view.has_value() ||
            !error_view.has_value() ||
            !weight_view.has_value()
        )
        {
            return false;
        }
        
        std::optional<std::span<float>> state = state_view.value().RawMutableSpan();
        std::optional<std::span<float>> error = state_view.value().RawMutableSpan();
        std::optional<std::span<float>> weight = state_view.value().RawMutableSpan();
        

        if (
            !state.has_value() ||
            !error.has_value() ||
            !weight.has_value() ||
            state.value().size() != static_cast<size_t>(GM::STATE_ROW_COUNT_HEIGHT) * profile.BatchCapacity ||
            error.value().size() != static_cast<size_t>(GM::ERROR_ROW_COUNT_HEIGHT) * profile.BatchCapacity ||
            weight.value().size() != profile.ParameterCount
        )
        {
            return false;
        }
        
        std::fill(state.value().begin(), state.value().end(), GMC::INITIAL_STORAGE_VALUE);
        std::fill(error.value().begin(), error.value().end(), GMC::INITIAL_STORAGE_VALUE);
        std::fill(weight.value().begin(), weight.value().end(), GMC::INITIAL_STORAGE_VALUE);

        const auto StateIndex___ = [&](GM::GHGFStateRow row, uint32_t batch) noexcept -> size_t
        {
            return (static_cast<size_t>(row) * profile.BatchCapacity) + batch;
        };
        
        for (uint32_t batch = 0; batch < profile.BatchCapacity; batch++)
        {
            (state.value())[StateIndex___(GM::GHGFStateRow::MEAN, batch)] = GMC::INITIAL_STORAGE_VALUE;
            if (role == GM::GHGFNodeRole::OBSERVATION)
            {
                (state.value())[StateIndex___(GM::GHGFStateRow::EXPECTED_MEAN, batch)] = GMC::INITIAL_BINARY_PROBABILITY;
            }
            else
            {
                (state.value())[StateIndex___(GM::GHGFStateRow::EXPECTED_MEAN, batch)] = GMC::INITIAL_STORAGE_VALUE;
            }

            (state.value())[StateIndex___(GM::GHGFStateRow::PRECISION, batch)] = GMC::INITIAL_PRECISION;
            (state.value())[StateIndex___(GM::GHGFStateRow::EFFECTIVE_PRECISION, batch)] = GMC::INITIAL_PRECISION;
            (state.value())[StateIndex___(GM::GHGFStateRow::CONDITIONAL_EXPECTED_PRECISION, batch)] = GMC::INITIAL_PRECISION;
            (state.value())[StateIndex___(GM::GHGFStateRow::OBSERVED, batch)] = GMC::INITIAL_PRECISION;
            (state.value())[StateIndex___(GM::GHGFStateRow::CURRENT_VARIANCE, batch)] = GMC::INITIAL_PRECISION;
            (state.value())[StateIndex___(GM::GHGFStateRow::EFFECTIVE_PRECISION, batch)] = GMC::INITIAL_STORAGE_VALUE;
        }

        (weight.value())[static_cast<size_t>(GM::GHGFErrorValueIndexing::TONIC_VOLATILE)] = role == GM::GHGFNodeRole::OBSERVATION?
            GMC::OBSERVATION_TONIC_LOG_VOLATILITY : GMC::INITIAL_TONIC_LOG_VOLATILITY;
        (weight.value())[static_cast<size_t>(GM::GHGFErrorValueIndexing::TONIC_DRIFT)] = GMC::INITIAL_STORAGE_VALUE;
        (weight.value())[static_cast<size_t>(GM::GHGFErrorValueIndexing::AUTO_CONNECTION)] = role == GM::GHGFNodeRole::OBSERVATION?
            GMC::INITIAL_STORAGE_VALUE : GMC::INITIAL_AUTO_CONNECTION;

        for (uint8_t i = 0; i < profile.MaxDirectParentPerAxis; i++)
        {
            const uint32_t value_index = GM::CouplingIndex(
                FabricSegments::VALUE_PARENT_EDGE_TABLE_H,
                i,
                profile.MaxDirectParentPerAxis
            );

            const uint32_t volatile_index = GM::CouplingIndex(
                FabricSegments::VOLATILE_PARENT_EDGE_TABLE_V,
                i,
                profile.MaxDirectParentPerAxis
            );

            if (
                value_index > weight.value().size() ||
                volatile_index >= weight.value().size()
            )
            {
                return false;
            }

            (weight.value())[value_index] = GMC::INITIAL_VALUE_COUPLING;
            (weight.value())[volatile_index] = GMC::INITIAL_VOLATILITY_COUPLING;
        }
        
        return true;
    }
            
}