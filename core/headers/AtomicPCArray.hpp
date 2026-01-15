#pragma once
// AtomicPCArray.hpp
// Single-array-of-64bit-atomics. Exposes auto pack/unpack helpers and
// a page/region relation index to look up ranges quickly by relation bitmask.

#include <atomic>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <cstring>
#include <new>

#include "PackedCell.hpp"
#include "PackedStRel.h"
#include "AllocNW.hpp"

namespace AtomicCScompact {

template<PackedMode MODE, typename UserValueT = uint32_t>
class AtomicPCArray {
public:
    using packed_t = packed64_t;
    using value32_t = val32_t;
    using clk16_t_t = clk16_t;
    using rel8_t = tag8_t;
    using strl16_t_t = strl16_t;
    using user_value_t = UserValueT;

    AtomicPCArray() noexcept = default;
    ~AtomicPCArray() { free_all(); }

    AtomicPCArray(const AtomicPCArray&) = delete;
    AtomicPCArray& operator=(const AtomicPCArray&) = delete;

    // --- Value conversion helpers: allows user to pick float/int/uint32_t as logical value.
    struct ValueConverter {
        static_assert(std::is_trivially_copyable_v<user_value_t>, "UserValueT must be trivially copyable");
        static inline value32_t to_storage(user_value_t v) noexcept {
            if constexpr (std::is_same_v<user_value_t, float>) {
                value32_t x = 0;
                static_assert(sizeof(x) == sizeof(v), "float must be 32-bit");
                std::memcpy(&x, &v, sizeof(x));
                return x;
            } else {
                value32_t x = 0;
                if constexpr (sizeof(user_value_t) <= sizeof(value32_t)) {
                    std::memcpy(&x, &v, sizeof(user_value_t));
                    if constexpr (sizeof(user_value_t) < sizeof(value32_t)) {
                        // remaining bytes already zeroed by initialization
                    }
                    return x;
                } else {
                    // larger types: truncate (documented)
                    return static_cast<value32_t>(v);
                }
            }
        }
        static inline user_value_t from_storage(value32_t s) noexcept {
            if constexpr (std::is_same_v<user_value_t, float>) {
                user_value_t out;
                std::memcpy(&out, &s, sizeof(s));
                return out;
            } else {
                user_value_t out{};
                std::memcpy(&out, &s, std::min(sizeof(out), sizeof(s)));
                return out;
            }
        }
    };

    // init: allocate owned memory on given NUMA node (or plain aligned alloc)
    void init_on_node(size_t n, int node = 0) {
        free_all();
        if (n == 0) throw std::invalid_argument("n==0");
        n_ = n;
        owned_bytes_ = sizeof(std::atomic<packed_t>) * n_;
        void* p = AllocNW::AlignedAllocONnode(64u, owned_bytes_, node);
        if (!p) throw std::bad_alloc();
        meta_ = reinterpret_cast<std::atomic<packed_t>*>(p);

        // ensure atomic<packed_t> is lock-free preferable but still allow if not
        std::atomic<packed_t> test{0};
        if (!test.is_lock_free()) {
            // If the platform doesn't provide lock-free atomics, we still proceed but
            // the user should be aware of potential performance/semantic differences.
        }

        // placement-new initialize each atomic with idle value
        packed_t idle = make_idle();
        for (size_t i = 0; i < n_; ++i) new (&meta_[i]) std::atomic<packed_t>(idle);
        owned_ = true;
        node_ = node;
    }

    // attach to externally provided backing store (no ownership)
    void init_from_existing(std::atomic<packed_t>* backing, size_t n) {
        free_all();
        if (!backing) throw std::invalid_argument("backing==nullptr");
        n_ = n;
        meta_ = backing;
        owned_ = false;
        owned_bytes_ = 0;
    }

    void free_all() noexcept {
        if (meta_) {
            if (owned_) {
                for (size_t i = 0; i < n_; ++i) {
                    // destroy atomics explicitly (they were placement-new'd)
                    meta_[i].~atomic<packed_t>();
                }
                AllocNW::FreeONNode(static_cast<void*>(meta_), owned_bytes_);
            }
            meta_ = nullptr;
        }
        n_ = 0;
        owned_bytes_ = 0;
        owned_ = false;
        region_size_ = 0;
        num_regions_ = 0;
        region_rel_.clear();
    }

    size_t size() const noexcept { return n_; }

