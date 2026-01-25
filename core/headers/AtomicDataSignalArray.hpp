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
#include "MasterClockConf.hpp"

namespace AtomicCScompact
{
    static_assert(__cpp_lib_atomic_wait, "C++ must support attomic wait");

    static inline void CpuRelaxHint()
    {
    #if defined(_MSC_VER)
        YieldProcessor();
    #else   
        __asm__ __volatile__("pause" ::: "memory");
    #endif
    }

struct ADSAConfig
{
    size_t ScanLimit = 256;
    size_t MaxGather = 1024;
    unsigned TimerDownShift = 10u;
    bool UseTimerStamp = true;
    bool AllowPublishClockFixup = true;
    size_t MaxTlsCandidates = 4096;
    size_t ProducerBlockSize = 64;
    size_t RegionSize = 0;
};

enum class PublishStatus : uint8_t
{
    OK = 0,
    FULL = 1,
    INVALID = 2,
};

struct PublishResult
{
    PublishStatus status;
    size_t index;
};

template<PackedMode MODE>
class AtomicDataSignalArray
{
private:
    std::atomic<packed64_t>* Backing_{nullptr};
    size_t Capacity_{0};
    bool Owned_{false};
    int UsedNode_{0};

    ADSAConfig Cfg_;
    //occupancy & cursor
    std::atomic<size_t> Occ_{0};
    std::atomic<size_t> ProducerCursor_{0};
    std::atomic<size_t> ConsumerCursor_{0};

    //adaptivebackoff
    AtomicAdaptiveBackoff Adaptivebkof_;
    //re-offset
    std::vector<std::atomic<uint32_t>> RelOffst_;
    //region
    size_t RegionSize_{0};
    size_t NumRegion_{0};
    std::vector<std::atomic<uint64_t>> RegionEpoch_;
    std::vector<std::atomic<uint8_t>> RegionRel_;
    std::vector<std::vector<std::atomic<uint64_t>>> RelBitmaps_; //[bit][word]

    //master clock
    MasterClockConf* MasterClkConfigaration_;

    static inline thread_local size_t& ADSAThreadLocalMID_() noexcept
    {
        static thread_local size_t id = SIZE_MAX;
    }

    void InitZeroOCR_()
    {
        Occ_.store(0, MoStoreUnSeq_);
        ProducerCursor_.store(0, MoStoreUnSeq_);
        ConsumerCursor_.store(0, MoStoreUnSeq_);
        RelOffst_.assign(Capacity_, std::atomic<uint32_t>(0));
    }

    inline bool AnyValid_() const noexcept
    {
        return Backing_ && Capacity_ > 0;
    }

public:
    AtomicDataSignalArray() noexcept :
        Backing_(nullptr), Capacity_(0), Owned_(false), UsedNode_(0),
        Cfg_(), Adaptivebkof_(typename AtomicAdaptiveBackoff::PCBCfg{}, MODE),
        RegionSize_(0), NumRegion_(0), MasterClkConfigaration_(nullptr)
    {}

    ~AtomicDataSignalArray()
    {
        FreeAll();
    }

    AtomicDataSignalArray(const AtomicDataSignalArray&) = delete;
    AtomicDataSignalArray& operator = (const AtomicDataSignalArray&) = delete;
    
    void InitOwned(size_t capacity, int node = REL_NODE0, ADSAConfig cfg = {}, size_t alignment = 64)
    {
        FreeAll();
        if (capacity == 0)
        {
            throw std::invalid_argument("capacity == 0");
        }
        std::atomic<packed64_t>test{0};
        size_t bytes = sizeof(std::atomic<packed64_t>)* capacity;
        void* mem = AllocNW::AlignedAllocONnode(alignment, bytes, node);
        if (!mem)
        {
            throw std::bad_alloc();
        }
        Backing_ = reinterpret_cast<std::atomic<packed64_t>*>(mem);
        packed64_t idle = PackedCell64_t::MakeInitialPacked(MODE);
        for (size_t i = 0; i < capacity; i++)
        {
            new (&Backing_[i]) std::atomic<packed64_t>(idle); 
        }
        Capacity_ = capacity;
        Owned_ = true;
        UsedNode_ = node;
        Cfg_ = cfg;

        InitZeroOCR_();

        if (Cfg_.RegionSize)
        {
            InitRegionIndex(Cfg_.RegionSize);
        }
    }

    void InitFromExisting(std::atomic<packed64_t>* backing, ADSAConfig cfg = {}, size_t capacity)
    {
        FreeAll();
        if (!backing)
        {
            throw std::invalid_argument("backing == nullptr");
        }
        if (capacity == 0)
        {
            throw std::invalid_argument("capacity == 0");
        }

        Backing_ = backing;
        Capacity_ = capacity;
        Owned_ = false;
        UsedNode_ = -1;
        Cfg_ = cfg;

        InitZeroOCR_();

        if (Cfg_.RegionSize)
        {
            InitRegionIndex(Cfg_.RegionSize);
        }
    }

    void FreeAll()
    {
        if (Backing_)
        {
            if (Owned_)
            {
                for (size_t i = 0; i < Capacity_; i++)
                {
                    Backing_[i].~atomic();
                }
                size_t bytes = sizeof(std::atomic<packed64_t>) * Capacity_;
                AllocNW::FreeONNode(static_cast<void*>(Backing_), bytes);
            }
        }
        Capacity_ = 0;
        Owned_ = false;
        RelOffst_.clear();
        //clear region
        for(auto& v : RelBitmaps_)
        {
            v.clear;
        }
        RelBitmaps_.clear();
        RegionEpoch_.clear();
        RegionRel_.clear();
        RegionSize_ = 0;
    }

    void SetMasterClockConf(MasterClockConf* mc) noexcept
    {
        MasterClkConfigaration_ = mc;
    }

    size_t AttachThreadMasterClockID(size_t m_id) noexcept
    {
        size_t prev = ADSAThreadLocalMID_();
        ADSAThreadLocalMID_() = MasterClkConfigaration_;
        if (MasterClkConfigaration_)
        {
            (void)MasterClkConfigaration_->AttachThreadMClockID(m_id);
        }
        return prev;
    }

    
    size_t ReserveProducerSlots(size_t n) noexcept
    {
        if (!AnyValid_() || n == 0)
        {
            return SIZE_MAX;
        }
        return ProducerCursor_.fetch_add(n, MoStoreUnSeq_);
    }

    size_t NextProducerSeq() noexcept
    {
        thread_local size_t block_base = 0;
        thread_local size_t block_left = 0;
        if (block_left == 0)
        {
            size_t block = std::min<size_t>(Cfg_.ProducerBlockSize, Capacity_);
            block_base = ReserveProducerSlots(block);
            block_left = block;
        }
        size_t seq = block_base++;
        --block_left;
        return seq;
    }
    void InitRegionIndex(size_t region_size)
    {

    }
};


}
