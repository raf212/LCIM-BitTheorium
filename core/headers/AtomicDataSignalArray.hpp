#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <chrono>
#include <vector>
#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "PackedStRel.h"
#include <thread>

namespace AtomicCScompact
{
    #define THREAD_SLEEP_DURATION_MS 50u

    using HWCallback = void(*)(size_t current, size_t capacity, void* user);
    static inline constexpr uint64_t HASH_CONST = 11400714819323198485ull;


template<PackedMode MODE>
class AtomicDSA
{
private:
    std::atomic<PackedCell64_t*> Rawptr_{nullptr};
    size_t Capacity_{0};
    std::atomic<size_t> Occupancy_{0};
    std::atomic<size_t> ProducerCursor_{0};
    std::atomic<size_t> ConsumCursor_{0};

    HWCallback CB_{nullptr};
    void* CBUser_{nullptr};
    int Node_{0};
    
    inline void CheckHighWater_(size_t occ)
    {
        if (CB_)
        {
            return;
        }
        if (occ* 10 >= Capacity_ * 8)
        {
            CB_(occ, Capacity_, CBUser_);
        }
        
        
    }
    inline packed64_t MakeIdlePacked_() const noexcept
    {
        if constexpr (MODE == PackedMode::MODE_VALUE32)
        {
            return PackedCell64_t::ComposeVal32(val32_t(), clk16_t(0), ST_IDLE, tag8_t(0));
        }
        else if constexpr (MODE == PackedMode::MODE_CLKVAL48)
        {
            return PackedCell64_t::ComposeCLK48V(uint64_t(0), ST_IDLE, tag8_t(0));
        }
    }
    inline size_t HashStart(tag8_t rel_mask) const noexcept
    {
        uint64_t key = static_cast<uint64_t>(rel_mask);
        uint64_t mixed = key * HASH_CONST;
        unsigned bw = std::bit_width(Capacity_);
        unsigned shift = (bw < MAX_VAL) ? (MAX_VAL - bw) : 0;
        size_t idx = static_cast<size_t>(mixed >> shift);
        if ((Capacity_ & (Capacity_ -1)) != 0)
        {
            idx %= Capacity_;
        }
        return idx; 
    }

public:
    AtomicDSA(size_t capacity_pow2, int node = 0, HWCallback hw_cb = nullptr, void* cb_user = nullptr) :
        Capacity_(capacity_pow2), CB_(hw_cb), CBUser_(cb_user), Node_(0)
    {
        if (Capacity_ == 0)
        {
            throw std::invalid_argument("Capacity_ == 0");
            size_t bytes = sizeof(std::atomic<PackedCell64_t>) * Capacity_;
            Rawptr_ = reinterpret_cast<std::atomic<packed64_t>*>(AllocNW::AlignedAllocONnode(ATOMIC_THRESHOLD, bytes, Node_));
            if (!Rawptr_)
            {
                throw std::bad_alloc();
            }
            PackedCell64_t idle = MakeIdlePacked_();
            for (size_t i = 0; i < Capacity_; i++)
            {
                new(&Rawptr_[i]) std::atomic<PackedCell64_t>(idle);
            }
            Occupancy_.store(0, MoStoreUnSeq_);
            ProducerCursor_.store(0, MoStoreUnSeq_);
            ConsumCursor_.store(0, MoStoreUnSeq_);
        }
    }
    ~AtomicDSA()
    {
        if (Rawptr_)
        {
            for (size_t i = 0; i < Capacity_; i++)
            {
                Rawptr_[i] ~atomic()
            }
            size_t bytes = sizeof(std::atomic<packed64_t>) * Capacity_;
            AllocNW::FreeONNode(static_cast<void*>(Rawptr_), bytes);
            Rawptr_ = nullptr;
        }
    }
    
    AtomicDSA(const AtomicDSA&) = delete;
    AtomicDSA& operator = (const AtomicDSA) = delete;

