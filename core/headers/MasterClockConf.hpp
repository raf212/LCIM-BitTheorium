#pragma once 
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <optional>
#include <cassert>
#include <limits>

#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp" 


namespace AtomicCScompact
{
    struct MasterClockConf
    {
        //master clock
        std::atomic<packed64_t>* MasterClockSlotsPtr_{nullptr};
        size_t MasterCLKCapacity_{0};
        std::atomic<size_t> MasterAlloc_{0};
    };

}