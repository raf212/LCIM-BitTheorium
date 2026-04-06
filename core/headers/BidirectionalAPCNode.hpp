#pragma once
#include "AdaptivePackedCellContainer.hpp"
#include "APCSideHelper.hpp"

namespace PredictedAdaptedEncoding
{
    class BidirectionalAPCNode
    {
    private:
        AdaptivePackedCellContainer* APCPtr_;
    public:
        BidirectionalAPCNode(AdaptivePackedCellContainer* adaptive_packed_cell_container = nullptr) noexcept :
            APCPtr_(adaptive_packed_cell_container)
        {}

        uint32_t ReadComputeKind() const noexcept;
        bool SetAUXUint32(uint32_t value) noexcept;
        uint32_t ReadAUXUint32() const noexcept;
        bool BindFeedForwardTo(BidirectionalAPCNode& next_apc_node) noexcept;
        bool BindFeedBackwardTo(BidirectionalAPCNode& previous_apc_node) noexcept;
    };
    
    
}