    // --- low-level atomic accessors
    packed_t load(size_t idx, std::memory_order mo = std::memory_order_acquire) const noexcept {
        if (idx >= n_ || !meta_) return packed_t(0);
        return meta_[idx].load(mo);
    }
    void store(size_t idx, packed_t v, std::memory_order mo = std::memory_order_release) noexcept {
        if (idx >= n_ || !meta_) return;
        meta_[idx].store(v, mo);
        std::atomic_notify_all(&meta_[idx]);
    }
    bool compare_exchange(size_t idx, packed_t &expected, packed_t desired,
                          std::memory_order success = std::memory_order_acq_rel,
                          std::memory_order failure = std::memory_order_relaxed) noexcept {
        if (idx >= n_ || !meta_) return false;
        return meta_[idx].compare_exchange_strong(expected, desired, success, failure);
    }

    // --- value setters / readers (MODE_VALUE32 only for explicit value writes)
    void set_value(size_t idx, user_value_t user_v, clk16_t_t clk, rel8_t rel) noexcept {
        static_assert(MODE == PackedMode::MODE_VALUE32, "set_value is only valid for MODE_VALUE32");
        if (idx >= n_ || !meta_) return;
        value32_t vbits = ValueConverter::to_storage(user_v);
        packed_t p = PackedCell64_t::PackV32x_64(vbits, clk, ST_PUBLISHED, rel);
        store(idx, p, std::memory_order_release);
    }

    // generic read: returns stored user value, clk (or truncated), st, rel
    void read_value(size_t idx, user_value_t &v_out, uint64_t &clk_out, rel8_t &st_out, rel8_t &rel_out) const noexcept {
        if (idx >= n_ || !meta_) { v_out = user_value_t{}; clk_out = 0; st_out = ST_IDLE; rel_out = REL_NONE; return; }
        packed_t p = load(idx);
        strl16_t_t sr = PackedCell64_t::ExtractSTRL(p);
        st_out = PackedCell64_t::StateFromSTRL(sr);
        rel_out = PackedCell64_t::RelationFromSTRL(sr);
        if constexpr (MODE == PackedMode::MODE_VALUE32) {
            value32_t vb = PackedCell64_t::ExtractValue32(p);
            clk16_t_t clk16 = PackedCell64_t::ExtractClk16(p);
            clk_out = static_cast<uint64_t>(clk16);
            v_out = ValueConverter::from_storage(vb);
        } else { // MODE_CLKVAL48
            uint64_t clk48 = PackedCell64_t::ExtractClk48(p);
            clk_out = clk48;
            v_out = user_value_t{};
        }
    }

    // --- CAS-based reserve (to mark a slot pending based on observed expected)
    // expected is the currently-observed packed value (will be copied into the CAS expected)
    bool reserve_for_update(size_t idx, packed_t observed_expected, clk16_t_t new_clk_low, rel8_t rel_hint) noexcept {
        if (idx >= n_ || !meta_) return false;
        packed_t pending = 0;
        if constexpr (MODE == PackedMode::MODE_VALUE32) {
            value32_t v = PackedCell64_t::ExtractValue32(observed_expected);
            pending = PackedCell64_t::PackV32x_64(v, new_clk_low, ST_PENDING, rel_hint);
        } else {
            // MODE_CLKVAL48: we only have a 48-bit clock field; for the reserve path we set ST_PENDING
            // and write a clk low portion (PackCLK48x_64 takes a 16-bit clk field in this API).
            pending = PackedCell64_t::PackCLK48x_64(new_clk_low, ST_PENDING, rel_hint);
        }
        packed_t exp = observed_expected;
        return compare_exchange(idx, exp, pending);
    }

    // commit: replace an expected_pending value with committed value
    bool commit_update(size_t idx, packed_t expected_pending, packed_t committed) noexcept {
        if (idx >= n_ || !meta_) return false;
        bool ok = compare_exchange(idx, expected_pending, committed);
        if (ok) std::atomic_notify_all(&meta_[idx]);
        return ok;
    }

    // set a slot to idle
    void set_idle(size_t idx) noexcept {
        if (idx >= n_ || !meta_) return;
        store(idx, make_idle());
    }

    // --- region index operations
    void init_region_index(size_t region_size) {
        if (region_size == 0) throw std::invalid_argument("region_size==0");
        if (n_ == 0) throw std::runtime_error("init_region_index: array not initialized");
        region_size_ = region_size;
        num_regions_ = (n_ + region_size_ - 1) / region_size_;
        region_rel_.assign(num_regions_, static_cast<rel8_t>(0));
        // populate initial region_rel_
        for (size_t r = 0; r < num_regions_; ++r) {
            size_t base = r * region_size_;
            size_t end = std::min(n_, base + region_size_);
            rel8_t accum = REL_NONE;
            for (size_t i = base; i < end; ++i) {
                packed_t p = load(i);
                strl16_t_t sr = PackedCell64_t::ExtractSTRL(p);
                rel8_t slot_rel = PackedCell64_t::RelationFromSTRL(sr);
                accum = static_cast<rel8_t>(accum | slot_rel);
            }
            region_rel_[r] = accum;
        }
    }

