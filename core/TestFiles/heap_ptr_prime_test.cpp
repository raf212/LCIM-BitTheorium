// main.cpp (fairer APC-vs-baseline comparison)
// Key fairness changes:
// 1) Build base primes ONCE and share them read-only across consumers.
// 2) Use APC only for TASK transport.
// 3) Remove RESULT_APC / collector thread / sort queue from the benchmark.
// 4) Each consumer accumulates local primes exactly like the baseline.
// 5) Final merge happens once in main, like the baseline.

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

#define FIRST_PRIME 2u

using namespace PredictedAdaptedEncoding;

constexpr uint64_t NTH_ELEMENT = 50000000ull;
constexpr uint64_t BLOCK_SIZE  = 2000000ull;
constexpr unsigned PRODUCER_COUNT = 2u;
constexpr unsigned CONSUMER_COUNT = 10u;
constexpr size_t MIN_APC_CAPACITY = 4096;

struct RangedTaskConf
{
    uint64_t StartingPoint;
    uint64_t EndPoint;

    RangedTaskConf(uint64_t start, uint64_t end) :
        StartingPoint(start), EndPoint(end)
    {}
};

struct StopWatch
{
    decltype(std::chrono::steady_clock::now()) StartStopWatch;

    StopWatch() noexcept :
        StartStopWatch(std::chrono::steady_clock::now())
    {}

    void ResetStopWatch() noexcept
    {
        StartStopWatch = std::chrono::steady_clock::now();
    }

    uint64_t ElapsedMicroSecond() const noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - StartStopWatch
            ).count()
        );
    }
};

struct AtomicTimerStatus
{
    std::atomic<uint64_t> Count{0};
    std::atomic<uint64_t> TotalMicroSecond{0};
    std::atomic<uint64_t> MaxMicroSecond{0};

    void AddSamplMicroSecond(uint64_t micro_second) noexcept
    {
        Count.fetch_add(1, std::memory_order_relaxed);
        TotalMicroSecond.fetch_add(micro_second, std::memory_order_relaxed);

        uint64_t previous = MaxMicroSecond.load(std::memory_order_relaxed);
        while (previous < micro_second &&
               !MaxMicroSecond.compare_exchange_weak(
                   previous, micro_second,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed))
        {
        }
    }

    uint64_t GetSampleCount() const noexcept
    {
        return Count.load(std::memory_order_relaxed);
    }

    uint64_t GetTotalMicroSeconds() const noexcept
    {
        return TotalMicroSecond.load(std::memory_order_relaxed);
    }

    uint64_t GetMaxMicroSecond() const noexcept
    {
        return MaxMicroSecond.load(std::memory_order_relaxed);
    }

    double AvarageMicoSecond() const noexcept
    {
        uint64_t samples = GetSampleCount();
        if (samples == 0)
        {
            return 0.0;
        }
        return static_cast<double>(GetTotalMicroSeconds()) / static_cast<double>(samples);
    }
};

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

