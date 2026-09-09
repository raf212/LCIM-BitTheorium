#pragma once 
#include "../AdaptivePackedCellContainer.hpp"

namespace BidirectionalInMemGraph
{
    class GHGFNode : protected AdaptivePackedCellContainer
    {
        friend class GHGFModelConstructor;
    private:
        using GM = GHGFLayerModel;

        GHGFModelConstructor* GHGFFabric_{nullptr};

        bool InitializeGHGFNode(
            GM::GHGFNodeRole role
        ) noexcept;
    };
    
}