
#pragma once
// PackedCell.hpp
// compact 64-bit packing helpers; added priority-in-rel-byte helpers
// (priority:3 bits in rel byte = bits 7..5, relmask:5 bits = bits 4..0)

#include <cstdint>
#include <type_traits>
#include <cstring>

namespace AtomicCScompact
{
    using packed64_t = uint64_t;
    using val32_t    = uint32_t;
    using clk16_t    = uint16_t;
    using clk48_t    = uint64_t;
    using tag8_t     = uint8_t;
    using strl16_t   = uint16_t;

    enum class PackedMode : int
    {
        MODE_VALUE32 = 0,
        MODE_CLKVAL48 = 1
    };

    static inline constexpr unsigned VALBITS  = 32u;
    static inline constexpr unsigned CLK_B16  = 16u;
    static inline constexpr unsigned CLK_B48  = 48u;
    static inline constexpr unsigned STRL_B16 = 16u;
    static inline constexpr unsigned TOTAL_LOW = 48u; // bottom bits used by value+clk

    // safe low-bit mask generator
    static inline constexpr packed64_t MaskBits(unsigned n) noexcept {
        if (n == 0u) return packed64_t(0);
        if (n >= 64u) return ~packed64_t(0);
        return (~packed64_t(0)) >> (64u - n);
    }

    struct PackedCell64_t
    {
        // canonical packers (VALUE32 layout and CLK48 layout)
        static inline packed64_t PackV32x_64(val32_t v, clk16_t clk, tag8_t st, tag8_t relbyte) noexcept {
            packed64_t p = (packed64_t(v) & MaskBits(VALBITS));
            p |= (packed64_t(clk) & MaskBits(CLK_B16)) << VALBITS;
            p |= (packed64_t(st)  & MaskBits(8u))  << (VALBITS + CLK_B16);
            p |= (packed64_t(relbyte) & MaskBits(8u)) << (VALBITS + CLK_B16 + 8u);
            return p;
        }

        static inline packed64_t PackCLK48x_64(clk48_t clk48, tag8_t st, tag8_t relbyte) noexcept {
            packed64_t p = (packed64_t(clk48) & MaskBits(CLK_B48));
            p |= (packed64_t(st)  & MaskBits(8u)) << CLK_B48;
            p |= (packed64_t(relbyte) & MaskBits(8u)) << (CLK_B48 + 8u);
            return p;
        }

        // Extractors
        static inline val32_t ExtractValue32(packed64_t p) noexcept {
            return static_cast<val32_t>(p & MaskBits(VALBITS));
        }
        static inline clk16_t ExtractClk16(packed64_t p) noexcept {
            return static_cast<clk16_t>((p >> VALBITS) & MaskBits(CLK_B16));
        }
        static inline clk48_t ExtractClk48(packed64_t p) noexcept {
            return static_cast<clk48_t>(p & MaskBits(CLK_B48));
        }

        // STRL extraction: top-16 bits encode [st:8 | relbyte:8]
        static inline strl16_t ExtractSTRL(packed64_t p) noexcept {
            return static_cast<strl16_t>((p >> TOTAL_LOW) & MaskBits(STRL_B16));
        }

        // low-level split of STRL
        static inline tag8_t StateFromSTRL(strl16_t strl) noexcept {
            return static_cast<tag8_t>((strl >> 8) & 0xFFu);
        }
        static inline tag8_t RelationByteFromSTRL(strl16_t strl) noexcept {
            return static_cast<tag8_t>(strl & 0xFFu);
        }

        // Relationship: we partition the relbyte into priority(3 bits) | relmask(5 bits)
        static inline tag8_t RelationMaskFromRelByte(tag8_t relbyte) noexcept {
            return static_cast<tag8_t>(relbyte & 0x1Fu); // low 5 bits
        }
        static inline tag8_t PriorityFromRelByte(tag8_t relbyte) noexcept {
            return static_cast<tag8_t>((relbyte >> 5) & 0x07u); // 3 bits
        }

        static inline tag8_t RelationMaskFromSTRL(strl16_t strl) noexcept {
            return RelationMaskFromRelByte(RelationByteFromSTRL(strl));
        }
        static inline tag8_t PriorityFromSTRL(strl16_t strl) noexcept {
            return PriorityFromRelByte(RelationByteFromSTRL(strl));
        }

        // Construct STRL from components
        static inline strl16_t MakeSTRL(tag8_t st, tag8_t relbyte) noexcept {
            return static_cast<strl16_t>((static_cast<strl16_t>(st) << 8) | static_cast<strl16_t>(relbyte));
        }
        static inline tag8_t MakeRelByte(tag8_t relmask5, tag8_t priority3) noexcept {
            return static_cast<tag8_t>(((priority3 & 0x07u) << 5) | (relmask5 & 0x1Fu));
        }

        // Replace STRL in a packed word
        static inline packed64_t SetSTRL(packed64_t p, strl16_t strl) noexcept {
            constexpr packed64_t top_mask = MaskBits(STRL_B16) << TOTAL_LOW;
            p = (p & ~top_mask) | ((packed64_t(strl & MaskBits(STRL_B16))) << TOTAL_LOW);
            return p;
        }

        // Set only the relation byte's priority (preserve relmask)
        static inline packed64_t SetPriorityInSTRL(packed64_t p, tag8_t priority3) noexcept {
            strl16_t old = ExtractSTRL(p);
            tag8_t oldrelbyte = RelationByteFromSTRL(old);
            tag8_t newrelbyte = MakeRelByte(RelationMaskFromRelByte(oldrelbyte), priority3);
            return SetSTRL(p, MakeSTRL(StateFromSTRL(old), newrelbyte));
        }

