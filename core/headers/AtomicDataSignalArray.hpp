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
    size_t MaxTLS = 8192;
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
    std::atomic<size_t> Occupancy_{0};
    std::atomic<size_t> ProducerCursor_{0};
    std::atomic<size_t> ConsumerCursor_{0};

    //adaptivebackoff
    AtomicAdaptiveBackoff Adaptivebkof_;
    //re-offset
    std::vector<std::atomic<uint32_t>> RelOffSet_;
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
        Occupancy_.store(0, MoStoreUnSeq_);
        ProducerCursor_.store(0, MoStoreUnSeq_);
        ConsumerCursor_.store(0, MoStoreUnSeq_);
        RelOffSet_.assign(Capacity_, std::atomic<uint32_t>(0));
    }

    inline bool IfAnyValid_() const noexcept
    {
        return Backing_ && Capacity_ > 0;
    }
    inline bool IfIdxValid(size_t idx) const noexcept
    {
        return (Backing_ && (idx < Capacity_));
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
        RelOffSet_.clear();
        //clear region
        for(auto& v : RelBitmaps_)
        {
            v.clear;
        }
        RelBitmaps_.clear();
        RegionEpoch_.clear();
        RegionRel_.clear();
        RegionSize_ = 0;
        NumRegion_  = 0;
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
        if (!IfAnyValid_() || n == 0)
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

    PublishResult PublishPackedOfADSA(packed64_t item, int max_prob = -1) noexcept
    {
        if (!IfAnyValid_())
        {
            return {PublishStatus::INVALID, SIZE_MAX};
        }

        size_t seq = NextProducerSeq();

        if constexpr (MODE == PackedMode::MODE_VALUE32)
        {
            strl16_t sr = PackedCell64_t::ExtractSTRL(item);
            tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr);
            val32_t v = PackedCell64_t::ExtractValue32(item);

            size_t mt = ADSAThreadLocalMID_();
            if (MasterClkConfigaration_ && (mt != 0))
            {
                packed64_t mp = MasterClkConfigaration_->ReadMasterClockPacked(mt);
                packed64_t mc = PackedCell64_t::ExtractClk48(mp);
                clk16_t clk16 = static_cast<clk16_t>((mc >> Cfg_.TimerDownShift + CLK48TO16_PACKED_ERROR) & MaskBits(CLK_B16));
                item = PackedCell64_t::PackV32x_64(v, clk16, ST_PUBLISHED, relbyte);
            }
            else if (Cfg_.UseTimerStamp)
            {
                packed64_t now = Adaptivebkof_.PublicTimer48.NowTicks();
                clk16_t clk16 = static_cast<clk16_t>((now >> Cfg_.TimerDownShift + CLK48TO16_PACKED_ERROR) & MaskBits(CLK_B16));
                item = PackedCell64_t::PackV32x_64(v, clk16, ST_PUBLISHED, relbyte);
            }
            else
            {
                clk16_t clk16 = static_cast<clk16_t>(seq);
                item = PackedCell64_t::PackV32x_64(v, clk16, ST_PUBLISHED, relbyte)
            }
        }
        else
        {
            packed64_t c48 = PackedCell64_t::ExtractClk48(item);
            strl16_t sr = PackedCell64_t::ExtractSTRL(c48);
            tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr);
            item = PackedCell64_t::PackCLK48x_64(c48, ST_PUBLISHED, relbyte);
        }

        size_t start = seq;
        size_t idx = start % Capacity_;
        uint64_t mix = (static_cast<uint64_t>((start) * ID_HASH_GOLDEN_CONST) ^(static_cast<uint64_t>(start >> 33)));       //Why 33??
        size_t step = 1;
        if (Capacity_ > 1)
        {
            step = static_cast<size_t>( mix % (Capacity_ -1));
        }
        
        SpinBackoff spin_backoff;
        spin_backoff.MaxTries = 16;

        int probs = 0;
        while (true)
        {
            packed64_t cur = Backing_.[idx].load(MoLoad_);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            tag8_t stcur = PackedCell64_t::StateFromSTRL(csr);
            if (stcur == ST_IDLE)
            {
                packed64_t to_write = item;
                if (Cfg_.AllowPublishClockFixup && (to_write == cur))
                {
                    if constexpr (MODE == PackedMode::MODE_VALUE32)
                    {
                        clk16_t oldclk = PackedCell64_t::ExtractClk16(to_write);
                        clk16_t nclk = static_cast<clk16_t>(oldclk + 1u);
                        strl16_t sr2 = PackedCell64_t::ExtractClk16(to_write);
                        tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr2);
                        val32_t v = PackedCell64_t::ExtractValue32(to_write);
                        to_write = PackedCell64_t::PackV32x_64(v, nclk, ST_PUBLISHED, relbyte)
                    }
                    else if constexpr (MODE == PackedMode::MODE_CLKVAL48)
                    {
                        packed64_t c48 = PackedCell64_t::ExtractClk48(to_write);
                        c48 = static_cast<packed64_t>((c48 + 1) & MaskBits(CLK_B48));
                        strl16_t sr2 = PackedCell64_t::ExtractClk16(to_write);
                        tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr2);
                        to_write = PackedCell64_t::PackCLK48x_64(c48, ST_PUBLISHED, relbyte);
                    }
                }
                
                packed64_t expected = cur;
                if (Backing_[idx].compare_exchange_strong(expected, to_write, EXsuccess_, EXfailure_))
                {
                    if (RegionSize_)
                    {
                        UpdateRegionRelForIndex(idx, PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(to_write)));
                        Occupancy_.fetch_add(1, std::memory_order_acq_rel);
                        Backing_[idx].notify_all();
                        return {PublishStatus::OK, idx};
                    }
                    spin_backoff.SpinOnce();
                }
                ++probs;
                if ((max_prob >= 0) && (probs >= max_prob))
                {
                    return { PublishStatus::FULL, SIZE_MAX};
                }
                if (probs >= static_cast<int>(Capacity_))
                {
                    return { PublishStatus::FULL, SIZE_MAX};
                }
                
                idx += step;
                if (idx >= Capacity_)
                {
                    idx %= Capacity_;
                }
            }   
        }
    }

    PublishResult PublishWithOffset(packed64_t item, packed64_t offset, int max_prob = -1)
    {
        PublishResult r = PublishPackedOfADSA(item, max_prob);
        if (r.status == PublishStatus::OK)
        {
            SetEncodeOffset(r.index, offset);
        }
        return r;
    }

    PublishResult PublishBatchFromReserved(size_t reserved_base, const packed64_t* items, size_t n ) noexcept
    {
        if (!IfAnyValid_() || !items || n == 0)
        {
            return {PublishStatus::INVALID, SIZE_MAX};
        }
        for (size_t i = 0; i < n; i++)
        {
            size_t idx = (reserved_base + i) % Capacity_;
            packed64_t cur = Backing_[idx].load(MoLoad_);
            if (PackedCell64_t::StateFromSTRL(PackedCell64_t::ExtractSTRL(cur) != ST_IDLE))
            {
                return {PublishStatus::FULL, idx};
            }
            packed64_t expected = cur;
            if (!Backing_[idx].compare_exchange_strong(expected, items[i], EXsuccess_, EXfailure_))
            {
                return {PublishStatus::FULL, idx};
            }
            if (RegionSize_)
            {
                UpdateRegionRelForIndex(idx, PackedCell64_t::ExtractSTRL(PackedCell64_t::RelationFromSTRL(items[i])));
            }
            Occupancy_.fetch_add(1, std::memory_order_acq_rel);
        }
        Backing_[reserved_base % Capacity_].notify_all();
        return {PublishStatus::OK, reserved_base % Capacity_};  
    }

    PublishResult PublishBlockingPacked(packed64_t item, int timeout_ms = -1, uint16_t sleep_ms = 50) noexcept
    {
        using namespace std::chrono;
        auto start = steady_clock::now();
        while (true)
        {
            PublishResult res = PublishPackedOfADSA(item, static_cast<int>(Capacity_));
            if (res.status == PublishResult::OK)
            {
                return res;
            }
            if (timeout_ms == 0)
            {
                return res;   
            }
            if (timeout_ms > 0)
            {
                auto now = steady_clock::now();
                if (duration_cast<milliseconds>(now - start).count() >= timeout_ms)
                {
                    return r;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_ms));
        }
    }

    bool ClaimOne(tag8_t rel_mask_low_5, size_t& out_idx, packed64_t& out_observed, int max_scan = -1) noexcept
    {
        if (!IfAnyValid_())
        {
            return false;
        }
        size_t start = ConsumerCursor_.fetch_add(1, MoStoreUnSeq_);
        size_t idx = start % Capacity_;
        uint64_t mix = (static_cast<uint64_t>(start) * ID_HASH_GOLDEN_CONST) ^(static_cast<uint64_t>(start) >> 29) // why shift 29 bit ??
        size_t step = 1;
        if (Capacity_ > 1)
        {
            static_cast<size_t>((mix % Capacity_ -1) + 1);
        }

        size_t available = Occupancy_.load(MoLoad_);
        if (available == 0)
        {
            return false;
        }
        size_t scan_limit = std::min<size_t>(Capacity_, std::max<size_t>(available + 8, Cfg_.ScanLimit));
        
        SpinBackoff spin_co;
        int scans = 0;
        while (scans < static_cast<int>(scan_limit))
        {
            packed64_t cur = Backing_[idx].load(MoLoad_);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            if (PackedCell64_t::StateFromSTRL(csr) == ST_PUBLISHED)
            {
                tag8_t relbyte = PackedCell64_t::RelationFromSTRL(csr);
                tag8_t slot_rel_mask = PackedCell64_t::RelMaskBSetFromRelation(relbyte);
                if ((slot_rel_mask & rel_mask_low_5) != 0)
                {
                    packed64_t desired = PackedCell64_t::SetSTRLInPacked(cur, PackedCell64_t::MakeSTRL(ST_CLAIMED, relbyte));
                    packed64_t expected = cur;
                    if (Backing_[idx].compare_exchange_strong(expected, desired, EXsuccess_, EXfailure_))
                    {
                        out_idx = idx;
                        out_observed = cur;
                        return true;
                    }
                    else
                    {
                        ApplyBackoffForSlot(cur, idx);
                    }
                }
            }
            ++scans;
            idx += step;
            if (idx >= Capacity_)
            {
                idx = idx % Capacity_;
            }
            if (max_scan >= 0)
            {
                return false;
            }
            
            //Gather Phase
            size_t tls_cap = std::min<size_t>(Cfg_.MaxTlsCandidates, Cfg_.MaxTLS);
            struct Candidate_CO
            {
                size_t Idx;
                packed64_t Obs;
                int EffPtr;
                uint16_t SlotSeq16;
            };

            thread_local static Candidate_CO tls_cand[Cfg_.MaxTLS];
            Candidate_CO* cbuf = tls_cand;
            size_t ccount = 0;

            idx = start % Capacity_;

            uint16_t prod_seq16 = static_cast<uint16_t>((ProducerCursor_.load(MoLoad_)) & 0xFFFFu); // why 0xFFFFu ??
            while (scans < static_cast<int>(scan_limit) && ccount < std::min<size_t>(Cfg_.MaxGather, tls_cap))
            {
                packed64_t cur = Backing_[idx].load(MoLoad_);
                strl16_t csr = PackedCell64_t::ExtractClk16(cur);
                if (PackedCell64_t::StateFromSTRL(csr) == ST_PUBLISHED)
                {
                    tag8_t relbyte = PackedCell64_t::RelationFromSTRL(csr);
                    tag8_t slot_rel_mask = PackedCell64_t::RelMaskBSetFromRelation(relbyte);
                    if ((slot_rel_mask & rel_mask_low_5) != 0)
                    {
                        uint16_t slot_seq16 = 0;
                        if constexpr (MODE == PackedMode::MODE_VALUE32)
                        {
                            slot_seq16 = static_cast<clk16_t>(PackedCell64_t::ExtractClk16(cur));
                        }
                        int eff ; //ComputeEffectivePriority
                    }
                    
                }
                
            }
            
            
        }
        
    }

    inline packed64_t ComputeEffectivePriority(tag8_t relbyte, uint16_t slot_seq16, uint16_t prod_seq16) noexcept
    {
        int base_pr = static_cast<int>(PackedCell64_t::PriorityFromRelation(relbyte));
        uint16_t age = static_cast<uint16_t>(prod_seq16 - slot_seq16);
        int age_bonus = std::min<int>(MAX_PRIORITY, (age >> 8)); //why the bit shift and why MAX_PRIORITY??(MAX_PRIORITY==7) 
        
    }


    inline void ApplyBackoffForSlot(packed64_t observed, size_t idx) noexcept
    {
        auto decision = Adaptivebkof_.DecideForSlot(observed);
        using PAct = AtomicAdaptiveBackoff::PCBAction;
        if (decision.Action == PAct::SPIN_IMMEDIATE)
        {
            CpuRelaxHint();
            return;
        }
        else if (decision.Action == PAct::SPIN_FOR_US)
        {
            auto dur = std::chrono::microseconds(decision.SuggestedUs);
            auto start = std::chrono::steady_clock::now();
            while((std::chrono::steady_clock::now() - start) < dur)
            {
                CpuRelaxHint();
                packed64_t cur = Backing_[idx].load(MoLoad_);
                if (cur != observed)
                {
                    break;
                }
            }
            return;
        }
        else if (decision.Action == PAct::PARK_FOR_US)
        {
            int ms = static_cast<int>((decision.SuggestedUs + 999) / 1000); // why +999/1000??
            Backing_[idx].wait(observed);
            return;
        }
        else
        {
            Backing_[idx].wait(observed);
            return;
        }
    }

    void UpdateRegionRelForIndex(size_t idx, tag8_t relbyte);


    void SetEncodeOffset(size_t idx, packed64_t offset) noexcept
    {
        if (!IfIdxValid(idx))
        {
            return 0;
        }
        RelOffSet_[idx].store(offset, MoStoreSeq_);
    }

    void InitRegionIndex(size_t region_size)
    {

    }
};


}
