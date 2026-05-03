#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <array>
#include <optional>

#include "APCSegmentsCausalCordinator.hpp"
#include "PackedCellContainerManager.hpp"

using namespace PredictedAdaptedEncoding;

namespace
{
    constexpr uint32_t VALUE_COUNT = 2560u;
    constexpr uint32_t PRODUCER_COUNT = 2u;
    constexpr uint32_t FF_WORKER_COUNT = 3u;
    constexpr uint32_t FB_WORKER_COUNT = 2u;
    constexpr uint32_t FINAL_WORKER_COUNT = 1u;

    struct GraphStats
    {
        std::atomic<uint64_t> SensorFFProduced{0};
        std::atomic<uint64_t> PredictorFBProduced{0};

        std::atomic<uint64_t> StateIntegrated{0};
        std::atomic<uint64_t> ErrorComputed{0};

        std::atomic<uint64_t> ForwardEmitted{0};
        std::atomic<uint64_t> FeedbackEmitted{0};

        std::atomic<uint64_t> FinalCollected{0};

        std::atomic<uint64_t> GrowFF{0};
        std::atomic<uint64_t> GrowFB{0};
        std::atomic<uint64_t> GrowSTATE{0};
        std::atomic<uint64_t> GrowERROR{0};

        std::atomic<uint64_t> Retry{0};
        std::atomic<uint64_t> TerminalFail{0};

        std::atomic<uint64_t> OlderFFObserved{0};
        std::atomic<uint64_t> OlderFBObserved{0};
    };

    struct ExactLocalityCount
    {
        uint32_t Idle{0};
        uint32_t Published{0};
        uint32_t Claimed{0};
        uint32_t Faulty{0};

        uint32_t Sum() const noexcept
        {
            return Idle + Published + Claimed + Faulty;
        }
    };

    static packed64_t PackU32(
        MasterClockConf& clock,
        uint32_t value,
        APCPagedNodeRelMaskClasses region,
        PriorityPhysics priority = PriorityPhysics::DEFAULT_PRIORITY
    )
    {
        return clock.ComposeValue32WithCurrentThreadStamp16(
            value,
            region,
            priority,
            PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType::UnsignedPCellDataType,
            PackedCellNodeAuthority::BIDIRECTIONAL_NEUROMORPHIC_SYSTEM
        );
    }

    static packed64_t PackFloat32(
        MasterClockConf& clock,
        float value,
        APCPagedNodeRelMaskClasses region,
        PriorityPhysics priority = PriorityPhysics::DEFAULT_PRIORITY
    )
    {
        const uint32_t bits = BitCastMaybe<uint32_t>(value);

        return clock.ComposeValue32WithCurrentThreadStamp16(
            bits,
            region,
            priority,
            PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType::FloatPCellDataType,
            PackedCellNodeAuthority::BIDIRECTIONAL_NEUROMORPHIC_SYSTEM
        );
    }

