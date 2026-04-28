// prime_baseline_10_threads.cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using u64 = std::uint64_t;

constexpr uint64_t NTH_ELEMENT = 500000000ull;
constexpr uint64_t BLOCK_SIZE  = 2000000ull;
constexpr unsigned THREAD_COUNT = 10u;
constexpr u64 FIRST_PRIME = 2ull;

struct StopWatch
{
    using clock = std::chrono::steady_clock;
    clock::time_point start{clock::now()};

    void reset() noexcept
    {
        start = clock::now();
    }

    u64 us() const noexcept
    {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count()
        );
    }
};

static std::vector<u64> build_base_primes(u64 limit)
{
    std::vector<u64> primes;
    if (limit < 2)
    {
        return primes;
    }

    std::vector<bool> is_prime(limit + 1, true);
    is_prime[0] = false;
    is_prime[1] = false;

    for (u64 p = 2; p * p <= limit; ++p)
    {
        if (is_prime[p])
        {
            for (u64 q = p * p; q <= limit; q += p)
            {
                is_prime[q] = false;
            }
        }
    }

    for (u64 i = 2; i <= limit; ++i)
    {
        if (is_prime[i])
        {
            primes.push_back(i);
        }
    }

    return primes;
}

static void segmented_sieve(u64 left, u64 right,
                            const std::vector<u64>& base_primes,
                            std::vector<u64>& out)
{
    if (left > right || right < 2)
    {
        return;
    }

    if (left < FIRST_PRIME)
    {
        left = FIRST_PRIME;
    }

    const u64 width = right - left + 1;
    std::vector<bool> is_prime(width, true);

    for (u64 p : base_primes)
    {
        if (p * p > right)
        {
            break;
        }

        u64 start = (left + p - 1) / p * p;
        if (start < p * p)
        {
            start = p * p;
        }

        for (u64 x = start; x <= right; x += p)
        {
            is_prime[x - left] = false;
        }
    }

    for (u64 i = 0; i < width; ++i)
    {
        if (is_prime[i])
        {
            out.push_back(left + i);
        }
    }
}

struct WorkerStats
{
    u64 tasks = 0;
    u64 compute_us = 0;
};

int main()
{
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    StopWatch total_timer;

    std::cout << "Simple 10-thread segmented sieve baseline\n";
    std::cout << "Range: 2 .. " << NTH_ELEMENT << "\n";
    std::cout << "Block size: " << BLOCK_SIZE << "\n";
    std::cout << "Threads: " << THREAD_COUNT << "\n\n";

    const u64 sqrt_limit = static_cast<u64>(
        std::sqrt(static_cast<long double>(NTH_ELEMENT))
    ) + 1;

    std::vector<u64> base_primes = build_base_primes(sqrt_limit);

    std::vector<std::pair<u64, u64>> tasks;
    for (u64 lo = FIRST_PRIME; lo <= NTH_ELEMENT; lo += BLOCK_SIZE)
    {
        const u64 hi = std::min<u64>(NTH_ELEMENT, lo + BLOCK_SIZE - 1);
        tasks.emplace_back(lo, hi);
    }

    std::atomic<size_t> next_task{0};

    std::vector<std::vector<u64>> thread_results(THREAD_COUNT);
    std::vector<WorkerStats> stats(THREAD_COUNT);
    std::vector<std::thread> workers;
    workers.reserve(THREAD_COUNT);

    for (unsigned tid = 0; tid < THREAD_COUNT; ++tid)
    {
        workers.emplace_back([&, tid]()
        {
            StopWatch worker_timer;
            std::vector<u64> local_primes;
            local_primes.reserve(64'000);

            while (true)
            {
                const size_t idx = next_task.fetch_add(1, std::memory_order_relaxed);
                if (idx >= tasks.size())
                {
                    break;
                }

                const auto [lo, hi] = tasks[idx];

                StopWatch task_timer;
                segmented_sieve(lo, hi, base_primes, local_primes);
                const u64 task_us = task_timer.us();

                ++stats[tid].tasks;
                stats[tid].compute_us += task_us;
            }

            thread_results[tid] = std::move(local_primes);

            // If you want total thread lifetime instead of summed task time,
            // replace the line above with:
            // stats[tid].compute_us = worker_timer.us();
        });
    }

    for (auto& t : workers)
    {
        t.join();
    }

    StopWatch merge_timer;
    std::vector<u64> all_primes;
    for (auto& vec : thread_results)
    {
        all_primes.insert(all_primes.end(), vec.begin(), vec.end());
    }

    std::sort(all_primes.begin(), all_primes.end());
    all_primes.erase(std::unique(all_primes.begin(), all_primes.end()), all_primes.end());

    const u64 merge_us = merge_timer.us();
    const u64 total_us = total_timer.us();

    std::cout << "Found primes: " << all_primes.size() << "\n";
    std::cout << "First 20 primes:\n";
    for (size_t i = 0; i < std::min<size_t>(20, all_primes.size()); ++i)
    {
        std::cout << i << ": " << all_primes[i] << "\n";
    }

    std::cout << "\n==== Timing summary ====\n";
    std::cout << "Total runtime: " << total_us << " us (" << (double)total_us / 1e6 << " s)\n";
    std::cout << "Merge runtime: " << merge_us << " us (" << (double)merge_us / 1e6 << " s)\n";

    u64 sum_compute = 0;
    u64 max_compute = 0;
    u64 sum_tasks = 0;

    for (unsigned i = 0; i < THREAD_COUNT; ++i)
    {
        sum_compute += stats[i].compute_us;
        max_compute = std::max<u64>(max_compute, stats[i].compute_us);
        sum_tasks += stats[i].tasks;
    }

    std::cout << "Worker compute total: " << sum_compute << " us\n";
    std::cout << "Worker compute max:   " << max_compute << " us\n";
    std::cout << "Total tasks processed: " << sum_tasks << "\n";

    for (unsigned i = 0; i < THREAD_COUNT; ++i)
    {
        std::cout << "Thread " << i
                  << " -> tasks=" << stats[i].tasks
                  << ", compute=" << stats[i].compute_us << " us\n";
    }

    return 0;
}