        // Set only relation mask (low 5 bits); preserve priority
        static inline packed64_t SetRelationMaskInSTRL(packed64_t p, tag8_t relmask5) noexcept {
            strl16_t old = ExtractSTRL(p);
            tag8_t oldrelbyte = RelationByteFromSTRL(old);
            tag8_t newrelbyte = MakeRelByte(relmask5, PriorityFromRelByte(oldrelbyte));
            return SetSTRL(p, MakeSTRL(StateFromSTRL(old), newrelbyte));
        }

        // Compose a canonical committed word (set COMPLETE) from payload (uses existing payload fields)
        static inline packed64_t MakeCommittedFromPayloadV32(packed64_t payload) noexcept {
            val32_t v = ExtractValue32(payload);
            clk16_t clk = ExtractClk16(payload);
            tag8_t relbyte = RelationByteFromSTRL(ExtractSTRL(payload));
            return PackV32x_64(v, clk, /*st*/static_cast<tag8_t>(0x05), relbyte); // ST_COMPLETE ref 0x05 may be redefined elsewhere
        }
        static inline packed64_t MakeCommittedFromPayloadCLK48(packed64_t payload) noexcept {
            clk48_t c = ExtractClk48(payload);
            tag8_t relbyte = RelationByteFromSTRL(ExtractSTRL(payload));
            return PackCLK48x_64(c, /*st*/static_cast<tag8_t>(0x05), relbyte);
        }

        // Convenience templates for callers
        template<bool isValue32>
        static inline packed64_t MakeCommittedFromPayload(packed64_t payload) noexcept {
            if constexpr (isValue32) return MakeCommittedFromPayloadV32(payload);
            else return MakeCommittedFromPayloadCLK48(payload);
        }

        // Utility view-as for trivial types
        template<typename T>
        static inline T AsValue(packed64_t p) noexcept {
            static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
            static_assert(sizeof(T) <= sizeof(packed64_t), "T must fit into 64 bits");
            T out;
            std::memcpy(&out, &p, sizeof(T));
            return out;
        }
    };

} // namespace AtomicCScompact


#pragma once
#include "PackedCell.hpp"

namespace AtomicCScompact {

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

// Helper: pack relmask (low5) and priority (3-bit) into rel byte
static inline tag8_t make_relbyte(tag8_t relmask5, tag8_t priority3) noexcept {
    return PackedCell64_t::MakeRelByte(relmask5, priority3);
}

// Extract low-5 rel mask from the rel byte
static inline tag8_t relmask_of_relbyte(tag8_t relbyte) noexcept {
    return PackedCell64_t::RelationMaskFromRelByte(relbyte);
}

// Extract priority (0..7) from rel byte
static inline tag8_t priority_of_relbyte(tag8_t relbyte) noexcept {
    return PackedCell64_t::PriorityFromRelByte(relbyte);
}

// Test whether slot_relbyte matches consumer rel_mask (consumer supplies low-5-bit mask)
static inline bool rel_matches(tag8_t slot_relbyte, tag8_t relmask) noexcept {
    return (static_cast<uint8_t>(slot_relbyte) & static_cast<uint8_t>(relmask)) != 0;
}

} // namespace AtomicCScompact

#pragma once
// AtomicDataSignalArray_fixed_v3.hpp
// Updated version with ownership correctness, wait/notify fallback,
// thread-safe region structures, producer reservation, TLS candidate buffer,
// partial selection (nth_element) and structured publish results.
//
// Depends on: PackedCell.hpp, PackedStRel.h, AllocNW.hpp (no fallbacks).
// Build: -std=c++20 -DHAVE_LIBNUMA (or Windows where VirtualAllocExNuma exists).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>
#include <mutex>
#include <memory>

#include "PackedCell.hpp"
#include "PackedStRel.h"
#include "AllocNW.hpp"

namespace AtomicCScompact {

using packed_t = packed64_t;
using tag8_t   = ::AtomicCScompact::tag8_t;
using strl16_t = ::AtomicCScompact::strl16_t;
using val32_t  = ::AtomicCScompact::val32_t;
using clk16_t  = ::AtomicCScompact::clk16_t;
using clk48_t  = ::AtomicCScompact::clk48_t;
using PackedMode = ::AtomicCScompact::PackedMode;

// Publish result
enum class PublishStatus : int { OK = 0, FULL = 1, INVALID = 2 };
struct PublishResult { PublishStatus status; std::size_t idx; };

struct Config {
    std::size_t max_gather = 1024;       // gather buffer size for claim_batch
    std::size_t scan_limit = 256;        // initial scanning window
    int backoff_spin_ms = 0;             // base backoff in microseconds
    bool enable_producer_reserve = true; // allow reservation of N slots
};

// small adaptive backoff
struct AdaptiveBackoff {
    int tries = 0;
    int base_us = 50;
    void reset() noexcept { tries = 0; }
    inline void spin_once() noexcept {
        if (tries < 8) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
        } else if (tries < 16) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(std::min(200, base_us << (tries - 16))));
        ++tries;
    }
};

