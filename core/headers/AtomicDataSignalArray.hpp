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


namespace AtomicCScompact
{
    using HWCallback = void(*)(size_t current, size_t capacity, void* user);
    static inline constexpr uint64_t HASH_CONST = 11400714819323198485ull;


template<PackedMode MODE>
class AtomicDSA
{
private:
    std::atomic<PackedCell64_t*> Rawptr_{nullptr};
    size_t Capacity_{0};
    std::atomic<size_t> Count_{0};
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
            Count_.store(0, MoStoreUnSeq_);
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
        return Count_.load(MoLoad_);
    }
    

};



}