#pragma once 
#include "../VagueTemoraryPremativeFabric.hpp"

namespace BidirectionalInMemGraph
{
    class GHGFModelConstructor : private VagueTemoraryPremativeFabric
    {
    private:
        using GM = GHGFLayerModel;
        GM::GHGFStorageProfile Profile_{};


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
            
    };
}