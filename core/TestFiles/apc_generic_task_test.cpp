#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include "TestHelper.hpp"

#define FIRST_PRIME 2u

using namespace PredictedAdaptedEncoding;

constexpr unsigned MAX_TREE_LEAFS = 20u;

struct APCLeafBrunch
{
    std::unique_ptr<AdaptivePackedCellContainer> APC_UniqPtr;
    unsigned LogicalLeafIdx = 0;
};

static unsigned NextPowerOf2 (unsigned x) noexcept
{
    if (x <= 1u)
    {
        return 1u;
    }
    --x;
    x |= x >> 1u;
    x |= x >> 2u;
    x |= x >> 4u;
    x |= x >> 8u;
    x |= x >> 16u;
    return x + 1u;
}

// Build small/base primes ONCE, exactly like the baseline idea.
static std::vector<uint64_t> BuildBasePrimes(uint64_t limit)
{
    std::vector<uint64_t> primes;
    if (limit < 2)
    {
        return primes;
    }

    std::vector<char> is_prime(limit + 1, 1);
    is_prime[0] = 0;
    is_prime[1] = 0;

    for (uint64_t p = FIRST_PRIME; p * p <= limit; ++p)
    {
        if (is_prime[p])
        {
            for (uint64_t q = p * p; q <= limit; q += p)
            {
                is_prime[q] = 0;
            }
        }
    }

    for (uint64_t i = FIRST_PRIME; i <= limit; ++i)
    {
        if (is_prime[i])
        {
            primes.push_back(i);
        }
    }

    return primes;
}

// Fair segmented sieve: uses shared base primes, not rebuilding them every task.
static void SegmentedSiveUsingBasePrimes(
    uint64_t left,
    uint64_t right,
    const std::vector<uint64_t>& base_primes,
    std::vector<uint64_t>& primes_out)
{
    if (right < FIRST_PRIME || left > right)
    {
        return;
    }

    if (left < FIRST_PRIME)
    {
        left = FIRST_PRIME;
    }

    const uint64_t width = (right - left) + 1;
    std::vector<char> is_prime(width, 1);

    for (uint64_t p : base_primes)
    {
        if (p * p > right)
        {
            break;
        }

        uint64_t start = ((left + p - 1) / p) * p;
        if (start < p * p)
        {
            start = p * p;
        }

        for (uint64_t x = start; x <= right; x += p)
        {
            is_prime[x - left] = 0;
        }
    }

    for (uint64_t i = 0; i < width; ++i)
    {
        if (is_prime[i])
        {
            primes_out.push_back(left + i);
        }
    }
}

struct WorkerStats
{
    uint64_t tasks = 0;
    uint64_t compute_us = 0;
};

static inline packed64_t MakePublishedTaskCellWithStamp16(uint32_t task_id, PackedCellContainerManager& apc_mannager) noexcept
{
    MasterClockConf& thereds_master_clock = apc_mannager.GetMasterClockAdaptivePackedCellContainerManager();
    return thereds_master_clock.ComposeValue32WithCurrentThreadStamp16(
        static_cast<val32_t>(task_id),
        REL_ALL_LOW_4,
        ZERO_PRIORITY,
        PackedCellLocalityTypes::ST_PUBLISHED,
        RelOffsetMode32::RELOFFSET_GENERIC_VALUE
    );
}

