
#pragma once 
#include <array>
#include <utility>
#include "PackedCell.hpp"
#include "SegmentIODefinition.hpp"

namespace PredictedAdaptedEncoding
{

    class CausalSegmentController
    {
    private:
        SegmentIODefinition* SegmentIOPtr_{nullptr};
    public:
        CausalSegmentController(SegmentIODefinition* apc_segment_io_ptr) noexcept :
            SegmentIOPtr_(apc_segment_io_ptr)
        {}
        
    };

    

}