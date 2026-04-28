#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cmath>

#include "APCSegmentsCausalCordinator.hpp"
#include "PackedCellContainerManager.hpp"

using namespace PredictedAdaptedEncoding;

namespace
{
    constexpr uint32_t VALUE_COUNT = 2560u;
    constexpr uint32_t PRODUCER_COUNT = 2u;
    constexpr uint32_t WORKERS_PER_STAGE = 3u;
    constexpr uint32_t MAX_ATTEMPTS = 4096u;

    struct GraphStats
    {
        std::atomic<uint64_t> ProducedSensor{0};
        std::atomic<uint64_t> ProducedBias{0};

        std::atomic<uint64_t> IntegratedState{0};
        std::atomic<uint64_t> ErrorComputed{0};
        std::atomic<uint64_t> ForwardEmitted{0};
        std::atomic<uint64_t> FeedbackEmitted{0};
        std::atomic<uint64_t> FinalCollected{0};

        std::atomic<uint64_t> GrowFF{0};
        std::atomic<uint64_t> GrowFB{0};
        std::atomic<uint64_t> GrowState{0};
        std::atomic<uint64_t> GrowError{0};
        std::atomic<uint64_t> GrowAux{0};

        std::atomic<uint64_t> OlderFF{0};
        std::atomic<uint64_t> OlderFB{0};
        std::atomic<uint64_t> Retry{0};
        std::atomic<uint64_t> TerminalFail{0};
    };

    static packed64_t PackU32(
        MasterClockConf& clock,
        uint32_t value,
        APCPagedNodeRelMaskClasses region,
        PriorityPhysics priority = PriorityPhysics::IDLE
    ) noexcept
    {
        return clock.ComposeValue32WithCurrentThreadStamp16(
            value,
            region,
            priority,
            PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType::UnsignedPCellDataType);
    }

    static bool PublishBudgeted(
        APCSegmentsCausalCordinator& node,
        APCPagedNodeRelMaskClasses region,
        packed64_t cell,
        PackedCellContainerManager& manager,
        std::atomic<uint64_t>* grow,
        GraphStats& stats
    ) noexcept
    {
        for (uint32_t i = 0; i < MAX_ATTEMPTS; ++i)
        {
            if (node.PublishCausal(region, cell, grow))
            {
                return true;
            }

            stats.Retry.fetch_add(1, std::memory_order_relaxed);
            manager.GetCellsAdaptiveBackoffFromManager(cell);
        }

        stats.TerminalFail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    static bool DoneAndDrained(
        std::atomic<uint64_t>& done,
        uint64_t expected,
        APCSegmentsCausalCordinator& node,
        APCPagedNodeRelMaskClasses region
    ) noexcept
    {
        return done.load(std::memory_order_acquire) >= expected &&
               !node.HasAnyPublishedInChain(region);
    }

    static void PrintNode(const char* name, APCSegmentsCausalCordinator& node)
    {

        std::cout << name
                << " branch=" << node.GetBranchId()
                << " logical=" << node.GetLogicalId()
                << " shared=" << node.GetSharedId()
                << " occ=" << node.AllPublishedCellsOccupancySnapshotAddOrSubAndGetAfterChange()
                << " exact_nonidle=" << node.GetLocalTotalOccupancy()
                << " FF=" << node.CountExactTotalChainOccupancy(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE)
                << " FB=" << node.CountExactTotalChainOccupancy(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE)
                << " STATE=" << node.CountExactTotalChainOccupancy(APCPagedNodeRelMaskClasses::STATE_SLOT)
                << " ERROR=" << node.CountExactTotalChainOccupancy(APCPagedNodeRelMaskClasses::ERROR_SLOT)
                << " chain_pub_aux=" << node.CountExactTotalChainOccupancy(APCPagedNodeRelMaskClasses::AUX_SLOT);
        std::cout << " accFF=" << node.ReadLastAcceptedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE)
                    << " emitFF=" << node.ReadLastEmittedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE)
                    << " accFB=" << node.ReadLastAcceptedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE)
                    << " emitFB=" << node.ReadLastEmittedClok16ForThisSegment(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE);
        std::cout << "\n";
    }
}

