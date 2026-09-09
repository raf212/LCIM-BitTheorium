#pragma once 
#include "../AdaptivePackedCellContainer.hpp"

namespace BidirectionalInMemGraph
{
    class GHGFNode : protected AdaptivePackedCellContainer
    {
        friend class GHGFModelConstructor;
    private:
        GHGFModelConstructor* GHGFFabric_{nullptr};
        
    };
    
}