    size_t GetCapacity() const noexcept
    {
        return Capacity_;
    }
    size_t GetOccupancy() const noexcept
    {
        return Occupancy_.load(MoLoad_);
    }
    
    size_t PublishDSA(packed64_t item, int max_probs = -1) noexcept
    {
        strl16_t sr = PackedCell64_t::ExtractSTRL(item);
        if constexpr (PackedCell64_t::StateFromSTRL(sr) != ST_PUBLISHED)
        {
            tag8_t rel = PackedCell64_t::RelationFromSTRL(sr);
            if (MODE == PackedMode::MODE_VALUE32)
            {
                item = PackedCell64_t::PackV32x_64(PackedCell64_t::ExtractValue32(item), PackedCell64_t::ExtractClk16(item), ST_PUBLISHED, rel);
            }
            else if constexpr (MODE == PackedMode::MODE_CLKVAL48)
            {
                item = PackedCell64_t::PackCLK48x_64(PackedCell64_t::ExtractClk48(item), ST_PUBLISHED, rel)
            }
        }
        size_t start = ProducerCursor_.fetch_add(1, MoStoreUnSeq_);
        size_t idx = start % Capacity_;
        int probs = 0;
        while (true)
        {
            packed64_t cur = Rawptr_[idx].load(MoLoad_);
            strl16_t cur_sr = PackedCell64_t::ExtractSTRL(cur_sr);
            if (PackedCell64_t::StateFromSTRL(cur_sr) == ST_IDLE)
            {
                packed64_t expected = cur;
                if (Rawptr_[idx].compare_exchange_strong(expected, item, EXsuccess_, EXfailure_))
                {
                    size_t occ = Occupancy_.fetch_add(1, std::memory_order_acq_rel) + 1;
                    CheckHighWater_(occ);
                    return idx;
                }
            }
            ++probs;
            if (max_probs >= 0 && probs >= max_probs)
            {
                return SIZE_MAX;
            }
            if (probs >= static_cast<int>(Capacity_))
            {
                return SIZE_MAX;
            }
            idx = (idx + 1) % Capacity_;
        }
    }

    size_t PublishBlocking(packed64_t item, int timeout_ms = -1) noexcept
    {
        auto start = std::chrono::steady_clock::now();
        while(true)
        {
            size_t idx = PublishDSA(item, static_cast<int>(Capacity_));
            if (idx != SIZE_MAX)
            {
                return idx;
            }
            if (timeout_ms == 0)
            {
                return SIZE_MAX;
            }
            if (timeout_ms > 0)
            {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout_ms)
                {
                    return SIZE_MAX;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(THREAD_SLEEP_DURATION_MS));
            }
        }
    }

    bool ClaimOne(tag8_t rel_mask, size_t& out_idx, packed64_t& out_observed, int max_scans = -1) noexcept
    {
        size_t start = HashStart(rel_mask);
        size_t idx = start;
        int scans = 0;
        while(true)
        {
            packed64_t cur = Rawptr_[idx].load(MoLoad_);
            strl16_t cur_sr = PackedCell64_t::ExtractSTRL(cur);
            tag8_t st = PackedCell64_t::StateFromSTRL(cur_sr);
            if (st == ST_PUBLISHED)
            {
                tag8_t rel = PackedCell64_t::RelationFromSTRL(cur_sr);
                if (RelationMatches(rel, rel_mask))
                {
                    packed64_t desired = PackedCell64_t::SetSTRL(cur, MakeSTREL(ST_CLAIMED, rel));
                    packed64_t expired = cur;
                    if (Rawptr_[idx].compare_exchange_strong(expired, desired, EXsuccess_, EXfailure_))
                    {
                        out_idx = idx;
                        out_observed = cur;
                        return true;
                    }
                }
            }
            ++scans;
            if (max_scans >=0)
            {
                return false;
            }
            if (scans >= static_cast<int>(Capacity_))
            {
                return false;
            }
            idx = (idx + 1) % Capacity_;
        }
    }