    static bool PublishBudgeted(
        APCSegmentsCausalCordinator& apc,
        APCPagedNodeRelMaskClasses region,
        packed64_t cell,
        PackedCellContainerManager& manager,
        std::atomic<uint64_t>* grow_counter,
        GraphStats& stats,
        uint32_t budget = 4096
    )
    {
        for (uint32_t attempt = 0; attempt < budget; ++attempt)
        {
            if (apc.PublishCausal(region, cell, grow_counter))
            {
                return true;
            }

            stats.Retry.fetch_add(1, std::memory_order_relaxed);
            manager.GetCellsAdaptiveBackoffFromManager(cell);
        }

        stats.TerminalFail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    static ExactLocalityCount CountExactLocality(APCSegmentsCausalCordinator& apc)
    {
        ExactLocalityCount out{};

        if (!apc.IfAPCBranchValid())
        {
            return out;
        }

        for (size_t i = apc.PayloadBegin(); i < apc.GetTotalCapacityForThisAPC(); ++i)
        {
            const packed64_t cell = apc.BackingPtr[i].load(MoLoad_);
            const auto view = PackedCell64_t::InspectPackedCell(cell);

            if (!view.IsCellValid ||
                view.LocalityOfCell == PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY)
            {
                ++out.Faulty;
                continue;
            }

            switch (view.LocalityOfCell)
            {
                case PackedCellLocalityTypes::ST_IDLE:
                    ++out.Idle;
                    break;

                case PackedCellLocalityTypes::ST_PUBLISHED:
                    ++out.Published;
                    break;

                case PackedCellLocalityTypes::ST_CLAIMED:
                    ++out.Claimed;
                    break;

                case PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY:
                default:
                    ++out.Faulty;
                    break;
            }
        }

        return out;
    }

    static uint32_t HeaderLocalitySum(APCSegmentsCausalCordinator& apc)
    {
        return
            apc.AllIdleCellsOccupancySnapshotAddOrSubAndGetAfterChange(0) +
            apc.AllPublishedCellsOccupancySnapshotAddOrSubAndGetAfterChange(0) +
            apc.AllClaimedCellsOccupancySnapshotAddOrSubAndGetAfterChange(0) +
            apc.AllFaultyCellsOccupancySnapshotAddOrSubAndGetAfterChange(0);
    }

    static uint32_t RegionMeta(APCSegmentsCausalCordinator& apc, APCPagedNodeRelMaskClasses region)
    {
        return apc.RegionOccupancyAddOrSubAndGet(region, 0);
    }

    static void PrintRegion(
        const char* label,
        APCSegmentsCausalCordinator& apc,
        APCPagedNodeRelMaskClasses region
    )
    {
        const auto maybe = apc.ReadLayoutBounds(region);

        std::cout << "    " << label
                  << " meta_pub=" << RegionMeta(apc, region);

        if (maybe)
        {
            std::cout << " span=[" << maybe->BeginIndex
                      << "," << maybe->EndIndex
                      << ") size=" << maybe->GetPayloadSpan();
        }

        std::cout << "\n";
    }

    static void PrintNode(
        const char* name,
        APCSegmentsCausalCordinator& apc
    )
    {
        const ExactLocalityCount exact = CountExactLocality(apc);

        const uint32_t header_idle =
            apc.AllIdleCellsOccupancySnapshotAddOrSubAndGetAfterChange(0);

        const uint32_t header_pub =
            apc.AllPublishedCellsOccupancySnapshotAddOrSubAndGetAfterChange(0);

        const uint32_t header_claim =
            apc.AllClaimedCellsOccupancySnapshotAddOrSubAndGetAfterChange(0);

        const uint32_t header_fault =
            apc.AllFaultyCellsOccupancySnapshotAddOrSubAndGetAfterChange(0);

        const uint32_t header_sum = HeaderLocalitySum(apc);
        const uint32_t payload = static_cast<uint32_t>(apc.PayloadCapacityFromHeader());

        std::cout << "\n[" << name << "]\n";
        std::cout << "  branch=" << apc.GetBranchId()
                  << " logical=" << apc.GetLogicalId()
                  << " shared=" << apc.GetSharedId()
                  << " group=" << apc.ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_GROUP_SIZE)
                  << "\n";

        std::cout << "  payload_capacity=" << payload
                  << " total_capacity=" << apc.GetTotalCapacityForThisAPC()
                  << " ready_bit=0x" << std::hex
                  << apc.ReadMetaCellValue32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT)
                  << std::dec
                  << "\n";

        std::cout << "  header locality: "
                  << "idle=" << header_idle
                  << " pub=" << header_pub
                  << " claim=" << header_claim
                  << " faulty=" << header_fault
                  << " sum=" << header_sum
                  << " invariant=" << (header_sum == payload ? "OK" : "BAD")
                  << "\n";

        std::cout << "  exact  locality: "
                  << "idle=" << exact.Idle
                  << " pub=" << exact.Published
                  << " claim=" << exact.Claimed
                  << " faulty=" << exact.Faulty
                  << " sum=" << exact.Sum()
                  << " invariant=" << (exact.Sum() == payload ? "OK" : "BAD")
                  << "\n";