int main()
{
    std::ios::sync_with_stdio(true);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    PackedCellContainerManager& manager = PackedCellContainerManager::Instance();
    manager.StartAPCManager();

    Timer48 timer;
    MasterClockConf clock(nullptr, timer);

    ContainerConf cfg;
    cfg.InitialMode = PackedMode::MODE_VALUE32;
    cfg.ProducerBlockSize = 16;
    cfg.RegionSize = 16;
    cfg.EnableBranching = true;
    cfg.BranchSplitThresholdPercentage = 35;
    cfg.BranchMaxDepth = 8;
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

    Sensor.InitAPCAsNode(256, cfg, SegmentIODefinition::APCNodeComputeKind::GENERATOR_UINT32, NO_VAL);
    Predictor.InitAPCAsNode(256, cfg, SegmentIODefinition::APCNodeComputeKind::BIDIRECTIONAL_PREDECTIVE, NO_VAL);
    Comparator.InitAPCAsNode(256, cfg, SegmentIODefinition::APCNodeComputeKind::ADD_UINT32, NO_VAL);
    Integrator.InitAPCAsNode(256, cfg, SegmentIODefinition::APCNodeComputeKind::GENERIC_VECTOR, NO_VAL);
    Motor.InitAPCAsNode(256, cfg, SegmentIODefinition::APCNodeComputeKind::GENERIC_VECTOR, NO_VAL);

    GraphStats stats;
    std::vector<float> collected;
    std::mutex collected_mutex;

    std::atomic<bool> producers_done{false};
    std::atomic<uint64_t> state_done{0};
    std::atomic<uint64_t> error_done{0};
    std::atomic<uint64_t> forward_done{0};
    std::atomic<uint64_t> feedback_done{0};
    std::atomic<uint64_t> final_done{0};

    std::vector<std::thread> producers;
    std::vector<std::thread> workers;
    const auto start = std::chrono::steady_clock::now();

    auto TimedOut = [&]() noexcept
    {
        return std::chrono::steady_clock::now() - start > std::chrono::seconds(30);
    };
    // Sensor evidence producer: bottom-up spikes.
    for (uint32_t p = 0; p < PRODUCER_COUNT; ++p)
    {
        producers.emplace_back([&, p]()
        {
            auto th = manager.RegisterAPCThread();

            for (uint32_t x = p + 1; x <= VALUE_COUNT; x += PRODUCER_COUNT)
            {
                packed64_t ff = PackU32(clock, x, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, PriorityPhysics::IMPORTANT);

                if (PublishBudgeted(Sensor, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, ff,
                                    manager, &stats.GrowFF, stats))
                {
                    stats.ProducedSensor.fetch_add(1, std::memory_order_relaxed);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
        if (TimedOut())
        {
            std::cerr << "watchdog break\n";
            break;
        }
    }

    // Predictor producer: top-down bias/prediction stream.
    for (uint32_t p = 0; p < PRODUCER_COUNT; ++p)
    {
        producers.emplace_back([&, p]()
        {
            auto th = manager.RegisterAPCThread();

            for (uint32_t x = p + 1; x <= VALUE_COUNT; x += PRODUCER_COUNT)
            {
                const uint32_t prediction = x + 3u;
                packed64_t fb = PackU32(clock, prediction, APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE, PriorityPhysics::TIME_DEPENDENCY);

                if (PublishBudgeted(Predictor, APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE, fb,
                                    manager, &stats.GrowFB, stats))
                {
                    stats.ProducedBias.fetch_add(1, std::memory_order_relaxed);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
        if (TimedOut())
        {
            std::cerr << "watchdog break\n";
            break;
        }
    }

    // Stage 1: Sensor FF -> Integrator STATE.
    for (uint32_t w = 0; w < WORKERS_PER_STAGE; ++w)
    {
        workers.emplace_back([&, w]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan = Sensor.PayloadBegin();

            while (true)
            {
                if (DoneAndDrained(state_done, VALUE_COUNT, Sensor, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE))
                    break;

                auto maybe = Sensor.ConsumeCausal(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, scan, &stats.OlderFF);
                if (!maybe)
                {
                    manager.GetManagersAdaptiveBackoff().AutoBackoff();
                    continue;
                }

                const uint32_t x = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe);
                const uint32_t state = x * 2u;

                packed64_t state_cell = PackU32(clock, state, APCPagedNodeRelMaskClasses::STATE_SLOT, PriorityPhysics::IMPORTANT);

                if (PublishBudgeted(Integrator, APCPagedNodeRelMaskClasses::STATE_SLOT, state_cell,
                                    manager, &stats.GrowState, stats))
                {
                    stats.IntegratedState.fetch_add(1, std::memory_order_relaxed);
                    state_done.fetch_add(1, std::memory_order_release);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
        if (TimedOut())
        {
            std::cerr << "watchdog break\n";
            break;
        }
    }

    // Stage 2: Predictor FB -> Comparator ERROR.
    for (uint32_t w = 0; w < WORKERS_PER_STAGE; ++w)
    {
        workers.emplace_back([&, w]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan = Predictor.PayloadBegin();

            while (true)
            {
                if (DoneAndDrained(error_done, VALUE_COUNT, Predictor, APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE))
                    break;

                auto maybe = Predictor.ConsumeCausal(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE, scan, &stats.OlderFB);
                if (!maybe)
                {
                    manager.GetManagersAdaptiveBackoff().AutoBackoff();
                    continue;
                }

                const uint32_t pred = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe);
                const uint32_t err = (pred >= 3u) ? 3u : pred;

                packed64_t err_cell = PackU32(clock, err, APCPagedNodeRelMaskClasses::ERROR_SLOT, PriorityPhysics::ERROR_DEPENDENCY);

                if (PublishBudgeted(Comparator, APCPagedNodeRelMaskClasses::ERROR_SLOT, err_cell,
                                    manager, &stats.GrowError, stats))
                {
                    stats.ErrorComputed.fetch_add(1, std::memory_order_relaxed);
                    error_done.fetch_add(1, std::memory_order_release);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
        if (TimedOut())
        {
            std::cerr << "watchdog break\n";
            break;
        }
    }

    // Stage 3: Integrator STATE -> Motor FF consequence.
    for (uint32_t w = 0; w < WORKERS_PER_STAGE; ++w)
    {
        workers.emplace_back([&, w]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan = Integrator.PayloadBegin();

            while (true)
            {
                if (DoneAndDrained(forward_done, VALUE_COUNT, Integrator, APCPagedNodeRelMaskClasses::STATE_SLOT))
                    break;

                auto maybe = Integrator.ConsumeCausal(APCPagedNodeRelMaskClasses::STATE_SLOT, scan, &stats.OlderFF);
                if (!maybe)
                {
                    manager.GetManagersAdaptiveBackoff().AutoBackoff();
                    continue;
                }

                const uint32_t state = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe);
                const uint32_t forward = state + 1u;

                packed64_t out = PackU32(clock, forward, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, PriorityPhysics::IMPORTANT);

                if (PublishBudgeted(Motor, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, out,
                                    manager, &stats.GrowFF, stats))
                {
                    stats.ForwardEmitted.fetch_add(1, std::memory_order_relaxed);
                    forward_done.fetch_add(1, std::memory_order_release);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
        if (TimedOut())
        {
            std::cerr << "watchdog break\n";
            break;
        }
    }

    // Stage 4: Comparator ERROR -> Predictor FB correction.
    for (uint32_t w = 0; w < WORKERS_PER_STAGE; ++w)
    {
        workers.emplace_back([&, w]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan = Comparator.PayloadBegin();

            while (feedback_done.load(std::memory_order_acquire) < VALUE_COUNT)
            {
                auto maybe = Comparator.ConsumeCausal(
                    APCPagedNodeRelMaskClasses::ERROR_SLOT,
                    scan,
                    &stats.OlderFB
                );

                if (!maybe)
                {
                    manager.GetManagersAdaptiveBackoff().AutoBackoff();
                    continue;
                }

                const uint32_t err = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe);
                const uint32_t correction = 1000u + err;

                packed64_t fb = PackU32(
                    clock,
                    correction,
                    APCPagedNodeRelMaskClasses::AUX_SLOT,
                    PriorityPhysics::ERROR_DEPENDENCY
                );

                if (PublishBudgeted(
                        Predictor,
                        APCPagedNodeRelMaskClasses::AUX_SLOT,
                        fb,
                        manager,
                        &stats.GrowAux,
                        stats))
                {
                    stats.FeedbackEmitted.fetch_add(1, std::memory_order_relaxed);
                    feedback_done.fetch_add(1, std::memory_order_release);
                }
            }

            manager.UnRegisterAPCThread(th);
        });
        if (TimedOut())
        {
            std::cerr << "watchdog break\n";
            break;
        }
    }

    // Stage 5: Motor FF -> AUX final collection.
    workers.emplace_back([&]()
    {
        auto th = manager.RegisterAPCThread();
        size_t scan = Motor.PayloadBegin();

        while (true)
        {
            if (DoneAndDrained(final_done, VALUE_COUNT, Motor, APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE))
                break;

            auto maybe = Motor.ConsumeCausal(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, scan, &stats.OlderFF);
            if (!maybe)
            {
                manager.GetManagersAdaptiveBackoff().AutoBackoff();
                continue;
            }

            const uint32_t y = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(*maybe);
            const float out = static_cast<float>(y) * 0.5f;

            {
                std::lock_guard<std::mutex> lock(collected_mutex);
                collected.push_back(out);
            }

            stats.FinalCollected.fetch_add(1, std::memory_order_relaxed);
            final_done.fetch_add(1, std::memory_order_release);
            if (TimedOut())
            {
                std::cerr << "watchdog break\n";
                break;
            }
        }

        manager.UnRegisterAPCThread(th);
    });

    for (auto& t : producers) t.join();
    producers_done.store(true, std::memory_order_release);
    std::cout << "All producers joined\n";

    for (auto& t : workers) t.join();
    std::cout << "All workers joined\n";

    std::sort(collected.begin(), collected.end());

    std::cout << "\n==== APC Multidirectional Causal Neuromorphic Test ====\n";
    std::cout << "Sensor FF produced     : " << stats.ProducedSensor.load() << "\n";
    std::cout << "Predictor FB produced  : " << stats.ProducedBias.load() << "\n";
    std::cout << "State integrated       : " << stats.IntegratedState.load() << "\n";
    std::cout << "Error computed         : " << stats.ErrorComputed.load() << "\n";
    std::cout << "Forward emitted        : " << stats.ForwardEmitted.load() << "\n";
    std::cout << "Feedback emitted       : " << stats.FeedbackEmitted.load() << "\n";
    std::cout << "Final collected        : " << stats.FinalCollected.load() << "\n";

    std::cout << "Grow FF                : " << stats.GrowFF.load() << "\n";
    std::cout << "Grow FB                : " << stats.GrowFB.load() << "\n";
    std::cout << "Grow STATE             : " << stats.GrowState.load() << "\n";
    std::cout << "Grow ERROR             : " << stats.GrowError.load() << "\n";
    std::cout << "Retry                  : " << stats.Retry.load() << "\n";
    std::cout << "Terminal fail          : " << stats.TerminalFail.load() << "\n";
    std::cout << "Older FF observed      : " << stats.OlderFF.load() << "\n";
    std::cout << "Older FB observed      : " << stats.OlderFB.load() << "\n";

    PrintNode("Sensor    ", Sensor);
    PrintNode("Predictor ", Predictor);
    PrintNode("Comparator", Comparator);
    PrintNode("Integrator", Integrator);
    PrintNode("Motor     ", Motor);

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