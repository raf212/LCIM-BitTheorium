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
    std::atomic<packed64_t>* MasterClockSlotsPtr_{nullptr};
    size_t MasterCLKCapacity_{0};
    std::atomic<size_t> MasterAlloc_{0};

    //TLS Thread MAster ID
    static inline thread_local size_t& ThreadLocMasterID_() noexcept
    {
        static thread_local size_t id = SIZE_MAX;
        return id;
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
        RegionSize_(0), NumRegion_(0)
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
    //master clock functions
    bool InitMasterClockSlots(size_t max_slots, size_t allignment = 64)
    {
        if (!MasterClockSlotsPtr_)
        {
            return;
        }
        if (max_slots == 0)
        {
            throw std::invalid_argument("MAX SLOTS == 0");
        }
        size_t bytes = sizeof(std::atomic<packed64_t>) * max_slots;
        void* mem = AllocNW::AlignedAllocONnode(allignment, bytes, UsedNode_);
        if (!mem)
        {
            throw std::bad_alloc();
        }
        MasterClockSlotsPtr_ = reinterpret_cast<std::atomic<packed64_t>*>(mem);
        uint64_t now = Adaptivebkof_.PublicTimer48.NowTicks();
        packed64_t init_p = PackedCell64_t::PackCLK48x_64((now & MaskBits(CLK_B48)), ST_IDLE, REL_NONE);
        for (size_t i = 0; i < max_slots; i++)
        {
            new (&MasterClockSlotsPtr_[i] std::atomic<packed64_t>(init_p));
        }
        MasterCLKCapacity_ = max_slots;
        MasterAlloc_.store(0, MoStoreSeq_);
        
    }

    void FreeMasterClockSlots() noexcept
    {
        if (!MasterClockSlotsPtr_)
        {
            return;
        }
        for (size_t i = 0; i < MasterCLKCapacity_; i++)
        {
            MasterClockSlotsPtr_[i]~atomic();
        }
        size_t bytes = sizeof(std::atomic<packed64_t>) * MasterCLKCapacity_;
        AllocNW::FreeONNode(static_cast<void*>(MasterClockSlotsPtr_), bytes);
        MasterClockSlotsPtr_ = nullptr;
        MasterCLKCapacity_ = 0;
        MasterAlloc_.store(0, MoStoreUnSeq_);
    }

    size_t RegisterMasterClockSlot(packed64_t initial = 0) noexcept
    {
        if (!MasterClockSlotsPtr_ || MasterCLKCapacity_ == 0)
        {
            return SIZE_MAX;
        }
        size_t id = MasterAlloc_.fetch_add(1, std::memory_order_acq_rel);
        if (id >= MasterCLKCapacity_)
        {
            MasterAlloc_.fetch_sub(1, std::memory_order_acq_rel);
            return SIZE_MAX;
        }
        packed64_t p = PackedCell64_t::PackCLK48x_64((initial & MaskBits(CLK_B48)), ST_PUBLISHED, REL_NONE);
        MasterClockSlotsPtr_[id].store(p, MoStoreSeq_);
        MasterClockSlotsPtr_[id].notify_all();
        return id;
    }

    bool UpdateMasterClock(size_t master_id, packed64_t clk48) noexcept
    {
        if (!MasterClockSlotsPtr_ || master_id >= MasterCLKCapacity_ )
        {
            return false;
        }
        auto& slot = MasterClockSlotsPtr_[master_id];
        packed64_t  oldv = slot.load(MoLoad_);
        while (true)
        {
            strl16_t sr = PackedCell64_t::ExtractSTRL(oldv);
            tag8_t st = PackedCell64_t::StateFromSTRL(sr);
            tag8_t rel = PackedCell64_t::RelationFromSTRL(sr);
            packed64_t desired = PackedCell64_t::PackCLK48x_64((clk48 & MaskBits(CLK_B48)), st, rel);
            if (slot.compare_exchange_weak(oldv, desired, EXsuccess_, MoLoad_))
            {
                slot.notify_all();                
            }
            CpuRelaxHint();
        }
    }

    packed64_t ReadMasterClock(size_t master_id) const noexcept
    {
        if (!MasterClockSlotsPtr_ || master_id >= MasterCLKCapacity_)
        {
            return 0;
        }
        packed64_t p = MasterClockSlotsPtr_[master_id].load(MoLoad_);
        return PackedCell64_t::ExtractClk48(p);
    }

    size_t AttachThreadMasterID(size_t master_id) const noexcept
    {
        size_t previous = ThreadLocMasterID_();
        ThreadLocMasterID_() = master_id;
        return previous;
    }

    packed64_t ReadMasterCLKPacked(size_t master_id)
    {
        if (!MasterClockSlotsPtr_ || master_id >= MasterCLKCapacity_)
        {
            return 0;
        }
        return MasterClockSlotsPtr_[master_id].load(MoLoad_);
        
    }
    void InitRegionIndex(size_t region_size)
    {

    }
};


}
