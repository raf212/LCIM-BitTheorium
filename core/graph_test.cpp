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

    static AdaptivePackedCellContainer* ChooseAOutputBranchAndMarkProcessedInHeader(
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

    int main()
    {
        std::ios::sync_with_stdio(true);
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);

        PackedCellContainerManager& apc_manager = PackedCellContainerManager::Instance();
        apc_manager.StartAPCManager();

        ContainerConf container_cfg;
        container_cfg.InitialMode = PackedMode::MODE_VALUE32;
        container_cfg.ProducerBlockSize = 8;
        container_cfg.RegionSize = 16;
        container_cfg.EnableBranching = true;
        container_cfg.BranchSplitThresholdPercentage = 20;
        container_cfg.BranchMaxDepth = 3;
        container_cfg.BranchMinChildCapacity = 256;

        AdaptivePackedCellContainer unsigned_generation_container_A;
        AdaptivePackedCellContainer square_generation_container_B;
        AdaptivePackedCellContainer add_one_generation_container_C;
        AdaptivePackedCellContainer multiplication_genaration_container_D;
        AdaptivePackedCellContainer float_geather_container_E;

        unsigned_generation_container_A.SetManagerForGlobalAPC(&apc_manager);
        square_generation_container_B.SetManagerForGlobalAPC(&apc_manager);
        add_one_generation_container_C.SetManagerForGlobalAPC(&apc_manager);
        multiplication_genaration_container_D.SetManagerForGlobalAPC(&apc_manager);
        float_geather_container_E.SetManagerForGlobalAPC(&apc_manager);

        unsigned_generation_container_A.InitOwned(256, container_cfg);
        square_generation_container_B.InitOwned(256, container_cfg);
        add_one_generation_container_C.InitOwned(256, container_cfg);
        multiplication_genaration_container_D.InitOwned(256, container_cfg);
        float_geather_container_E.InitOwned(256, container_cfg);

        GraphStatus graph_status_main;
        // why no causal for first and last container??
        NodeCausalState causalB;
        NodeCausalState causalC;
        NodeCausalState causalD;
        NodeCausalState causalE;

        std::vector<float> collected_floar_vector;
        std::mutex collectecd_mutex;

        std::thread ProducerUnsignedA([&](){
            PackedCellContainerManager::ThreadHandlePCCM thread_handle = apc_manager.RegisterAPCThread();
            for (uint32_t i = 1; i <= VALUE_COUNT; i++)
            {
                packed64_t clock16_stamped_cell = apc_manager.GetMasterClockAdaptivePackedCellContainerManager().ComposeValue32WithCurrentThreadStamp16(
                    static_cast<val32_t>(i),
                    REL_NONE,
                    ZERO_PRIORITY,
                    PackedCellLocalityTypes::ST_PUBLISHED,
                    RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
                    PackedCellDataType::UnsignedPCellDataType
                );
                AdaptivePackedCellContainer* target_container = ChooseAOutputBranchAndMarkProcessedInHeader(unsigned_generation_container_A, i, graph_status_main);
                if (!unsigned_generation_container_A.WriteGenericValueCellWithCASClaimedManager(clock16_stamped_cell))
                {
                    std::cerr << "Unsigned generation container failed to publish:: A" << i << "\n";
                }
                else
                {
                    graph_status_main.ProducedUnsigned.fetch_add(1, std::memory_order_relaxed);
                }

                if (unsigned_generation_container_A.GetBranchPlugin()->ShouldSplitNow())
                {
                    graph_status_main.SplitRequests.fetch_add(1, std::memory_order_relaxed);
                    apc_manager.RequestBranchCreationForTheAdaptivePackedCellContainer(&unsigned_generation_container_A);
                    apc_manager.GetCellsAdaptiveBackoffFromManager(clock16_stamped_cell);
                }
            }
            unsigned_generation_container_A.WriteGenericValueCellWithCASClaimedManager(MakeStopControllCell(apc_manager));
            apc_manager.UnRegisterAPCThread(thread_handle);
        });

        std::thread WorkerSquareB([&]{
            PackedCellContainerManager::ThreadHandlePCCM thread_handle = apc_manager.RegisterAPCThread();
            size_t scan_root = unsigned_generation_container_A.PayloadBegin();
            bool stop = false;
            while (!stop)
            {
                bool did_work = false;
                packed64_t easy_return_for_processing{0};
                auto maybe_fanout = square_generation_container_B.GetAFanOut();
                if (!maybe_fanout)
                {
                    break;
                }
                
                AdaptivePackedCellContainer::BinaryFanOutView fanout = *maybe_fanout;
                if (fanout.SelfPtr->ConsumeAndIdleGenericValueCell(scan_root, easy_return_for_processing) || 
                (fanout.LeftChildPtr ? fanout.LeftChildPtr->ConsumeAndIdleGenericValueCell(scan_root, easy_return_for_processing) : false) ||
                (fanout.RightCgildPtr ? fanout.RightCgildPtr->ConsumeAndIdleGenericValueCell(scan_root, easy_return_for_processing) : false)
                )
                {
                    did_work = true;
                    if (!AcceptCausalClockDemo(causalB, easy_return_for_processing))
                    {
                        graph_status_main.DroppedAsStale.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    if (IsControlStopCell(easy_return_for_processing))
                    {
                        ++graph_status_main.ControlStopsSeen;
                        square_generation_container_B.WriteGenericValueCellWithCASClaimedManager(easy_return_for_processing);
                        stop = true;
                        continue;
                    }

                    if (!IsMode32TypedPublishedCell<uint32_t>(easy_return_for_processing))
                    {
                        std::cerr << "Cell data type is not Unsigned\n";
                    }

                    const uint32_t unsigned_value = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(easy_return_for_processing);
                    const uint32_t square_of_unsigned = unsigned_value * unsigned_value;

                    packed64_t out_cell = apc_manager.GetMasterClockAdaptivePackedCellContainerManager().ComposeValue32WithCurrentThreadStamp16(
                        square_of_unsigned,
                        REL_NONE
                    );
                    if (!square_generation_container_B.WriteGenericValueCellWithCASClaimedManager(easy_return_for_processing))
                    {
                        std::cerr << "Container B  failed for " << square_of_unsigned << "\n";
                    }
                    else
                    {
                        graph_status_main.consumedSquare.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (!did_work)
                {
                    apc_manager.GetCellsAdaptiveBackoffFromManager(easy_return_for_processing);
                }
            }
            apc_manager.UnRegisterAPCThread(thread_handle);
        });


    }

}