#pragma once

#include <type_traits>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <optional>
#include <vector>
#include <algorithm>
#include <limits>
#include <cassert>
#include <cstddef>
#include <array>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <memory>
#include <latch> // For std::latch (C++20)

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace AtomicCScompact {
    using packed64_t = uint64_t;
    using val32_t    = uint32_t;
    using clk16_t    = uint16_t;
    using tag8_t     = uint8_t;
    using strl16_t   = uint16_t;

    #define NO_VAL 0u
    #define MAX_VAL 64u
    #define LN_OF_BYTE_IN_BITS 8u
    #define MASK_8_BIT  0xFFu
    #define RELATION_MASK_5 0x1Fu
    #define RELATION_PRIORITY 0x07u
    #define ID_HASH_GOLDEN_CONST 0x9E3779B97F4A7C15ull 
    #define ATOMIC_THRESHOLD 64u

    static constexpr unsigned MASK16B_HIGH8B_0 = 0xFF00u;

    static constexpr ::std::memory_order MoLoad_      = ::std::memory_order_acquire;
    static constexpr ::std::memory_order MoStoreSeq_  = ::std::memory_order_release;
    static constexpr ::std::memory_order MoStoreUnSeq_= ::std::memory_order_relaxed;
    static constexpr ::std::memory_order EXsuccess_   = ::std::memory_order_acq_rel;
    static constexpr ::std::memory_order EXfailure_   = ::std::memory_order_relaxed;

    static constexpr unsigned CLK_B48 = 48u;
    static constexpr unsigned VALBITS  = 32u;
    static constexpr unsigned CLK_B16  = 16u;
    static constexpr unsigned STRL_B16  = 16u;
    static constexpr unsigned STBITS   = 8u;
    static constexpr unsigned RELBITS  = 8u;
    static constexpr unsigned TOTAL_LOW = 48u;
    static constexpr unsigned MASK_OF_RELBIT = 5u;
    static constexpr unsigned CLK48TO16_PACKED_ERROR = 16u;


// States (8-bit) - keep your canonical names
static constexpr tag8_t ST_IDLE        = 0x00;
static constexpr tag8_t ST_PUBLISHED   = 0x01;
static constexpr tag8_t ST_PENDING     = 0x02;
static constexpr tag8_t ST_CLAIMED     = 0x03;
static constexpr tag8_t ST_PROCESSING  = 0x04;
static constexpr tag8_t ST_COMPLETE    = 0x05;
static constexpr tag8_t ST_RETIRED     = 0x06;
static constexpr tag8_t ST_EPOCH_BUMP  = 0x07;
static constexpr tag8_t ST_LOCKED      = 0x08;

// Relation masks: keep lower-5-bit usage for relation masks (0..4). Priority will live in bits 7..5.
static constexpr tag8_t REL_NONE        = 0x00;
static constexpr tag8_t REL_NODE0       = 0x01;
static constexpr tag8_t REL_NODE1       = 0x02;
static constexpr tag8_t REL_PAGE        = 0x04;
static constexpr tag8_t REL_PATTERN     = 0x08;
static constexpr tag8_t REL_SELF        = 0x10;
static constexpr tag8_t REL_ALL_LOW5    = 0x1F; // all low-5 relation bits
static constexpr uint8_t MAX_PRIORITY   = 7;



}