    void update_rel_hint(size_t idx, rel8_t rel) noexcept {
        if (idx >= n_ || !meta_) return;
        packed_t p = load(idx);
        strl16_t_t cur_sr = PackedCell64_t::ExtractSTRL(p);
        strl16_t_t new_sr = static_cast<strl16_t_t>((static_cast<unsigned>(cur_sr & 0xFF00u)) | static_cast<unsigned>(rel));
        packed_t newp = PackedCell64_t::SetSTRL(p, new_sr);
        store(idx, newp);
        if (region_size_) {
            size_t r = idx / region_size_;
            region_rel_[r] = static_cast<rel8_t>(region_rel_[r] | rel);
        }
    }

    // scan ranges where slot relation matches rel_mask. returns vector of (start, length).
    std::vector<std::pair<size_t,size_t>> scan_rel_ranges(rel8_t rel_mask) const noexcept {
        std::vector<std::pair<size_t,size_t>> out;
        if (n_ == 0 || !meta_) return out;
        if (region_size_ == 0) {
            // full linear scan
            size_t i = 0;
            while (i < n_) {
                packed_t p = load(i);
                rel8_t r = PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(p));
                if (!RelationMatches(r, rel_mask)) { ++i; continue; }
                size_t s = i++;
                while (i < n_) {
                    packed_t q = load(i);
                    rel8_t rr = PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(q));
                    if (!RelationMatches(rr, rel_mask)) break;
                    ++i;
                }
                out.emplace_back(s, i - s);
            }
            return out;
        }
        // region-accelerated
        for (size_t r = 0; r < num_regions_; ++r) {
            rel8_t rr = region_rel_[r];
            if ((rr & rel_mask) == 0) continue; // region has no matching bits
            size_t base = r * region_size_;
            size_t end  = std::min(n_, base + region_size_);
            size_t i = base;
            while (i < end) {
                packed_t p = load(i);
                rel8_t rl = PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(p));
                if (!RelationMatches(rl, rel_mask)) { ++i; continue; }
                size_t s = i++;
                while (i < end) {
                    packed_t q = load(i);
                    rel8_t rr2 = PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(q));
                    if (!RelationMatches(rr2, rel_mask)) break;
                    ++i;
                }
                out.emplace_back(s, i - s);
            }
        }
        return out;
    }

    // convenience: read discrete fields
    void get_fields(size_t idx, user_value_t &user_value_out, uint64_t &clk48_out, rel8_t &st_out, rel8_t &rel_out) const noexcept {
        if (idx >= n_ || !meta_) { user_value_out = user_value_t{}; clk48_out = 0; st_out = ST_IDLE; rel_out = REL_NONE; return; }
        packed_t p = load(idx);
        strl16_t_t sr = PackedCell64_t::ExtractSTRL(p);
        st_out = PackedCell64_t::StateFromSTRL(sr);
        rel_out = PackedCell64_t::RelationFromSTRL(sr);
        if constexpr (MODE == PackedMode::MODE_VALUE32) {
            value32_t vb = PackedCell64_t::ExtractValue32(p);
            clk16_t_t clk16 = PackedCell64_t::ExtractClk16(p);
            clk48_out = static_cast<uint64_t>(clk16);
            user_value_out = ValueConverter::from_storage(vb);
        } else {
            uint64_t clk48 = PackedCell64_t::ExtractClk48(p);
            clk48_out = clk48;
            user_value_out = user_value_t{};
        }
    }

private:
    inline packed_t make_idle() const noexcept {
        if constexpr (MODE == PackedMode::MODE_VALUE32) {
            return PackedCell64_t::PackV32x_64(value32_t(0), clk16_t_t(0), ST_IDLE, rel8_t(0));
        } else {
            return PackedCell64_t::PackCLK48x_64(clk16_t_t(0), ST_IDLE, rel8_t(0));
        }
    }

    size_t n_{0};
    std::atomic<packed_t>* meta_{nullptr};
    size_t owned_bytes_{0};
    bool owned_{false};

    // region index
    size_t region_size_{0};
    size_t num_regions_{0};
    std::vector<rel8_t> region_rel_;

    // memory node
    int node_{0};
};

} // namespace AtomicCScompact