static bool PublishTaskIdCell(
    AdaptivePackedCellContainer& apc_address,
    PackedCellContainerManager & apc_mannager_address,
    uint32_t task_id,
    uint16_t max_tries = 128
) noexcept
{
    if (!apc_address.IfAPCBranchValid())
    {
        return false;
    }
    const size_t payload_begin = apc_address.PayloadBegin();
    const size_t payload_capacity = apc_address.GetPayloadCapacity();
    uint16_t tries = 0;
    while (tries++ < max_tries)
    {
        size_t next_sequense = apc_address.NextProducerSequence();
        if (next_sequense == SIZE_MAX)
        {
            return false;
        }

        size_t idx = payload_begin + ((next_sequense - payload_begin) % payload_capacity);
        size_t step = 1u + ((next_sequense * ID_HASH_GOLDEN_CONST) % ((payload_capacity > 1) ? (payload_capacity - 1) : 1));
        for (size_t probs = 0; probs < payload_capacity; probs++)
        {
            packed64_t current_cell = apc_address.BackingPtr[idx].load(MoLoad_);
            PackedCellLocalityTypes cur_locality = PackedCell64_t::ExtractLocalityFromPacked(current_cell);
            if (cur_locality == PackedCellLocalityTypes::ST_IDLE)
            {
                packed64_t claimed_cell = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_PUBLISHED);
                packed64_t expected_cell = current_cell;
                if (apc_address.BackingPtr[idx].compare_exchange_strong(expected_cell, claimed_cell, OnExchangeSuccess, OnExchangeFailure))
                {
                    packed64_t published_cell = MakePublishedTaskCellWithStamp16(task_id, apc_mannager_address);
                    apc_address.BackingPtr[idx].store(published_cell, MoStoreSeq_);
                    apc_address.BackingPtr[idx].notify_all();
                    apc_address.OccupancyAddOrSubAndGetAfterChange(+1);
                    if (apc_address.GetBranchPlugin()->ShouldSplitNow())
                    {
                        apc_mannager_address.RequestAPCSegmentCreationFromManager_(&apc_address);
                    }
                    return true;
                }
            }
            idx = payload_begin + ((idx - payload_begin + step) % payload_capacity);
        }
        size_t observed_idx = payload_begin + (task_id % payload_capacity);
        apc_mannager_address.GetCellsAdaptiveBackoffFromManager(
            apc_address.BackingPtr[observed_idx].load(MoLoad_)
        );
    }
    return false;
}

static bool TryClaimOneTaskIdCell(
    AdaptivePackedCellContainer& apc_address,
    PackedCellContainerManager& manager_address,
    size_t& scan_cursor,
    uint32_t& out_task_id
) noexcept
{
    if (!apc_address.IfAPCBranchValid())
    {
        return false;
    }

    const size_t payload_begain = apc_address.PayloadBegin();
    const size_t payload_capacity = apc_address.GetPayloadCapacity();

    uint32_t current_occupancy = static_cast<uint32_t>(apc_address.OccupancyAddOrSubAndGetAfterChange());
    if (current_occupancy == 0)
    {
        return false;
    }
    
    for (size_t prob = 0; prob < payload_capacity; prob++)
    {
        size_t idx = payload_begain + ((scan_cursor - payload_begain + prob) % payload_capacity);
        packed64_t current_cell = apc_address.BackingPtr[idx].load(MoLoad_);
        if (PackedCell64_t::ExtractLocalityFromPacked(current_cell) != PackedCellLocalityTypes::ST_PUBLISHED)
        {
            continue;
        }

        if (static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(current_cell)) != RelOffsetMode32::RELOFFSET_GENERIC_VALUE)
        {
            continue;
        }
        packed64_t claimed_cell = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_CLAIMED);

        packed64_t expected_cell = current_cell;
        if (!apc_address.BackingPtr[idx].compare_exchange_strong(expected_cell, claimed_cell, OnExchangeSuccess, OnExchangeFailure))
        {
            manager_address.GetCellsAdaptiveBackoffFromManager(expected_cell);
            continue;
        }
        out_task_id = static_cast<uint32_t>(PackedCell64_t::ExtractValue32(current_cell));
        packed64_t idle_cell = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
        apc_address.BackingPtr[idx].store(idle_cell, MoStoreSeq_);
        apc_address.BackingPtr[idx].notify_all();
        apc_address.OccupancyAddOrSubAndGetAfterChange(-1);
        scan_cursor = idx + 1;
        if (scan_cursor >= payload_begain + payload_capacity)
        {
            scan_cursor = payload_begain;
        }
        return true;
    }
    return false;
}
///continue