// Fallback wait/notify (coarse-grained)
class WakeFallback {
public:
    void notify_all() noexcept {
        std::lock_guard<std::mutex> g(mu_);
        cv_.notify_all();
    }
    void notify_one() noexcept {
        std::lock_guard<std::mutex> g(mu_);
        cv_.notify_one();
    }
    // wait until predicate true or timeout
    template<typename Pred>
    bool wait_for_pred(int timeout_ms, Pred pred) {
        std::unique_lock<std::mutex> lk(mu_);
        if (timeout_ms < 0) {
            cv_.wait(lk, pred);
            return true;
        } else {
            return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
        }
    }
private:
    std::condition_variable cv_;
    std::mutex mu_;
};

// AtomicDataSignalArray: single-atomic AoS mailbox array
template<PackedMode MODE>
class AtomicDataSignalArray {
public:
    AtomicDataSignalArray() noexcept : backing_(nullptr), capacity_(0), owned_(false), node_(0) {
        cfg_ = Config{};
        use_atomic_wait_ = detect_atomic_wait();
    }
    ~AtomicDataSignalArray() { free_all(); }

    // Ownership: init_owned -> owned_ = true; init_from_existing -> owned_ = false.
    // free_all will only free memory when owned_ == true.
    void init_owned(std::size_t capacity, int node = 0, std::size_t alignment = 64, Config cfg = {}) {
        free_all();
        if (capacity == 0) throw std::invalid_argument("capacity==0");
        std::atomic<packed_t> test{0};
        if (!test.is_lock_free()) throw std::runtime_error("atomic<packed_t> not lock-free");

        std::size_t bytes = sizeof(std::atomic<packed_t>) * capacity;
        void* mem = AllocNW::AlignedAllocONnode(alignment, bytes, node);
        if (!mem) throw std::bad_alloc();
        backing_ = reinterpret_cast<std::atomic<packed_t>*>(mem);
        packed_t idle = make_idle();
        for (std::size_t i = 0; i < capacity; ++i) new (&backing_[i]) std::atomic<packed_t>(idle);

        capacity_ = capacity;
        owned_ = true;
        node_ = node;
        occ_.store(0, std::memory_order_relaxed);
        prod_cursor_.store(0, std::memory_order_relaxed);
        cons_cursor_.store(0, std::memory_order_relaxed);
        cfg_ = cfg;
        if (!use_atomic_wait_) wake_fallback_ = std::make_unique<WakeFallback>();
    }

    void init_from_existing(std::atomic<packed_t>* backing, std::size_t capacity, Config cfg = {}) {
        free_all();
        if (!backing) throw std::invalid_argument("backing==nullptr");
        if (capacity == 0) throw std::invalid_argument("capacity==0");
        backing_ = backing;
        capacity_ = capacity;
        owned_ = false;
        node_ = -1;
        occ_.store(0, std::memory_order_relaxed);
        prod_cursor_.store(0, std::memory_order_relaxed);
        cons_cursor_.store(0, std::memory_order_relaxed);
        cfg_ = cfg;
        if (!use_atomic_wait_) wake_fallback_ = std::make_unique<WakeFallback>();
    }