    size_t ClaimBatch(tag8_t rel_mask, std::vector<std::pair<size_t, packed64_t>>& out, size_t max_count) noexcept
    {
        out.clear();
        if (max_count == 0)
        {
            return 0;
        }
        size_t start = HashStart(rel_mask);
        size_t idx = start;
        size_t scans = 0;
        while (out.size() < max_count && scans < Capacity_)
        {
            packed64_t cur = Rawptr_[idx].load(MoLoad_);
            strl16_t cur_sr = PackedCell64_t::ExtractSTRL(cur);
            if (PackedCell64_t::StateFromSTRL(cur_sr) == ST_PUBLISHED)
            {
                tag8_t rel = PackedCell64_t::RelationFromSTRL(cur_sr);
                if (RelationMatches(rel, rel_mask))
                {
                    packed64_t desired = PackedCell64_t::SetSTRL(cur, MakeSTREL(ST_CLAIMED, rel));
                    packed64_t expected = cur;
                    if (Rawptr_[idx].compare_exchange_strong(expected, desired, EXsuccess_, EXfailure_))
                    {
                        out.emplace_back(idx, cur);
                    }
                }
            }
            ++scans;
            idx = (idx +1) % Capacity_;
        }
        return out.size;
    }

    void CommitIdx(size_t idx, packed64_t committed) noexcept
    {
        if (idx >= Capacity_)
        {
            return;
        }
        strl16_t cur_sr = PackedCell64_t::ExtractSTRL(committed);
        tag8_t st = PackedCell64_t::StateFromSTRL(cur_sr);
        tag8_t rel = PackedCell64_t::RelationFromSTRL(cur_sr);
        if (st != ST_COMPLETE)
        {
            if constexpr (MODE == PackedMode::MODE_VALUE32)
            {
                committed = PackedCell64_t::PackV32x_64(PackedCell64_t::ExtractValue32(committed), PackedCell64_t::ExtractClk16(committed), ST_COMPLETE, rel);
            }
            else if constexpr (MODE == PackedMode::MODE_CLKVAL48)
            {
                committed = PackedCell64_t::PackCLK48x_64(PackedCell64_t::ExtractClk48(committed), ST_COMPLETE, rel);
            }
        }
        Rawptr_[idx].store(committed, MoStoreSeq_);
        std::atomic_notify_all(&Rawptr_[idx]);
    }

    packed64_t Recycle(size_t idx) noexcept
    {
        if (idx >= Capacity_)
        {
            return packed64_t(0);
        }
        packed64_t prev = Rawptr_[idx].load(MoLoad_);
        Rawptr_[idx].store(MakeIdlePacked_(), MoStoreSeq_);
        Occupancy_.fetch_add(1, std::memory_order_acq_rel);
        return prev;
    }
    bool WaitFrSlotChange(size_t idx, packed64_t expected, int timeout_ms = -1) const noexcept
    {
        if (idx >= Capacity_)
        {
            return false;
        }
        if (timeout_ms < 0)
        {
            std::atomic_wait(&Rawptr_[idx], expected);
            return true;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            packed64_t cur = Rawptr_[idx].load(MoLoad_);
            if (cur != expected)
            {
                return true;
            }
            std::atomic_wait(&Rawptr_[idx], expected);
        }
        return false;
    }
    std::vector<size_t> FindState(tag8_t st_filter)
    {
        std::vector<size_t> v;
        v.reserve(MAX_VAL);
        for (size_t i = 0; i < Capacity_; i++)
        {
            packed64_t p = Rawptr_[idx].load(MoLoad_);
            tag8_t st = PackedCell64_t::StateFromSTRL(PackedCell64_t::ExtractSTRL(p));
            if (st == st_filter)
            {
                v.push_back(i);
            }
            return v;
        }
    }

};



}