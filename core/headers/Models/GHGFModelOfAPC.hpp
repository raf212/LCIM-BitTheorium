#pragma once 
#include "../NeuromorphicTimeSpace/VagueTemoraryPremativeFabric.hpp"

namespace BidirectionalInMemGraph
{

    class GHGFModelConstructor : private VagueTemoraryPremativeFabric
    {
    private:
        using GM = GHGFLayerModel;
        GM::GHGFCache Cache_{};

    public :

        bool InitializeGHGFFabric(
            uint32_t slot_count,
            const GHGFLayerModel::GHGFStorageProfile& profile
        ) noexcept;

        bool InitializeGHGFNode(
            AdaptivePackedCellContainer& apc,
            GM::GHGFNodeRole role,
            const GM::GHGFStorageProfile& profile
        ) noexcept;

        bool CreateNodeOfGHGF(AdaptivePackedCellContainer& desired_apc) noexcept
        {
            return HasDefaultRegionTable_ && CreateAPC(desired_apc, DefaultRegionTable_);
        }

        size_t GHGFSlotBegin_(uint32_t slot) const noexcept;
        bool IsLiveGHGFSlot_(uint32_t slot) noexcept;
        bool IsGHGFPlanCurrent_() noexcept;
        
        
            
    };
}