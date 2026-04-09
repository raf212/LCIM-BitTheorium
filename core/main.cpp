#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <array>
#include <memory>

#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

using namespace PredictedAdaptedEncoding;

namespace PredictedAdaptedEncoding
{
    static bool SharedChainEmpty_(
        AdaptivePackedCellContainer& any_member
    ) noexcept
    {
        if (!any_member.IfAPCBranchValid() || !any_member.GetBranchPlugin())
        {
            return true;
        }

        PackedCellContainerManager* manager_ptr = any_member.GetAPCManager();
        if (!manager_ptr)
        {
            return any_member.OccupancyAddOrSubAndGetAfterChange(0) == 0;
        }

        AdaptivePackedCellContainer* current = any_member.FindSharedRootOrThis();

        while (current)
        {
            if (current->OccupancyAddOrSubAndGetAfterChange(0) > 0)
            {
                return false;
            }

            PackedCellBranchPlugin* plugin = current->GetBranchPlugin();
            if (!plugin)
            {
                break;
            }

            const uint32_t next_id =
                plugin->ReadMetaCellValue32(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);

            if (next_id == NO_VAL || next_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                break;
            }

            AdaptivePackedCellContainer* next_ptr =
                manager_ptr->GetAPCPtrFromBranchId(next_id);

            if (!next_ptr || next_ptr == current)
            {
                break;
            }

            current = next_ptr;
        }

        return true;
    }

    static bool TryConsumeFromSharedChain_(
        AdaptivePackedCellContainer& any_member,
        size_t& root_scan_cursor,
        packed64_t& out_cell
    ) noexcept
    {
        PackedCellContainerManager* manager_ptr = any_member.GetAPCManager();
        if (!manager_ptr)
        {
            return any_member.ConsumeAndIdleGenericValueCell(root_scan_cursor, out_cell);
        }

        AdaptivePackedCellContainer* current = any_member.FindSharedRootOrThis();
        while (current)
        {
            if (current->ConsumeAndIdleGenericValueCell(root_scan_cursor, out_cell))
            {
                return true;
            }

            PackedCellBranchPlugin* plugin = current->GetBranchPlugin();
            if (!plugin)
            {
                break;
            }

            const uint32_t next_id =
                plugin->ReadMetaCellValue32(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);

            if (next_id == NO_VAL || next_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                break;
            }

            AdaptivePackedCellContainer* next_ptr =
                manager_ptr->GetAPCPtrFromBranchId(next_id);

            if (!next_ptr || next_ptr == current)
            {
                break;
            }

            current = next_ptr;
        }

        return false;
    }
}

namespace
{
    constexpr uint32_t VALUE_COUNT      = 256u;
    constexpr float    D_MULTIPLIER     = 0.5f;

    constexpr uint32_t PRODUCER_COUNT   = 2u;
    constexpr uint32_t B_WORKER_COUNT   = 3u;
    constexpr uint32_t C_WORKER_COUNT   = 2u;
    constexpr uint32_t D_WORKER_COUNT   = 2u;
    constexpr uint32_t E_WORKER_COUNT   = 1u;

    struct GraphStats
    {
        std::atomic<uint64_t> ProducedA{0};

        std::atomic<uint64_t> ConsumedB{0};
        std::atomic<uint64_t> ConsumedC{0};
        std::atomic<uint64_t> ConsumedD{0};
        std::atomic<uint64_t> ConsumedE{0};

        // terminal failures only
        std::atomic<uint64_t> TerminalPublishFailA{0};
        std::atomic<uint64_t> TerminalPublishFailB{0};
        std::atomic<uint64_t> TerminalPublishFailC{0};
        std::atomic<uint64_t> TerminalPublishFailD{0};

        // successful growth attempts
        std::atomic<uint64_t> SharedGrowA{0};
        std::atomic<uint64_t> SharedGrowB{0};
        std::atomic<uint64_t> SharedGrowC{0};
        std::atomic<uint64_t> SharedGrowD{0};

