#pragma once 
#include "../VagueTemoraryPremativeFabric.hpp"

namespace BidirectionalInMemGraph
{
    class GHGFModelConstructor : private VagueTemoraryPremativeFabric
    {
    public :
    using GM = GHGFLayerModel;

        bool InitializeGHGFFabric(
            uint32_t slot_count,
            const GHGFLayerModel::GHGFStorageProfile& profile
        ) noexcept;

        bool InitializeAGHGFGNode(
            AdaptivePackedCellContainer& apc,
            GM::GHGFNodeRole role,
            const GM::GHGFStorageProfile& profile
        ) noexcept;
            
    };
}