        std::cout << "  region published-data pressure:\n";
        PrintRegion("FF   ", apc, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE);
        PrintRegion("FB   ", apc, APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE);
        PrintRegion("STATE", apc, APCPagedNodeRelMaskClasses::STATE_SLOT);
        PrintRegion("ERROR", apc, APCPagedNodeRelMaskClasses::ERROR_SLOT);
        PrintRegion("AUX  ", apc, APCPagedNodeRelMaskClasses::AUX_SLOT);

        std::cout << "  clocks: "
                  << "accFF=" << apc.ReadLastAcceptedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE)
                  << " emitFF=" << apc.ReadLastEmittedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE)
                  << " accFB=" << apc.ReadLastAcceptedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE)
                  << " emitFB=" << apc.ReadLastEmittedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE)
                  << "\n";
    }
}

int main()
{
    std::ios::sync_with_stdio(true);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    auto& manager = PackedCellContainerManager::Instance();
    manager.StartAPCManager();

    Timer48 timer;
    MasterClockConf clock(nullptr, timer);

    ContainerConf cfg;
    cfg.InitialMode = PackedMode::MODE_VALUE32;
    cfg.ProducerBlockSize = 8;
    cfg.RegionSize = 16;
    cfg.EnableBranching = true;
    cfg.BranchSplitThresholdPercentage = 70;
    cfg.BranchMaxDepth = 6;
    cfg.BranchMinChildCapacity = 256;
    cfg.NodeGroupSize = 1u;

    APCSegmentsCausalCordinator Sensor;
    APCSegmentsCausalCordinator Predictor;
    APCSegmentsCausalCordinator Comparator;
    APCSegmentsCausalCordinator Integrator;
    APCSegmentsCausalCordinator Motor;

    Sensor.SetManagerForGlobalAPC(&manager);
    Predictor.SetManagerForGlobalAPC(&manager);
    Comparator.SetManagerForGlobalAPC(&manager);
    Integrator.SetManagerForGlobalAPC(&manager);
    Motor.SetManagerForGlobalAPC(&manager);

    Sensor.InitAPCAsNode(
        256,
        cfg,
        SegmentIODefinition::APCNodeComputeKind::GENERATOR_UINT32,
        0
    );

    Predictor.InitAPCAsNode(
        256,
        cfg,
        SegmentIODefinition::APCNodeComputeKind::BIDIRECTIONAL_PREDECTIVE,
        0
    );

    Comparator.InitAPCAsNode(
        256,
        cfg,
        SegmentIODefinition::APCNodeComputeKind::ADD_UINT32,
        0
    );

    Integrator.InitAPCAsNode(
        256,
        cfg,
        SegmentIODefinition::APCNodeComputeKind::GENERIC_VECTOR,
        0
    );

    Motor.InitAPCAsNode(
        256,
        cfg,
        SegmentIODefinition::APCNodeComputeKind::GENERIC_VECTOR,
        0
    );

    GraphStats stats;

    std::atomic<bool> producers_done{false};
    std::atomic<uint64_t> ff_consumed{0};
    std::atomic<uint64_t> fb_consumed{0};
    std::atomic<uint64_t> state_consumed{0};
    std::atomic<uint64_t> error_consumed{0};
    std::atomic<uint64_t> final_done{0};

    std::vector<float> collected;
    std::mutex collected_mutex;

    const auto start_time = std::chrono::steady_clock::now();

    std::vector<std::thread> producers;
    std::vector<std::thread> workers;

    for (uint32_t p = 0; p < PRODUCER_COUNT; ++p)
    {
        producers.emplace_back([&, p]()
        {
            auto th = manager.RegisterAPCThread();

            for (uint32_t i = p + 1; i <= VALUE_COUNT; i += PRODUCER_COUNT)
            {
                const packed64_t ff =
                    PackU32(clock, i, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, PriorityPhysics::IMPORTANT);

                const packed64_t fb =
                    PackU32(clock, i + 1u, APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE, PriorityPhysics::TIME_DEPENDENCY);

                if (PublishBudgeted(
                        Sensor,
                        APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE,
                        ff,
                        manager,
                        &stats.GrowFF,
                        stats))
                {
                    stats.SensorFFProduced.fetch_add(1);
                    stats.ForwardEmitted.fetch_add(1);
                }

                if (PublishBudgeted(
                        Predictor,
                        APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE,
                        fb,
                        manager,
                        &stats.GrowFB,
                        stats))
                {
                    stats.PredictorFBProduced.fetch_add(1);
                    stats.FeedbackEmitted.fetch_add(1);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (uint32_t w = 0; w < FF_WORKER_COUNT; ++w)
    {
        workers.emplace_back([&, w]()
        {
            auto th = manager.RegisterAPCThread();
            size_t cursor = Sensor.PayloadBegin();

            while (ff_consumed.load(std::memory_order_acquire) < VALUE_COUNT)
            {
                auto maybe =
                    Sensor.ConsumeCausal(
                        APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE,
                        cursor,
                        &stats.OlderFFObserved
                    );

                if (!maybe)
                {
                    manager.GetManagersAdaptiveBackoff().AutoBackoff();
                    continue;
                }

                const auto maybe_x =
                    PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe);

                if (!maybe_x)
                {
                    continue;
                }

                const uint32_t state_value = maybe_x.value() + 1u;

                const packed64_t state_cell =
                    PackU32(
                        clock,
                        state_value,
                        APCPagedNodeRelMaskClasses::STATE_SLOT,
                        PriorityPhysics::STRUCTURAL_DEPENDENCY
                    );

                if (PublishBudgeted(
                        Integrator,
                        APCPagedNodeRelMaskClasses::STATE_SLOT,
                        state_cell,
                        manager,
                        &stats.GrowSTATE,
                        stats))
                {
                    stats.StateIntegrated.fetch_add(1);
                    ff_consumed.fetch_add(1);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (uint32_t w = 0; w < FB_WORKER_COUNT; ++w)
    {
        workers.emplace_back([&, w]()
        {
            auto th = manager.RegisterAPCThread();
            size_t cursor = Predictor.PayloadBegin();

            while (fb_consumed.load(std::memory_order_acquire) < VALUE_COUNT)
            {
                auto maybe =
                    Predictor.ConsumeCausal(
                        APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE,
                        cursor,
                        &stats.OlderFBObserved
                    );

                if (!maybe)
                {
                    manager.GetManagersAdaptiveBackoff().AutoBackoff();
                    continue;
                }

                const auto maybe_x =
                    PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe);

                if (!maybe_x)
                {
                    continue;
                }

                const uint32_t error_value = 1u;

                const packed64_t error_cell =
                    PackU32(
                        clock,
                        error_value,
                        APCPagedNodeRelMaskClasses::ERROR_SLOT,
                        PriorityPhysics::ERROR_DEPENDENCY
                    );

                if (PublishBudgeted(
                        Comparator,
                        APCPagedNodeRelMaskClasses::ERROR_SLOT,
                        error_cell,
                        manager,
                        &stats.GrowERROR,
                        stats))
                {
                    stats.ErrorComputed.fetch_add(1);
                    fb_consumed.fetch_add(1);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (uint32_t w = 0; w < FINAL_WORKER_COUNT; ++w)
    {
        workers.emplace_back([&, w]()
        {
            auto th = manager.RegisterAPCThread();

            size_t state_cursor = Integrator.PayloadBegin();
            size_t error_cursor = Comparator.PayloadBegin();

            while (final_done.load(std::memory_order_acquire) < VALUE_COUNT)
            {
                auto maybe_state =
                    Integrator.ConsumeCausal(
                        APCPagedNodeRelMaskClasses::STATE_SLOT,
                        state_cursor,
                        nullptr
                    );

                if (!maybe_state)
                {
                    manager.GetManagersAdaptiveBackoff().AutoBackoff();
                    continue;
                }

                const auto maybe_state_u32 =
                    PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe_state);

                if (!maybe_state_u32)
                {
                    continue;
                }

                uint32_t error_adjust = 0u;

                auto maybe_error =
                    Comparator.ConsumeCausal(
                        APCPagedNodeRelMaskClasses::ERROR_SLOT,
                        error_cursor,
                        nullptr
                    );

                if (maybe_error)
                {
                    const auto maybe_error_u32 =
                        PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe_error);

                    if (maybe_error_u32)
                    {
                        error_adjust = maybe_error_u32.value();
                        error_consumed.fetch_add(1);
                    }
                }

                const float motor_value =
                    static_cast<float>(maybe_state_u32.value()) + 0.5f * static_cast<float>(error_adjust);

                const packed64_t motor_cell =
                    PackFloat32(
                        clock,
                        motor_value,
                        APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE,
                        PriorityPhysics::HANDLE_NOW
                    );

                if (PublishBudgeted(
                        Motor,
                        APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE,
                        motor_cell,
                        manager,
                        nullptr,
                        stats))
                {
                    state_consumed.fetch_add(1);
                }

                size_t motor_cursor = Motor.PayloadBegin();

                auto final_cell =
                    Motor.ConsumeCausal(
                        APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE,
                        motor_cursor,
                        nullptr
                    );

                if (final_cell)
                {
                    const auto maybe_float =
                        PackedCell64_t::ExtractAnyPackedValueX<float>(*final_cell);

                    if (maybe_float)
                    {
                        std::lock_guard<std::mutex> lock(collected_mutex);
                        collected.push_back(maybe_float.value());
                        stats.FinalCollected.fetch_add(1);
                        final_done.fetch_add(1);
                    }
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (auto& t : producers)
    {
        t.join();
    }

    producers_done.store(true, std::memory_order_release);
    std::cout << "All producers joined\n";

    for (auto& t : workers)
    {
        t.join();
    }

    std::cout << "All workers joined\n";

    const auto end_time = std::chrono::steady_clock::now();
    const auto runtime_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time
        ).count();

    std::sort(collected.begin(), collected.end());

    std::cout << "\n==== APC Strict Authoritative Occupancy / Bidirectional Causal Test ====\n";
    std::cout << "Runtime us             : " << runtime_us << "\n";
    std::cout << "Sensor FF produced     : " << stats.SensorFFProduced.load() << "\n";
    std::cout << "Predictor FB produced  : " << stats.PredictorFBProduced.load() << "\n";
    std::cout << "State integrated       : " << stats.StateIntegrated.load() << "\n";
    std::cout << "Error computed         : " << stats.ErrorComputed.load() << "\n";
    std::cout << "Forward emitted        : " << stats.ForwardEmitted.load() << "\n";
    std::cout << "Feedback emitted       : " << stats.FeedbackEmitted.load() << "\n";
    std::cout << "Final collected        : " << stats.FinalCollected.load() << "\n";
    std::cout << "Grow FF                : " << stats.GrowFF.load() << "\n";
    std::cout << "Grow FB                : " << stats.GrowFB.load() << "\n";
    std::cout << "Grow STATE             : " << stats.GrowSTATE.load() << "\n";
    std::cout << "Grow ERROR             : " << stats.GrowERROR.load() << "\n";
    std::cout << "Retry                  : " << stats.Retry.load() << "\n";
    std::cout << "Terminal fail          : " << stats.TerminalFail.load() << "\n";
    std::cout << "Older FF observed      : " << stats.OlderFFObserved.load() << "\n";
    std::cout << "Older FB observed      : " << stats.OlderFBObserved.load() << "\n";

    PrintNode("Sensor", Sensor);
    PrintNode("Predictor", Predictor);
    PrintNode("Comparator", Comparator);
    PrintNode("Integrator", Integrator);
    PrintNode("Motor", Motor);

    std::cout << "\nFirst 16 collected values:\n";
    for (size_t i = 0; i < std::min<size_t>(16, collected.size()); ++i)
    {
        std::cout << i << " -> " << collected[i] << "\n";
    }

    Motor.FreeAll();
    Integrator.FreeAll();
    Comparator.FreeAll();
    Predictor.FreeAll();
    Sensor.FreeAll();

    manager.StopAPCManager();
    return 0;
}