        std::atomic<uint64_t> StaleDroppedB{0};
        std::atomic<uint64_t> StaleDroppedC{0};
        std::atomic<uint64_t> StaleDroppedD{0};
        std::atomic<uint64_t> StaleDroppedE{0};

        // retry counts only
        std::atomic<uint64_t> RetryPublishA{0};
        std::atomic<uint64_t> RetryPublishB{0};
        std::atomic<uint64_t> RetryPublishC{0};
        std::atomic<uint64_t> RetryPublishD{0};
    };

    struct NodeCausalState
    {
        std::atomic<uint16_t> LastAcceptedClk16{0};
    };

    static inline uint32_t BitCastFloatToU32(float x) noexcept
    {
        uint32_t out = 0u;
        std::memcpy(&out, &x, sizeof(out));
        return out;
    }

    static inline packed64_t MakeStampedU32Cell(
        PackedCellContainerManager& manager,
        uint32_t value,
        tag8_t rel_mask = REL_NONE,
        tag8_t priority = ZERO_PRIORITY
    ) noexcept
    {
        return manager.GetMasterClockAdaptivePackedCellContainerManager()
            .ComposeValue32WithCurrentThreadStamp16(
                static_cast<val32_t>(value),
                rel_mask,
                priority,
                PackedCellLocalityTypes::ST_PUBLISHED,
                RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
                PackedCellDataType::UnsignedPCellDataType
            );
    }

    static inline packed64_t MakeStampedFloatCell(
        PackedCellContainerManager& manager,
        float value,
        tag8_t rel_mask = REL_NONE,
        tag8_t priority = ZERO_PRIORITY
    ) noexcept
    {
        return manager.GetMasterClockAdaptivePackedCellContainerManager()
            .ComposeValue32WithCurrentThreadStamp16(
                BitCastFloatToU32(value),
                rel_mask,
                priority,
                PackedCellLocalityTypes::ST_PUBLISHED,
                RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
                PackedCellDataType::FloatPCellDataType
            );
    }