int main()
{
    std::ios::sync_with_stdio(true);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::cout << "AdaptivePackedCellContainer fair task-transport prime test\n";
    std::cout << "Finding Upto Element : " << NTH_ELEMENT
              << ", Each Block Size : " << BLOCK_SIZE << "\n";

    StopWatch runtime_stop_watch;

    const uint64_t sqrt_limit =
        static_cast<uint64_t>(std::sqrt(static_cast<long double>(NTH_ELEMENT))) + 1;
    std::vector<uint64_t> base_primes = BuildBasePrimes(sqrt_limit);

    PackedCellContainerManager& apc_manager = PackedCellContainerManager::Instance();
    apc_manager.StartAPCManager();

    std::vector<std::pair<uint64_t, uint64_t>> task_ranges;
    for (uint64_t lo = FIRST_PRIME; lo <= NTH_ELEMENT; lo += BLOCK_SIZE)
    {
        uint64_t hi = std::min<uint64_t>(NTH_ELEMENT, lo + BLOCK_SIZE - 1);
        task_ranges.emplace_back(lo, hi);
    }

    const size_t num_of_tasks = task_ranges.size();
    if (num_of_tasks >= SegmentIODefinition::BRANCH_SENTINAL)
    {
        std::cerr << "numver of tasks should be less than UINT32_MAX";
        return 1;
    }

    const unsigned leaf_branch_count = std::min<unsigned>(MAX_TREE_LEAFS, NextPowerOf2(std::max(1u, std::min(CONSUMER_COUNT, MAX_TREE_LEAFS))));
    std::vector<APCLeafBrunch> leaf_branches_struct;
    leaf_branches_struct.resize(leaf_branch_count);
    ContainerConf branch_cfg_default;

    const size_t task_per_leaf_set = (num_of_tasks + leaf_branch_count -1) / leaf_branch_count;
    const size_t branch_capacity = std::max<size_t>(MINIMUM_BRANCH_CAPACITY, AdaptivePackedCellContainer::PayloadBegin() + task_per_leaf_set * 4 + 128);

    for (unsigned i = 0; i < leaf_branch_count; i++)
    {
        leaf_branches_struct[i].APC_UniqPtr = std::make_unique<AdaptivePackedCellContainer>();
        leaf_branches_struct[i].LogicalLeafIdx = i;
        leaf_branches_struct[i].APC_UniqPtr->InitOwned(branch_capacity, branch_cfg_default);
        leaf_branches_struct[i].APC_UniqPtr->SetManagerForGlobalAPC(&apc_manager);
        leaf_branches_struct[i].APC_UniqPtr->InitRegionIdx(branch_cfg_default.RegionSize);
        apc_manager.RequestAPCSegmentCreationFromManager_(leaf_branches_struct[i].APC_UniqPtr.get());
    }

    std::atomic<size_t> producer_done{0};
    std::atomic<uint64_t> task_processed{0};
    std::atomic<uint64_t> failed_task_publications{0};
    std::atomic<uint64_t> retired_from_task{0};

    AtomicTimerStatus producer_publish_timer;
    AtomicTimerStatus consumer_compute_timer;

    std::vector<WorkerStats> stats(CONSUMER_COUNT);
    std::vector<std::vector<uint64_t>> thread_results(CONSUMER_COUNT);

    auto producer_function = [&](unsigned producer_id)
    {
        std::cout << "Producer ID : " << producer_id << " -> starting\n";
        StopWatch local_stop_watch_producer;

        const size_t start_idx = (task_ranges.size() * producer_id) / PRODUCER_COUNT;
        const size_t end_idx   = (task_ranges.size() * (producer_id + 1)) / PRODUCER_COUNT;

        for (size_t i = start_idx; i < end_idx; ++i)
        {
            uint32_t task_id = static_cast<uint32_t>(i);
            const unsigned leaf = static_cast<unsigned>(task_id & (leaf_branch_count - 1u));
            AdaptivePackedCellContainer& target_producer_container = *leaf_branches_struct[leaf].APC_UniqPtr;
            StopWatch operation_stop_watch;
            bool ok = PublishTaskIdCell(target_producer_container, apc_manager, task_id, 256);
            uint64_t elapsed_us = operation_stop_watch.ElapsedMicroSecond();
            producer_publish_timer.AddSamplMicroSecond(elapsed_us);

            if (!ok)
            {
                std::cerr << "Producer ID : " << producer_id
                          << " failed to publish task id = "
                          << task_id << "-> into leaf = " << leaf << "\n";
            }
        }
        uint64_t total_loop_us = local_stop_watch_producer.ElapsedMicroSecond();
        producer_done.fetch_add(1, std::memory_order_release);

        std::cout << "Producer ID : " << producer_id
                  << " -> Done || Elapsed Time : " << total_loop_us << " us\n";
    };

    auto consumer_function = [&](unsigned worker_id)
    {
        std::cout << "Consumer ID : " << worker_id << " started\n";

        PackedCellContainerManager::ThreadHandlePCCM thread_handle_consumer =
            apc_manager.RegisterAPCThread();

        std::vector<uint64_t>& local_output = thread_results[worker_id];
        local_output.reserve(256000);

        std::vector<size_t>scan_offsets(leaf_branch_count, 0);
        for (unsigned branch = 0; branch < leaf_branch_count; branch++)
        {
            scan_offsets[branch] = AdaptivePackedCellContainer::PayloadBegin();
        }
        const unsigned preffered_leaf = worker_id & (leaf_branch_count - 1u);
        size_t idle_loops = 0;
        while (true)
        {
            bool did_work = false;

            for (unsigned hop = 0; hop < leaf_branch_count; hop++)
            {
                const unsigned current_leaf = (preffered_leaf + hop) & (leaf_branch_count - 1u);
                AdaptivePackedCellContainer& current_branch_address = *leaf_branches_struct[current_leaf].APC_UniqPtr;
                uint32_t current_task_id = 0;
                if (!TryClaimOneTaskIdCell(current_branch_address, apc_manager, scan_offsets[current_leaf], current_task_id))
                {
                    continue;
                }

                const uint64_t low = task_ranges[current_task_id].first;
                const uint64_t high = task_ranges[current_task_id].second;
                StopWatch compute_watch;
                SegmentedSiveUsingBasePrimes(low, high, base_primes, local_output);
                uint64_t compute_us = compute_watch.ElapsedMicroSecond();

                consumer_compute_timer.AddSamplMicroSecond(compute_us);
                ++stats[worker_id].tasks;
                stats[worker_id].compute_us +=compute_us;
                task_processed.fetch_add(1, std::memory_order_relaxed);
                did_work = true;
                idle_loops = 0;
                break;
            }
            if (!did_work)
            {
                ++idle_loops;
                bool all_done = producer_done.load(MoLoad_) >= PRODUCER_COUNT;
                bool any_work_left = false;
                for (unsigned branch = 0; branch < leaf_branch_count; branch++)
                {
                    if (leaf_branches_struct[branch].APC_UniqPtr->OccupancyAddOrSubAndGetAfterChange() > 0)
                    {
                        any_work_left = true;
                        break;
                    }
                }
                if (all_done && !any_work_left)
                {
                    break;
                }
                if ((idle_loops & 0x3f) == 0)
                {
                    std::this_thread::yield();
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                }

            }
            

            if ((++idle_loops & 0x3f) == 0)
            {
                std::cout << "consumer " << worker_id
                          << " heartbeat loops=" << idle_loops
                          << " retired_tasks=" << retired_from_task.load()
                          << " producers_done=" << producer_done.load() << "\n";
            }
        }

        apc_manager.UnRegisterAPCThread(thread_handle_consumer);
        std::cout << "Consumer stopped ID : " << worker_id << "\n";
    };

    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < PRODUCER_COUNT; ++producer)
    {
        producers.emplace_back(producer_function, producer);
    }

    std::vector<std::thread> consumers;
    consumers.reserve(CONSUMER_COUNT);
    for (unsigned consumer = 0; consumer < CONSUMER_COUNT; ++consumer)
    {
        consumers.emplace_back(consumer_function, consumer);
    }

    for (auto& thrd : producers)
    {
        thrd.join();
    }
    std::cout << "All producers joined\n";

    for (auto& thrd : consumers)
    {
        thrd.join();
    }
    std::cout << "All consumers joined\n";

    // Final merge: same style as baseline.
    StopWatch merge_stop_watch;
    std::vector<uint64_t> all_primes_array;
    for (auto& vec : thread_results)
    {
        all_primes_array.insert(all_primes_array.end(), vec.begin(), vec.end());
    }

    std::sort(all_primes_array.begin(), all_primes_array.end());
    all_primes_array.erase(
        std::unique(all_primes_array.begin(), all_primes_array.end()),
        all_primes_array.end());
    uint64_t merge_us = merge_stop_watch.ElapsedMicroSecond();

    std::cout << "Found number of Primes : " << all_primes_array.size() << "\n";
    for (size_t i = 0; i < std::min<size_t>(20u, all_primes_array.size()); ++i)
    {
        std::cout << "Position : " << i
                  << " Prime number : " << all_primes_array[i] << "\n";
    }

    auto print_us = [](uint64_t us) {
        std::ostringstream ss;
        ss << us << " us (" << std::fixed << std::setprecision(3)
           << static_cast<double>(us) / 1e6 << " s)";
        return ss.str();
    };

    std::cout << "\n==== Instrumentation Summary ====\n";
    std::cout << "Total runtime (steady): "
              << print_us(runtime_stop_watch.ElapsedMicroSecond()) << "\n";
    std::cout << "Final merge runtime: "
              << print_us(merge_us) << "\n";

    std::cout << "\nProducer publish stats:\n";
    std::cout << "  samples: " << producer_publish_timer.GetSampleCount()
              << "  total: "   << print_us(producer_publish_timer.GetTotalMicroSeconds())
              << "  avg: "     << std::fixed << std::setprecision(3)
              << producer_publish_timer.AvarageMicoSecond() << " us"
              << "  max: "     << print_us(producer_publish_timer.GetMaxMicroSecond())
              << "\n";

    std::cout << "\nConsumer compute stats (per-task):\n";
    std::cout << "  tasks processed: " << task_processed.load()
              << "  total compute: "   << print_us(consumer_compute_timer.GetTotalMicroSeconds())
              << "  avg compute: "     << std::fixed << std::setprecision(3)
              << consumer_compute_timer.AvarageMicoSecond() << " us"
              << "  max compute: "     << print_us(consumer_compute_timer.GetMaxMicroSecond())
              << "\n";

    std::cout << "\nPer-thread stats:\n";
    for (unsigned i = 0; i < CONSUMER_COUNT; ++i)
    {
        std::cout << "  Thread " << i
                  << " -> tasks=" << stats[i].tasks
                  << ", compute=" << stats[i].compute_us << " us\n";
    }

    std::cout << "\nAPC counts:\n";
    std::cout << "  retired_from_task: " << retired_from_task.load() << "\n";
    std::cout << "  task_apc_capacity: " << branch_capacity << "\n";
    std::cout << "  num_of_tasks: " << num_of_tasks << "\n";

    std::cout << "Branch Status \n";
    

    for (auto& branch : leaf_branches_struct)
    {
        if (branch.APC_UniqPtr)
        {
            branch.APC_UniqPtr->FreeAll();
        }
    }
    apc_manager.StopAPCManager();

    std::cout << "Adaptive Packed Cell Container :: Fair Prime Test :: DONE\n";
    return 0;
}