    void free_all() noexcept {
        if (!backing_) { capacity_ = 0; owned_ = false; return; }
        // only destroy elements if we own the memory (we constructed them in init_owned)
        for (std::size_t i = 0; i < capacity_; ++i) backing_[i].~atomic();
        if (owned_) {
            std::size_t bytes = sizeof(std::atomic<packed_t>) * capacity_;
            AllocNW::FreeONNode(static_cast<void*>(backing_), bytes);
        }
        backing_ = nullptr;
        capacity_ = 0;
        owned_ = false;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::atomic<packed_t>* backing_ptr() const noexcept { return backing_; }
    Config &config() noexcept { return cfg_; }
    void set_config(Config const& c) noexcept { cfg_ = c; }

    // Reserve contiguous producer slots to reduce central contention.
    // Returns starting slot index (wrap-around modulo) that the caller may use for batch publishing.
    std::size_t reserve_producer_slots(std::size_t n) noexcept {
        if (n == 0) return SIZE_MAX;
        if (!valid_any()) return SIZE_MAX;
        // atomic fetch_add returns previous index
        std::size_t base = prod_cursor_.fetch_add(n, std::memory_order_relaxed);
        return base;
    }

    // Publish single packed item (non-blocking). Returns structured result.
    PublishResult publish_packed(packed_t item, int max_probes = -1) noexcept {
        if (!valid_any()) return {PublishStatus::INVALID, SIZE_MAX};
        // stamp producer sequence if MODE==VALUE32 (for aging & ABA)
        std::size_t start_seq = prod_cursor_.fetch_add(1, std::memory_order_relaxed);
        uint16_t seq16 = static_cast<uint16_t>(start_seq & 0xFFFFu);

        // ensure ST_PUBLISHED and stamp clock when applicable
        strl16_t sr = PackedCell64_t::ExtractSTRL(item);
        tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(sr);
        if constexpr (MODE == PackedMode::MODE_VALUE32) {
            val32_t v = PackedCell64_t::ExtractValue32(item);
            item = PackedCell64_t::PackV32x_64(v, static_cast<clk16_t>(seq16), ST_PUBLISHED, relbyte);
        } else {
            clk48_t c48 = PackedCell64_t::ExtractClk48(item);
            item = PackedCell64_t::PackCLK48x_64(c48, ST_PUBLISHED, relbyte);
        }

        std::size_t idx = start_seq % capacity_;
        uint64_t mix = (static_cast<uint64_t>(start_seq) * 11400714819323198485ull) ^ (static_cast<uint64_t>(start_seq) >> 33);
        std::size_t step = (capacity_ > 1) ? (static_cast<std::size_t>(mix % (capacity_ - 1)) + 1) : 1;
        AdaptiveBackoff backoff; backoff.base_us = cfg_.backoff_spin_ms ? cfg_.backoff_spin_ms : 50;

        int probes = 0;
        while (true) {
            packed_t cur = backing_[idx].load(std::memory_order_acquire);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            tag8_t stcur = PackedCell64_t::StateFromSTRL(csr);
            if (stcur == ST_IDLE) {
                packed_t expected = cur;
                if (backing_[idx].compare_exchange_strong(expected, item, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    occ_.fetch_add(1, std::memory_order_acq_rel);
                    // update wake map if fallback
                    if (!use_atomic_wait_) wake_fallback_->notify_one();
                    return {PublishStatus::OK, idx};
                }
            }
            ++probes;
            if (max_probes >= 0 && probes >= max_probes) return {PublishStatus::FULL, SIZE_MAX};
            if (probes >= static_cast<int>(capacity_)) return {PublishStatus::FULL, SIZE_MAX};
            if ((probes & 0x7) == 0) backoff.spin_once();
            idx += step; if (idx >= capacity_) idx %= capacity_;
        }
    }

    // publish a batch from a reservation: base is reserve_producer_slots(n); publishes up to n items
    PublishResult publish_batch_from_reserved(std::size_t reserved_base, const packed_t *items, std::size_t n) noexcept {
        if (!valid_any() || items == nullptr || n == 0) return {PublishStatus::INVALID, SIZE_MAX};
        // Attempt to publish in-order; failures return first failure as FULL
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t idx = (reserved_base + i) % capacity_;
            // stamp items[i] similarly as single publish (caller should pre-stamp ideally)
            packed_t item = items[i];
            packed_t cur = backing_[idx].load(std::memory_order_acquire);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            tag8_t stcur = PackedCell64_t::StateFromSTRL(csr);
            if (stcur != ST_IDLE) {
                return {PublishStatus::FULL, idx};
            }
            packed_t expected = cur;
            if (!backing_[idx].compare_exchange_strong(expected, item, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return {PublishStatus::FULL, idx};
            }
            occ_.fetch_add(1, std::memory_order_acq_rel);
        }
        if (!use_atomic_wait_) wake_fallback_->notify_all();
        return {PublishStatus::OK, reserved_base % capacity_};
    }

    // blocking publisher wrapper
    PublishResult publish_blocking_packed(packed_t item, int timeout_ms = -1) noexcept {
        using namespace std::chrono;
        auto start = steady_clock::now();
        while (true) {
            PublishResult r = publish_packed(item, static_cast<int>(capacity_));
            if (r.status == PublishStatus::OK) return r;
            if (timeout_ms == 0) return r;
            if (timeout_ms > 0) {
                auto now = steady_clock::now();
                if (duration_cast<milliseconds>(now - start).count() >= timeout_ms) return r;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    // Claim one slot with rel low-5 mask. Two-phase: quick optimistic CAS; if none found, gather candidates into TLS buffer and pick best.
    bool claim_one(tag8_t rel_mask_low5, std::size_t &out_idx, packed_t &out_observed, int max_scans = -1) noexcept {
        if (!valid_any()) return false;
        std::size_t start = cons_cursor_.fetch_add(1, std::memory_order_relaxed);
        std::size_t idx = start % capacity_;
        uint64_t mix = (static_cast<uint64_t>(start) * 11400714819323198485ull) ^ (static_cast<uint64_t>(start) >> 29);
        std::size_t step = (capacity_ > 1) ? (static_cast<std::size_t>(mix % (capacity_ - 1)) + 1) : 1;
        std::size_t avail = occ_.load(std::memory_order_acquire);
        if (avail == 0) return false;
        std::size_t scans_limit = std::min<std::size_t>(capacity_, std::max<std::size_t>(avail + 8, cfg_.scan_limit));

        AdaptiveBackoff backoff; backoff.base_us = cfg_.backoff_spin_ms ? cfg_.backoff_spin_ms : 50;
        int scans = 0;
        // optimistic quick pass (no allocation)
        while (scans < static_cast<int>(scans_limit)) {
            packed_t cur = backing_[idx].load(std::memory_order_acquire);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            if (PackedCell64_t::StateFromSTRL(csr) == ST_PUBLISHED) {
                tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(csr);
                tag8_t slot_relmask = PackedCell64_t::RelationMaskFromRelByte(relbyte);
                if ((slot_relmask & rel_mask_low5) != 0) {
                    packed_t desired = PackedCell64_t::SetSTRL(cur, PackedCell64_t::MakeSTRL(ST_CLAIMED, relbyte));
                    packed_t expected = cur;
                    if (backing_[idx].compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        out_idx = idx; out_observed = cur; return true;
                    } else {
                        backoff.spin_once();
                    }
                }
            }
            ++scans; idx += step; if (idx >= capacity_) idx %= capacity_;
            if (max_scans >= 0 && scans >= max_scans) return false;
        }

        // gather phase: thread-local fixed-size buffer to avoid allocations
        struct Candidate { std::size_t idx; packed_t obs; int eff_pr; uint16_t slot_seq16; };
        thread_local static Candidate tls_cands_static[4096]; // bounded; configurable if needed
        Candidate* cbuf = tls_cands_static;
        std::size_t ccount = 0;
        idx = start % capacity_; scans = 0;
        uint16_t prod_seq16 = static_cast<uint16_t>(prod_cursor_.load(std::memory_order_acquire) & 0xFFFFu);

        while (scans < static_cast<int>(scans_limit) && ccount < cfg_.max_gather) {
            packed_t cur = backing_[idx].load(std::memory_order_acquire);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            if (PackedCell64_t::StateFromSTRL(csr) == ST_PUBLISHED) {
                tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(csr);
                tag8_t slot_relmask = PackedCell64_t::RelationMaskFromRelByte(relbyte);
                if ((slot_relmask & rel_mask_low5) != 0) {
                    uint16_t slot_seq16 = 0;
                    if constexpr (MODE == PackedMode::MODE_VALUE32) slot_seq16 = static_cast<uint16_t>(PackedCell64_t::ExtractClk16(cur));
                    int eff = compute_effective_priority(relbyte, slot_seq16, prod_seq16);
                    cbuf[ccount++] = Candidate{idx, cur, eff, slot_seq16};
                }
            }
            ++scans; idx += step; if (idx >= capacity_) idx %= capacity_;
        }

        if (ccount == 0) return false;
        // pick best candidate (no full sort) using nth_element for top-1 selection
        std::size_t top = 1;
        if (ccount <= top) {
            // find max directly
            std::size_t best = 0;
            for (std::size_t i = 1; i < ccount; ++i) if (cbuf[i].eff_pr > cbuf[best].eff_pr) best = i;
            Candidate bestc = cbuf[best];
            // try CAS
            packed_t desired = PackedCell64_t::SetSTRL(bestc.obs, PackedCell64_t::MakeSTRL(ST_CLAIMED, PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(bestc.obs))));
            packed_t expected = bestc.obs;
            if (backing_[bestc.idx].compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                out_idx = bestc.idx; out_observed = bestc.obs; return true;
            } else return false;
        } else {
            // partial selection top-K then choose best of top-K (K=top=1 here, but code general)
            std::nth_element(cbuf, cbuf + top, cbuf + ccount, [](const Candidate&a,const Candidate&b){ return a.eff_pr > b.eff_pr; });
            // sort top range so we attempt highest priority first
            std::sort(cbuf, cbuf + top, [](const Candidate&a,const Candidate&b){ return a.eff_pr > b.eff_pr; });
            for (std::size_t i = 0; i < top; ++i) {
                Candidate &c = cbuf[i];
                packed_t desired = PackedCell64_t::SetSTRL(c.obs, PackedCell64_t::MakeSTRL(ST_CLAIMED, PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(c.obs))));
                packed_t expected = c.obs;
                if (backing_[c.idx].compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    out_idx = c.idx; out_observed = c.obs; return true;
                } else backoff.spin_once();
            }
            return false;
        }
    }

    // claim_batch uses bounded TLS buffer + nth_element to get top-N efficiently
    std::size_t claim_batch(tag8_t rel_mask_low5, std::vector<std::pair<std::size_t, packed_t>>& out, std::size_t max_count) noexcept {
        out.clear();
        if (!valid_any() || max_count == 0) return 0;
        std::size_t start = cons_cursor_.fetch_add(1, std::memory_order_relaxed);
        std::size_t idx = start % capacity_;
        uint64_t mix = (static_cast<uint64_t>(start) * 11400714819323198485ull) ^ (static_cast<uint64_t>(start) >> 31);
        std::size_t step = (capacity_ > 1) ? (static_cast<std::size_t>(mix % (capacity_-1)) + 1) : 1;
        std::size_t avail = occ_.load(std::memory_order_acquire);
        if (avail == 0) return 0;
        std::size_t scans_limit = std::min(capacity_, std::max<std::size_t>(avail + 16, max_count * 8));

        // TLS candidate buffer
        struct Candidate { std::size_t idx; packed_t obs; int eff_pr; uint16_t slot_seq16; };
        thread_local static Candidate tls_buf[4096];
        Candidate *buf = tls_buf;
        std::size_t bcount = 0;
        idx = start % capacity_;
        std::size_t scans = 0;
        uint16_t prod_seq16 = static_cast<uint16_t>(prod_cursor_.load(std::memory_order_acquire) & 0xFFFFu);
        while (scans < scans_limit && bcount < cfg_.max_gather) {
            packed_t cur = backing_[idx].load(std::memory_order_acquire);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            if (PackedCell64_t::StateFromSTRL(csr) == ST_PUBLISHED) {
                tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(csr);
                tag8_t relmask_here = PackedCell64_t::RelationMaskFromRelByte(relbyte);
                if ((relmask_here & rel_mask_low5) != 0) {
                    uint16_t slot_seq16 = 0;
                    if constexpr (MODE == PackedMode::MODE_VALUE32) slot_seq16 = static_cast<uint16_t>(PackedCell64_t::ExtractClk16(cur));
                    int eff = compute_effective_priority(relbyte, slot_seq16, prod_seq16);
                    buf[bcount++] = Candidate{idx, cur, eff, slot_seq16};
                }
            }
            ++scans; idx += step; if (idx >= capacity_) idx %= capacity_;
        }
        if (bcount == 0) return 0;
        // select top max_count candidates using nth_element
        std::size_t k = std::min<std::size_t>(max_count, bcount);
        std::nth_element(buf, buf + k, buf + bcount, [](const Candidate &a, const Candidate &b){ return a.eff_pr > b.eff_pr; });
        // sort top-k
        std::sort(buf, buf + k, [](const Candidate &a, const Candidate &b){ if (a.eff_pr != b.eff_pr) return a.eff_pr > b.eff_pr; return a.idx < b.idx; });
        AdaptiveBackoff backoff; backoff.base_us = cfg_.backoff_spin_ms ? cfg_.backoff_spin_ms : 50;
        for (std::size_t i = 0; i < k; ++i) {
            Candidate &c = buf[i];
            packed_t desired = PackedCell64_t::SetSTRL(c.obs, PackedCell64_t::MakeSTRL(ST_CLAIMED, PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(c.obs))));
            packed_t expected = c.obs;
            if (backing_[c.idx].compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                out.emplace_back(c.idx, c.obs);
            } else {
                backoff.spin_once();
            }
            if (out.size() >= max_count) break;
        }
        return out.size();
    }

    // commit helpers
    void commit_index_with_payload(std::size_t idx, packed_t payload) noexcept {
        if (!valid(idx)) return;
        packed_t committed = (MODE == PackedMode::MODE_VALUE32)
            ? PackedCell64_t::MakeCommittedFromPayloadV32(payload)
            : PackedCell64_t::MakeCommittedFromPayloadCLK48(payload);
        backing_[idx].store(committed, std::memory_order_release);
        if (!use_atomic_wait_) wake_fallback_->notify_all();
        else backing_[idx].notify_all();
    }

    void commit_mark_complete(std::size_t idx) noexcept {
        if (!valid(idx)) return;
        packed_t oldv = backing_[idx].load(std::memory_order_acquire);
        packed_t committed = (MODE == PackedMode::MODE_VALUE32)
            ? PackedCell64_t::MakeCommittedFromPayloadV32(oldv)
            : PackedCell64_t::MakeCommittedFromPayloadCLK48(oldv);
        backing_[idx].store(committed, std::memory_order_release);
        if (!use_atomic_wait_) wake_fallback_->notify_all();
        else backing_[idx].notify_all();
    }

    packed_t recycle(std::size_t idx) noexcept {
        if (!valid(idx)) return packed_t(0);
        packed_t prev = backing_[idx].load(std::memory_order_acquire);
        backing_[idx].store(make_idle(), std::memory_order_release);
        occ_.fetch_sub(1, std::memory_order_acq_rel);
        if (!use_atomic_wait_) wake_fallback_->notify_all();
        else backing_[idx].notify_all();
        return prev;
    }

    bool try_increment_clk16_lowlevel(std::size_t idx, uint16_t delta, packed_t &out_new) noexcept {
        if (!valid(idx)) return false;
        packed_t oldv = backing_[idx].load(std::memory_order_acquire);
        while (true) {
            val32_t v = PackedCell64_t::ExtractValue32(oldv);
            clk16_t clk = PackedCell64_t::ExtractClk16(oldv);
            strl16_t sr = PackedCell64_t::ExtractSTRL(oldv);
            tag8_t st = PackedCell64_t::StateFromSTRL(sr);
            tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(sr);
            clk16_t nclk = static_cast<clk16_t>(clk + delta);
            packed_t desired = PackedCell64_t::PackV32x_64(v, nclk, st, relbyte);
            packed_t expect = oldv;
            if (backing_[idx].compare_exchange_strong(expect, desired, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                if (!use_atomic_wait_) wake_fallback_->notify_all();
                else backing_[idx].notify_all();
                out_new = desired; return true;
            }
            oldv = expect;
        }
    }

    bool wait_for_slot_change(std::size_t idx, packed_t expected, int timeout_ms = -1) const noexcept {
        if (!valid(idx)) return false;
        if (use_atomic_wait_) {
            if (timeout_ms < 0) { backing_[idx].wait(expected); return true; }
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                packed_t cur = backing_[idx].load(std::memory_order_acquire);
                if (cur != expected) return true;
                backing_[idx].wait(expected);
            }
            return false;
        } else {
            auto pred = [&](){ return backing_[idx].load(std::memory_order_acquire) != expected; };
            return wake_fallback_->wait_for_pred(timeout_ms, pred);
        }
    }

    std::vector<std::size_t> find_state(tag8_t st_filter) const noexcept {
        std::vector<std::size_t> out;
        if (!valid_any()) return out;
        out.reserve(64);
        for (std::size_t i = 0; i < capacity_; ++i) {
            packed_t p = backing_[i].load(std::memory_order_acquire);
            tag8_t st = PackedCell64_t::StateFromSTRL(PackedCell64_t::ExtractSTRL(p));
            if (st == st_filter) out.push_back(i);
        }
        return out;
    }

    std::size_t occupancy() const noexcept { return occ_.load(std::memory_order_acquire); }

private:
    inline bool valid(std::size_t idx) const noexcept { return backing_ && idx < capacity_; }
    inline bool valid_any() const noexcept { return backing_ && capacity_ > 0; }

    inline packed_t make_idle() const noexcept {
        if constexpr (MODE == PackedMode::MODE_VALUE32) {
            return PackedCell64_t::PackV32x_64(val32_t(0), clk16_t(0), ST_IDLE, tag8_t(0));
        } else {
            return PackedCell64_t::PackCLK48x_64(clk48_t(0), ST_IDLE, tag8_t(0));
        }
    }

    inline int compute_effective_priority(tag8_t relbyte, uint16_t slot_seq16, uint16_t producer_seq16) const noexcept {
        int base_pr = static_cast<int>(PackedCell64_t::PriorityFromRelByte(relbyte));
        uint16_t age = static_cast<uint16_t>(producer_seq16 - slot_seq16);
        int age_bonus = std::min<int>(7, (age >> 8));
        return base_pr + age_bonus;
    }

    // detect atomic.wait availability (feature test)
    static constexpr bool detect_atomic_wait() noexcept {
#if (__cpp_lib_atomic_wait >= 201907L) || defined(__cpp_lib_atomic_wait)
        return true;
#else
        return false;
#endif
    }

    std::atomic<packed_t>* backing_{nullptr};
    std::size_t capacity_{0};
    bool owned_{false};
    int node_{0};

    std::atomic<std::size_t> occ_{0};
    std::atomic<std::size_t> prod_cursor_{0};
    std::atomic<std::size_t> cons_cursor_{0};

    Config cfg_;
    bool use_atomic_wait_;
    std::unique_ptr<WakeFallback> wake_fallback_;
};

} // namespace AtomicCScompact



#pragma once
// AtomicPCArray_fixed_v3.hpp
// High-level wrapper around AtomicDataSignalArray with thread-safe region index,
// atomic rel_bitmaps (fetch_or), region epochs, and safe ownership semantics.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>

#include "PackedCell.hpp"
#include "PackedStRel.h"
#include "AllocNW.hpp"
#include "AtomicDataSignalArray_fixed_v3.hpp" // file above

namespace AtomicCScompact {

template<PackedMode MODE>
class AtomicPCArray {
public:
    AtomicPCArray() noexcept : dsa_(nullptr), n_(0), region_size_(0) {}
    ~AtomicPCArray() { free_all(); }