    static inline bool AcceptByCausalClockDemo(
        NodeCausalState& state,
        packed64_t cell
    ) noexcept
    {
        const uint16_t incoming = PackedCell64_t::ExtractClk16(cell);
        uint16_t seen = state.LastAcceptedClk16.load(std::memory_order_relaxed);

        while (seen < incoming &&
               !state.LastAcceptedClk16.compare_exchange_weak(
                    seen,
                    incoming,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
        {
        }
        return true;
    }

    static bool TryPublishWithSharedGrowthOnce(
        AdaptivePackedCellContainer& root,
        packed64_t packed_cell,
        std::atomic<uint64_t>* growth_counter = nullptr
    ) noexcept
    {
        if (root.WriteGenericValueCellWithCASClaimedManager(packed_cell))
        {
            return true;
        }

        AdaptivePackedCellContainer* grown = root.GrowSharedNodeCheaply(true);
        if (grown != nullptr)
        {
            if (growth_counter)
            {
                growth_counter->fetch_add(1, std::memory_order_relaxed);
            }

            if (root.WriteGenericValueCellWithCASClaimedManager(packed_cell))
            {
                return true;
            }
        }

        return false;
    }

    static bool PublishUntilSuccessOrBudgetEnd(
        AdaptivePackedCellContainer& root,
        packed64_t packed_cell,
        PackedCellContainerManager& manager,
        std::atomic<uint64_t>* growth_counter,
        std::atomic<uint64_t>* retry_counter,
        uint32_t max_attempts = 4096
    ) noexcept
    {
        for (uint32_t attempt = 0; attempt < max_attempts; ++attempt)
        {
            if (TryPublishWithSharedGrowthOnce(root, packed_cell, growth_counter))
            {
                return true;
            }

            if (retry_counter)
            {
                retry_counter->fetch_add(1, std::memory_order_relaxed);
            }

            manager.GetCellsAdaptiveBackoffFromManager(packed_cell);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        return false;
    }

    static void PrintNodeSummary(
        const char* name,
        AdaptivePackedCellContainer& apc
    )
    {
        std::cout << name
                  << " branch=" << apc.GetBranchId()
                  << " logical=" << apc.GetLogicalId()
                  << " shared=" << apc.GetSharedId()
                  << " occ=" << apc.OccupancyAddOrSubAndGetAfterChange(0)
                  << "\n";
    }
}

int main()
{
    std::ios::sync_with_stdio(true);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    PackedCellContainerManager& manager = PackedCellContainerManager::Instance();
    manager.StartAPCManager();

    ContainerConf cfg;
    cfg.InitialMode = PackedMode::MODE_VALUE32;
    cfg.ProducerBlockSize = 8;
    cfg.RegionSize = 16;
    cfg.EnableBranching = true;
    cfg.BranchSplitThresholdPercentage = 20;
    cfg.BranchMaxDepth = 3;
    cfg.BranchMinChildCapacity = 256;
    cfg.NodeGroupSize = 1u;

    AdaptivePackedCellContainer A;
    AdaptivePackedCellContainer B;
    AdaptivePackedCellContainer C;
    AdaptivePackedCellContainer D;
    AdaptivePackedCellContainer E;

    A.SetManagerForGlobalAPC(&manager);
    B.SetManagerForGlobalAPC(&manager);
    C.SetManagerForGlobalAPC(&manager);
    D.SetManagerForGlobalAPC(&manager);
    E.SetManagerForGlobalAPC(&manager);

    A.InitAPCAsNode(
        256,
        cfg,
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::SELF_DATA_NODE),
        PackedCellBranchPlugin::APCNodeComputeKind::GENERATOR_UINT32,
        NO_VAL
    );

    B.InitAPCAsNode(
        256,
        cfg,
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::ACCEPTS_FEEDFORWARD) |
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::EMMITS_FEEDFORWARD),
        PackedCellBranchPlugin::APCNodeComputeKind::SQUARE_UINT32,
        NO_VAL
    );