int main()
{
    std::ios::sync_with_stdio(true);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::cout << "AdaptivePackedCellContainer fair task-transport prime test\n";
    std::cout << "Finding Upto Element : " << NTH_ELEMENT
              << ", Each Block Size : " << BLOCK_SIZE << "\n";

    StopWatch runtime_stop_watch;

    // Precompute base primes once: fairness fix.
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

    // Because pointer publication is pair-based in APC, keep extra slack.
    // Fairer and safer than a razor-thin num_tasks*2+16.
    const size_t apc_capacity =
        std::max<size_t>(MIN_APC_CAPACITY, num_of_tasks * 4 + 64);

    AdaptivePackedCellContainer TASK_APC;
    ContainerConf task_cfg;
    task_cfg.ProducerBlockSize = 64;
    task_cfg.InitialMode = PackedMode::MODE_VALUE32;

    TASK_APC.InitOwned(apc_capacity, task_cfg);
    TASK_APC.SetManagerForGlobalAPC(&apc_manager);
    apc_manager.ReclaimationRequestOfAPCSegmentFromManager_(&TASK_APC);

    std::atomic<size_t> producer_done{0};
    std::atomic<uint64_t> task_processed{0};
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
            uint64_t low  = task_ranges[i].first;
            uint64_t high = task_ranges[i].second;

            RangedTaskConf* range_task_conf_ptr = new RangedTaskConf(low, high);

            StopWatch operation_stop_watch;
            bool ok = TASK_APC.PublishHeapPtrWithAdaptiveBackoff(
                reinterpret_cast<void*>(range_task_conf_ptr));
            uint64_t elapsed_us = operation_stop_watch.ElapsedMicroSecond();
            producer_publish_timer.AddSamplMicroSecond(elapsed_us);

            if (!ok)
            {
                std::cerr << "Producer ID : " << producer_id
                          << " failed to publish the pointer of range from->"
                          << low << " to->" << high << "\n";
                delete range_task_conf_ptr;
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

        const size_t task_apc_capacity = TASK_APC.GetPayloadCapacity();
        if (task_apc_capacity == 0)
        {
            apc_manager.UnRegisterAPCThread(thread_handle_consumer);
            std::cout << "Consumer ID : " << worker_id << " No Work -> Early EXIT\n";
            return;
        }

        std::vector<uint64_t>& local_output = thread_results[worker_id];
        local_output.reserve(256000);

        size_t scan_offset = worker_id % task_apc_capacity;
        size_t local_loop_counter = 0;

        while (true)
        {
            bool did_work = false;

            for (size_t probe = 0; probe < task_apc_capacity; ++probe)
            {
                const size_t idx = (scan_offset + probe) % task_apc_capacity;

                auto acquired_paired_ptr_struct =
                    TASK_APC.AcquirePairedAtomicPtr(idx, true, 64);

                if (!acquired_paired_ptr_struct)
                {
                    packed64_t observed =
                        TASK_APC.BackingPtr ? TASK_APC.BackingPtr[idx].load(MoLoad_) : 0;
                    apc_manager.GetCellsAdaptiveBackoffFromManager(observed);
                    continue;
                }

                if (!acquired_paired_ptr_struct->Ownership)
                {
                    continue;
                }

                RangedTaskConf* range_task_ptr =
                    reinterpret_cast<RangedTaskConf*>(
                        reinterpret_cast<void*>(acquired_paired_ptr_struct->AssembeledPtr));

                if (range_task_ptr == nullptr)
                {
                    TASK_APC.RetireAcquiredPointerPair(*acquired_paired_ptr_struct);
                    retired_from_task.fetch_add(1, std::memory_order_relaxed);
                    did_work = true;
                    continue;
                }

                StopWatch compute_stop_watch;
                SegmentedSiveUsingBasePrimes(
                    range_task_ptr->StartingPoint,
                    range_task_ptr->EndPoint,
                    base_primes,
                    local_output);
                uint64_t compute_us = compute_stop_watch.ElapsedMicroSecond();

                consumer_compute_timer.AddSamplMicroSecond(compute_us);
                ++stats[worker_id].tasks;
                stats[worker_id].compute_us += compute_us;
                task_processed.fetch_add(1, std::memory_order_relaxed);

                delete range_task_ptr;
                TASK_APC.RetireAcquiredPointerPair(*acquired_paired_ptr_struct);

                did_work = true;
            }

            if ((++local_loop_counter & 0x3f) == 0)
            {
                std::cout << "consumer " << worker_id
                          << " heartbeat loops=" << local_loop_counter
                          << " retired_tasks=" << retired_from_task.load()
                          << " producers_done=" << producer_done.load() << "\n";
            }

            bool all_producer_done =
                producer_done.load(MoLoad_) >= PRODUCER_COUNT;
            size_t final_task_apc_occupancy =
                TASK_APC.AllPublishedCellsOccupancySnapshotAddOrSubAndGetAfterChange();

            if (all_producer_done && final_task_apc_occupancy == 0)
            {
                break;
            }

            if (!did_work)
            {
                std::this_thread::yield();
            }

            scan_offset = (scan_offset + 17) % task_apc_capacity;
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
    std::cout << "  task_apc_capacity: " << apc_capacity << "\n";
    std::cout << "  num_of_tasks: " << num_of_tasks << "\n";

    TASK_APC.FreeAll();
    apc_manager.StopAPCManager();

    std::cout << "Adaptive Packed Cell Container :: Fair Prime Test :: DONE\n";
    return 0;
}