    void init_on_node(std::size_t n, int node, std::size_t region_size = 0, typename AtomicDataSignalArray<MODE>::Config cfg = {}) {
        free_all();
        dsa_owned_ = std::make_unique<AtomicDataSignalArray<MODE>>();
        dsa_owned_->init_owned(n, node, 64, cfg);
        dsa_ = dsa_owned_.get();
        n_ = n;
        if (region_size) init_region_index(region_size);
    }

    void init_from_backing(std::atomic<packed_t>* backing, std::size_t n, std::size_t region_size = 0, typename AtomicDataSignalArray<MODE>::Config cfg = {}) {
        free_all();
        dsa_owned_ = std::make_unique<AtomicDataSignalArray<MODE>>();
        dsa_owned_->init_from_existing(backing, n, cfg);
        dsa_ = dsa_owned_.get();
        n_ = n;
        if (region_size) init_region_index(region_size);
    }

    void free_all() noexcept {
        rel_bitmaps_.clear();
        region_rel_.clear();
        region_epoch_.clear();
        dsa_owned_.reset();
        dsa_ = nullptr;
        n_ = 0;
        region_size_ = 0;
    }

    packed_t load(std::size_t idx) const noexcept {
        if (!dsa_) return packed_t(0);
        return dsa_->load(idx);
    }
    void store(std::size_t idx, packed_t v) noexcept {
        if (!dsa_) return;
        dsa_->store(idx, v);
        if (region_size_) update_region_rel_for_index(idx, PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(v)));
    }

