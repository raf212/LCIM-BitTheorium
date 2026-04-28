#pragma once

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

constexpr uint64_t NTH_ELEMENT = 50000000ull;
constexpr uint64_t BLOCK_SIZE  = 2000000ull;
constexpr unsigned PRODUCER_COUNT = 2u;
constexpr unsigned CONSUMER_COUNT = 10u;


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
