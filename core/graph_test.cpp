#include "PCSideHelper.hpp"
#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

using namespace PredictedAdaptedEncoding;

namespace 
{
    constexpr uint32_t VALUE_COUNT = 96u;
    constexpr float DEMO_MULT = 0.5f;
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
        if(IsControlStopCell(cell))
        {
            return true;
        }
        const uint16_t incoming = PackedCell64_t::ExtractClk16(cell);
        uint16_t last_seen = causal_state.LastAcceptedClk16.load(MoLoad_);
        if (incoming < last_seen)
        {
            return false;
        }
        while (last_seen < incoming && !causal_state.LastAcceptedClk16.compare_exchange_strong(last_seen, incoming, OnExchangeSuccess, OnExchangeFailure))
        {
            //loop
        }
        return true;
    }

    static AdaptivePackedCellContainer* ChooseOutputBranchA(
        AdaptivePackedCellContainer& root_a,
        uint32_t produced_idx,
        GraphStatus& graph_status
    )noexcept
    {
        auto maybe_fanout = root_a.GetAFanOut();

        if (!maybe_fanout)
        {
            return &root_a;
        }
        AdaptivePackedCellContainer::BinaryFanOutView fanout = *maybe_fanout;

        if (!fanout.LeftChildPtr && !fanout.RightCgildPtr)
        {
            return fanout.SelfPtr;
        }

        const size_t self_apc_occupancy = fanout.SelfPtr ? fanout.SelfPtr->OccupancyAddOrSubAndGetAfterChange() : SIZE_MAX;
        const size_t left_child_apc_occupancy = fanout.LeftChildPtr ? fanout.LeftChildPtr->OccupancyAddOrSubAndGetAfterChange() : SIZE_MAX;
        const size_t right_child_apc_occupancy = fanout.RightCgildPtr ? fanout.RightCgildPtr->OccupancyAddOrSubAndGetAfterChange() : SIZE_MAX;

        AdaptivePackedCellContainer* best_target_ptr = fanout.SelfPtr;
        size_t best_occupancy = self_apc_occupancy;
        if (fanout.LeftChildPtr && left_child_apc_occupancy < best_occupancy)
        {
            best_target_ptr = fanout.LeftChildPtr;
            best_occupancy = left_child_apc_occupancy;
        }

        if (fanout.RightCgildPtr && right_child_apc_occupancy < best_occupancy)
        {
            best_target_ptr = fanout.RightCgildPtr;
            best_occupancy = right_child_apc_occupancy;
        }
        if (best_target_ptr != fanout.SelfPtr)
        {
            graph_status.ForwardedToChild.fetch_add(1, std::memory_order_acq_rel);
        }
        return best_target_ptr ? best_target_ptr : fanout.SelfPtr;
    }

}