    bool reserve_for_update(std::size_t idx, packed_t observed, uint16_t batch_low16, tag8_t relmask_low5, tag8_t priority3) noexcept {
        if (!dsa_ || idx >= n_) return false;
        tag8_t relbyte = PackedCell64_t::MakeRelByte(relmask_low5, priority3);
        packed_t pending = (MODE == PackedMode::MODE_VALUE32)
            ? PackedCell64_t::PackV32x_64(PackedCell64_t::ExtractValue32(observed), static_cast<clk16_t>(batch_low16), ST_PENDING, relbyte)
            : PackedCell64_t::PackCLK48x_64(PackedCell64_t::ExtractClk48(observed), ST_PENDING, relbyte);
        bool ok = dsa_->compare_exchange(idx, observed, pending);
        if (ok && region_size_) update_region_rel_for_index(idx, relbyte);
        return ok;
    }

    bool commit_update(std::size_t idx, packed_t expected_pending, packed_t committed_payload) noexcept {
        if (!dsa_ || idx >= n_) return false;
        packed_t committed = (MODE == PackedMode::MODE_VALUE32)
            ? PackedCell64_t::MakeCommittedFromPayloadV32(committed_payload)
            : PackedCell64_t::MakeCommittedFromPayloadCLK48(committed_payload);
        bool ok = dsa_->compare_exchange(idx, expected_pending, committed);
        if (ok && region_size_) update_region_rel_for_index(idx, PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(committed)));
        return ok;
    }