    C.InitAPCAsNode(
        256,
        cfg,
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::ACCEPTS_FEEDFORWARD) |
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::EMMITS_FEEDFORWARD),
        PackedCellBranchPlugin::APCNodeComputeKind::ADD_UINT32,
        NO_VAL
    );

    D.InitAPCAsNode(
        256,
        cfg,
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::ACCEPTS_FEEDFORWARD) |
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::EMMITS_FEEDFORWARD),
        PackedCellBranchPlugin::APCNodeComputeKind::DIV_UINT32,
        NO_VAL
    );

    E.InitAPCAsNode(
        256,
        cfg,
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::ACCEPTS_FEEDFORWARD) |
        static_cast<uint32_t>(PackedCellBranchPlugin::APCBranchNodeRoleFlags::SELF_DATA_NODE),
        PackedCellBranchPlugin::APCNodeComputeKind::GENERIC_VECTOR,
        NO_VAL
    );

    GraphStats stats;

    std::array<NodeCausalState, B_WORKER_COUNT> causalB{};
    std::array<NodeCausalState, C_WORKER_COUNT> causalC{};
    std::array<NodeCausalState, D_WORKER_COUNT> causalD{};
    std::array<NodeCausalState, E_WORKER_COUNT> causalE{};

    std::vector<float> collected;
    std::mutex collected_mutex;

    std::atomic<bool> producers_done{false};

    std::atomic<uint64_t> total_done_B{0};
    std::atomic<uint64_t> total_done_C{0};
    std::atomic<uint64_t> total_done_D{0};
    std::atomic<uint64_t> total_done_E{0};

    std::vector<std::thread> producer_threads;
    std::vector<std::thread> worker_threads;

    for (uint32_t producer_id = 0; producer_id < PRODUCER_COUNT; ++producer_id)
    {
        producer_threads.emplace_back([&, producer_id]()
        {
            auto th = manager.RegisterAPCThread();

            for (uint32_t i = producer_id + 1; i <= VALUE_COUNT; i += PRODUCER_COUNT)
            {
                packed64_t cell = MakeStampedU32Cell(manager, i, REL_NODE0, ZERO_PRIORITY);

                if (PublishUntilSuccessOrBudgetEnd(
                        A,
                        cell,
                        manager,
                        &stats.SharedGrowA,
                        &stats.RetryPublishA))
                {
                    stats.ProducedA.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    stats.TerminalPublishFailA.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "A terminal publish failure for value " << i << "\n";
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (uint32_t worker_id = 0; worker_id < B_WORKER_COUNT; ++worker_id)
    {
        worker_threads.emplace_back([&, worker_id]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan_cursor = A.PayloadBegin();
            uint64_t idle_loops = 0;

            while (true)
            {
                if (total_done_B.load(std::memory_order_acquire) >= VALUE_COUNT)
                {
                    break;
                }

                packed64_t in = 0;
                if (!PredictedAdaptedEncoding::TryConsumeFromSharedChain_(A, scan_cursor, in))
                {
                    if (producers_done.load(std::memory_order_acquire) &&
                        PredictedAdaptedEncoding::SharedChainEmpty_(A) &&
                        total_done_B.load(std::memory_order_acquire) >= VALUE_COUNT)
                    {
                        break;
                    }

                    if ((++idle_loops & 0x3FFu) == 0u)
                    {
                        std::cout << "[B-" << worker_id << "] heartbeat done="
                                  << total_done_B.load() << "/" << VALUE_COUNT
                                  << " A_occ=" << A.OccupancyAddOrSubAndGetAfterChange(0)
                                  << "\n";
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                    continue;
                }

                idle_loops = 0;

                if (!AcceptByCausalClockDemo(causalB[worker_id], in))
                {
                    stats.StaleDroppedB.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (!APCSideHelper::IsCellPublishedMode32Generic(in))
                {
                    std::cerr << "B received non-unsigned payload cell\n";
                    continue;
                }

                const uint32_t x = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(in);
                const uint32_t y = x * x;
                packed64_t out = MakeStampedU32Cell(manager, y, REL_NODE1, ZERO_PRIORITY);

                if (PublishUntilSuccessOrBudgetEnd(
                        B,
                        out,
                        manager,
                        &stats.SharedGrowB,
                        &stats.RetryPublishB))
                {
                    stats.ConsumedB.fetch_add(1, std::memory_order_relaxed);
                    total_done_B.fetch_add(1, std::memory_order_release);
                }
                else
                {
                    stats.TerminalPublishFailB.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "B terminal publish failure for value " << y << "\n";
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (uint32_t worker_id = 0; worker_id < C_WORKER_COUNT; ++worker_id)
    {
        worker_threads.emplace_back([&, worker_id]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan_cursor = B.PayloadBegin();
            uint64_t idle_loops = 0;

            while (true)
            {
                if (total_done_C.load(std::memory_order_acquire) >= VALUE_COUNT)
                {
                    break;
                }

                packed64_t in = 0;
                if (!PredictedAdaptedEncoding::TryConsumeFromSharedChain_(B, scan_cursor, in))
                {
                    if (total_done_B.load(std::memory_order_acquire) >= VALUE_COUNT &&
                        PredictedAdaptedEncoding::SharedChainEmpty_(B) &&
                        total_done_C.load(std::memory_order_acquire) >= VALUE_COUNT)
                    {
                        break;
                    }

                    if ((++idle_loops & 0x3FFu) == 0u)
                    {
                        std::cout << "[C-" << worker_id << "] heartbeat done="
                                  << total_done_C.load() << "/" << VALUE_COUNT
                                  << " B_occ=" << B.OccupancyAddOrSubAndGetAfterChange(0)
                                  << "\n";
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                    continue;
                }

                idle_loops = 0;

                if (!AcceptByCausalClockDemo(causalC[worker_id], in))
                {
                    stats.StaleDroppedC.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (!APCSideHelper::IsCellPublishedMode32Generic(in))
                {
                    std::cerr << "C received non-unsigned payload cell\n";
                    continue;
                }

                const uint32_t x = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(in);
                const uint32_t y = x + 1u;
                packed64_t out = MakeStampedU32Cell(manager, y, REL_PAGE, ZERO_PRIORITY);

                if (PublishUntilSuccessOrBudgetEnd(
                        C,
                        out,
                        manager,
                        &stats.SharedGrowC,
                        &stats.RetryPublishC))
                {
                    stats.ConsumedC.fetch_add(1, std::memory_order_relaxed);
                    total_done_C.fetch_add(1, std::memory_order_release);
                }
                else
                {
                    stats.TerminalPublishFailC.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "C terminal publish failure for value " << y << "\n";
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (uint32_t worker_id = 0; worker_id < D_WORKER_COUNT; ++worker_id)
    {
        worker_threads.emplace_back([&, worker_id]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan_cursor = C.PayloadBegin();
            uint64_t idle_loops = 0;

            while (true)
            {
                if (total_done_D.load(std::memory_order_acquire) >= VALUE_COUNT)
                {
                    break;
                }

                packed64_t in = 0;
                if (!PredictedAdaptedEncoding::TryConsumeFromSharedChain_(C, scan_cursor, in))
                {
                    if (total_done_C.load(std::memory_order_acquire) >= VALUE_COUNT &&
                        PredictedAdaptedEncoding::SharedChainEmpty_(C) &&
                        total_done_D.load(std::memory_order_acquire) >= VALUE_COUNT)
                    {
                        break;
                    }

                    if ((++idle_loops & 0x3FFu) == 0u)
                    {
                        std::cout << "[D-" << worker_id << "] heartbeat done="
                                  << total_done_D.load() << "/" << VALUE_COUNT
                                  << " C_occ=" << C.OccupancyAddOrSubAndGetAfterChange(0)
                                  << "\n";
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                    continue;
                }

                idle_loops = 0;

                if (!AcceptByCausalClockDemo(causalD[worker_id], in))
                {
                    stats.StaleDroppedD.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (!APCSideHelper::IsCellPublishedMode32Generic(in))
                {
                    std::cerr << "D received non-unsigned payload cell\n";
                    continue;
                }

                const uint32_t x = PackedCell64_t::ExtractAnyPackedValueX<uint32_t>(in);
                const float y = static_cast<float>(x) * D_MULTIPLIER;
                packed64_t out = MakeStampedFloatCell(manager, y, REL_PATTERN, ZERO_PRIORITY);

                if (PublishUntilSuccessOrBudgetEnd(
                        D,
                        out,
                        manager,
                        &stats.SharedGrowD,
                        &stats.RetryPublishD))
                {
                    stats.ConsumedD.fetch_add(1, std::memory_order_relaxed);
                    total_done_D.fetch_add(1, std::memory_order_release);
                }
                else
                {
                    stats.TerminalPublishFailD.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "D terminal publish failure for value " << y << "\n";
                }
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (uint32_t worker_id = 0; worker_id < E_WORKER_COUNT; ++worker_id)
    {
        worker_threads.emplace_back([&, worker_id]()
        {
            auto th = manager.RegisterAPCThread();
            size_t scan_cursor = D.PayloadBegin();
            uint64_t idle_loops = 0;

            while (true)
            {
                if (total_done_E.load(std::memory_order_acquire) >= VALUE_COUNT)
                {
                    break;
                }

                packed64_t in = 0;
                if (!PredictedAdaptedEncoding::TryConsumeFromSharedChain_(D, scan_cursor, in))
                {
                    if (total_done_D.load(std::memory_order_acquire) >= VALUE_COUNT &&
                        PredictedAdaptedEncoding::SharedChainEmpty_(D) &&
                        total_done_E.load(std::memory_order_acquire) >= VALUE_COUNT)
                    {
                        break;
                    }

                    if ((++idle_loops & 0x3FFu) == 0u)
                    {
                        std::cout << "[E-" << worker_id << "] heartbeat done="
                                  << total_done_E.load() << "/" << VALUE_COUNT
                                  << " D_occ=" << D.OccupancyAddOrSubAndGetAfterChange(0)
                                  << "\n";
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                    continue;
                }

                idle_loops = 0;

                if (!AcceptByCausalClockDemo(causalE[worker_id], in))
                {
                    stats.StaleDroppedE.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (!APCSideHelper::IsMode32TypedPublishedCell<float>(in))
                {
                    std::cerr << "E received non-float payload cell\n";
                    continue;
                }

                const float value = PackedCell64_t::ExtractAnyPackedValueX<float>(in);
                {
                    std::lock_guard<std::mutex> lock(collected_mutex);
                    collected.push_back(value);
                }

                stats.ConsumedE.fetch_add(1, std::memory_order_relaxed);
                total_done_E.fetch_add(1, std::memory_order_release);
            }

            manager.UnRegisterAPCThread(th);
        });
    }

    for (auto& t : producer_threads)
    {
        t.join();
    }
    producers_done.store(true, std::memory_order_release);
    std::cout << "All producers joined\n";

    for (auto& t : worker_threads)
    {
        t.join();
    }
    std::cout << "All workers joined\n";

    std::sort(collected.begin(), collected.end());

    std::cout << "\n==== Current APC Shared-Node Concurrent Test (shared-chain fixed) ====\n";
    std::cout << "A produced values      : " << stats.ProducedA.load() << "\n";
    std::cout << "B squared              : " << stats.ConsumedB.load() << "\n";
    std::cout << "C add+1                : " << stats.ConsumedC.load() << "\n";
    std::cout << "D float*0.5            : " << stats.ConsumedD.load() << "\n";
    std::cout << "E collected            : " << stats.ConsumedE.load() << "\n";

    std::cout << "A terminal fail        : " << stats.TerminalPublishFailA.load() << "\n";
    std::cout << "B terminal fail        : " << stats.TerminalPublishFailB.load() << "\n";
    std::cout << "C terminal fail        : " << stats.TerminalPublishFailC.load() << "\n";
    std::cout << "D terminal fail        : " << stats.TerminalPublishFailD.load() << "\n";

    std::cout << "A shared grows         : " << stats.SharedGrowA.load() << "\n";
    std::cout << "B shared grows         : " << stats.SharedGrowB.load() << "\n";
    std::cout << "C shared grows         : " << stats.SharedGrowC.load() << "\n";
    std::cout << "D shared grows         : " << stats.SharedGrowD.load() << "\n";

    std::cout << "B stale dropped        : " << stats.StaleDroppedB.load() << "\n";
    std::cout << "C stale dropped        : " << stats.StaleDroppedC.load() << "\n";
    std::cout << "D stale dropped        : " << stats.StaleDroppedD.load() << "\n";
    std::cout << "E stale dropped        : " << stats.StaleDroppedE.load() << "\n";

    std::cout << "Retry publish A        : " << stats.RetryPublishA.load() << "\n";
    std::cout << "Retry publish B        : " << stats.RetryPublishB.load() << "\n";
    std::cout << "Retry publish C        : " << stats.RetryPublishC.load() << "\n";
    std::cout << "Retry publish D        : " << stats.RetryPublishD.load() << "\n";

    PrintNodeSummary("A", A);
    PrintNodeSummary("B", B);
    PrintNodeSummary("C", C);
    PrintNodeSummary("D", D);
    PrintNodeSummary("E", E);

    std::cout << "\nFirst 16 collected values:\n";
    for (size_t i = 0; i < std::min<size_t>(16, collected.size()); ++i)
    {
        std::cout << i << " -> " << collected[i] << "\n";
    }

    E.FreeAll();
    D.FreeAll();
    C.FreeAll();
    B.FreeAll();
    A.FreeAll();

    manager.StopPCCManager();
    return 0;
}