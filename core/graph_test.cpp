#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

using namespace PredictedAdaptedEncoding;

namespace 
{
    constexpr uint32_t VALUE_COUNT = 96u;
    constexpr float DEMO_MULT = 0.5f;
    constexpr unsigned STOP_CODE = 1u;

    struct GraphStatus
    {
// A generates unsigned cells
// B squares them
// C adds 1
// D multiplies by float
// E collects floats
        std::atomic<uint64_t> ProducedUnsigned{0};
        std::atomic<uint64_t> ConsumedUnsigned{0};
        std::atomic<uint64_t> consumedSquare{0};
        std::atomic<uint64_t> ConsumedAddition{0};
        std::atomic<uint64_t> ConsumedMultiplication{0};
        std::atomic<uint64_t> DroppedAsStale{0};
        std::atomic<uint64_t> ForwardedToChild{0};
        std::atomic<uint64_t> SplitRequests{0};
        std::atomic<uint64_t> ControlStopsSeen{0};
    };

    struct NodeCausalState
    {
        std::atomic<uint16_t> LastAcceptedClk16{0};
    };

    static inline packed64_t MakeStopControllCell( PackedCellContainerManager &manager)
    {
        return manager.GetMasterClockAdaptivePackedCellContainerManager().ComposeValue32WithCurrentThreadStamp16(
            STOP_CODE,
            REL_SELF,
            MAX_PRIORITY,
            PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType::CharPCellDataType
        );
    }

    static bool AcceptCausalClockDemo (NodeCausalState& causal_state, packed64_t cell) noexcept
    {
        if(PackedCell64_t)
    }

    
}