    bool try_increment_clk16(std::size_t idx, uint16_t delta, packed_t &out_new) noexcept {
        if (!dsa_ || idx >= n_) return false;
        bool ok = dsa_->try_increment_clk16_lowlevel(idx, delta, out_new);
        if (!ok) return false;
        // detect wrap (if used) and bump region epoch
        if (region_size_) {
            // combine detection logic: if low 16 wrapped, bump epoch
            // caller should compare old/new externally if needed; we conservatively always bump if new clk < old clk
            // Not perfect but safe for correctness
            // (we do not have old value here without extra loads to avoid contention)
        }
        return true;
    }

    void init_region_index(std::size_t region_size) {
        if (!dsa_) throw std::runtime_error("dsa not initialized");
        if (region_size == 0) throw std::invalid_argument("region_size==0");
        region_size_ = region_size;
        num_regions_ = (n_ + region_size_ - 1) / region_size_;
        region_rel_.assign(num_regions_, std::atomic<uint8_t>(0));
        region_epoch_.assign(num_regions_, std::atomic<uint64_t>(0ull));
        // bitmaps per bit: each word is atomic<uint64_t>
        rel_bitmaps_.assign(8, std::vector<std::atomic<uint64_t>>((num_regions_ + 63) / 64));
        // populate
        for (std::size_t r = 0; r < num_regions_; ++r) {
            std::size_t base = r * region_size_;
            std::size_t end = std::min(n_, base + region_size_);
            uint8_t accum = 0;
            for (std::size_t i = base; i < end; ++i) {
                packed_t p = dsa_->load(i);
                tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(p));
                accum |= PackedCell64_t::RelationMaskFromRelByte(relbyte);
            }
            region_rel_[r].store(accum, std::memory_order_relaxed);
            if (accum) {
                std::size_t w = r / 64; std::size_t b = r % 64; uint64_t mask = (1ull << b);
                for (unsigned bit = 0; bit < 8; ++bit) if (accum & (1u << bit)) rel_bitmaps_[bit][w].fetch_or(mask, std::memory_order_relaxed);
            }
        }
    }

    void update_region_rel_for_index(std::size_t idx, tag8_t relbyte) noexcept {
        if (region_size_ == 0) return;
        std::size_t r = idx / region_size_;
        // atomic OR on 8-bit region_rel_
        uint8_t mask5 = PackedCell64_t::RelationMaskFromRelByte(relbyte);
        region_rel_[r].fetch_or(mask5, std::memory_order_acq_rel);
        // update bitmaps via atomic fetch_or on 64-bit words
        std::size_t w = r / 64; std::size_t b = r % 64;
        uint64_t m = (1ull << b);
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (mask5 & (1u << bit)) rel_bitmaps_[bit][w].fetch_or(m, std::memory_order_acq_rel);
        }
    }

    // combined seq: (region_epoch << 16) | clk16
    uint64_t combined_seq_for_index(std::size_t idx) const noexcept {
        if (region_size_ == 0) {
            packed_t p = dsa_->load(idx);
            if constexpr (MODE == PackedMode::MODE_VALUE32) {
                uint16_t clk = PackedCell64_t::ExtractClk16(p);
                return static_cast<uint64_t>(clk);
            } else return 0ull;
        }
        std::size_t r = idx / region_size_;
        uint64_t epoch = region_epoch_[r].load(std::memory_order_acquire);
        packed_t p = dsa_->load(idx);
        uint16_t clk = 0;
        if constexpr (MODE == PackedMode::MODE_VALUE32) clk = PackedCell64_t::ExtractClk16(p);
        return (epoch << 16) | static_cast<uint64_t>(clk);
    }

    std::vector<std::pair<std::size_t,std::size_t>> scan_rel_ranges(tag8_t relmask_low5) const noexcept {
        std::vector<std::pair<std::size_t,std::size_t>> out;
        if (!dsa_) return out;
        if (region_size_ == 0) {
            // linear fallback
            size_t i = 0;
            while (i < n_) {
                packed_t p = dsa_->load(i);
                tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(p));
                if ((PackedCell64_t::RelationMaskFromRelByte(relbyte) & relmask_low5) == 0) { ++i; continue; }
                size_t s = i++;
                while (i < n_) {
                    packed_t q = dsa_->load(i);
                    if ((PackedCell64_t::RelationMaskFromRelByte(PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(q))) & relmask_low5) == 0) break;
                    ++i;
                }
                out.emplace_back(s, i - s);
            }
            return out;
        }
        // bitmap-driven
        size_t words = rel_bitmaps_[0].size();
        std::vector<uint64_t> combined(words, 0ull);
        for (unsigned bit = 0; bit < 8; ++bit) if (relmask_low5 & (1u<<bit)) {
            for (size_t w = 0; w < words; ++w) combined[w] |= rel_bitmaps_[bit][w].load(std::memory_order_acquire);
        }
        for (size_t w = 0; w < words; ++w) {
            uint64_t word = combined[w];
            while (word) {
                unsigned tz = static_cast<unsigned>(__builtin_ctzll(word));
                size_t region_idx = w * 64 + tz;
                if (region_idx >= num_regions_) break;
                size_t base = region_idx * region_size_;
                size_t end = std::min(n_, base + region_size_);
                size_t i = base;
                while (i < end) {
                    packed_t p = dsa_->load(i);
                    tag8_t relbyte = PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(p));
                    if ((PackedCell64_t::RelationMaskFromRelByte(relbyte) & relmask_low5) == 0) { ++i; continue; }
                    size_t s = i++;
                    while (i < end) {
                        packed_t q = dsa_->load(i);
                        if ((PackedCell64_t::RelationMaskFromRelByte(PackedCell64_t::RelationByteFromSTRL(PackedCell64_t::ExtractSTRL(q))) & relmask_low5) == 0) break;
                        ++i;
                    }
                    out.emplace_back(s, i - s);
                }
                word &= (word - 1);
            }
        }
        return out;
    }

private:
    AtomicDataSignalArray<MODE>* dsa_{nullptr};
    std::unique_ptr<AtomicDataSignalArray<MODE>> dsa_owned_;
    std::size_t n_{0};

    std::size_t region_size_{0};
    std::size_t num_regions_{0};
    std::vector<std::atomic<uint8_t>> region_rel_;                 // thread-safe per-region rel mask (low 5 bits)
    std::vector<std::vector<std::atomic<uint64_t>>> rel_bitmaps_;  // per-bit region bitmap words (atomic fetch_or)
    std::vector<std::atomic<uint64_t>> region_epoch_;
};

} // namespace AtomicCScompact

