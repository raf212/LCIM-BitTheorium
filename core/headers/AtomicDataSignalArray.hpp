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
    static constexpr size_t MaxTLS = 8192;
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
public:
    std::atomic<packed64_t>* BackingPtr{nullptr};
private:
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
    std::vector<packed64_t> RelOffSet_;
    //region
    size_t RegionSize_{0};
    size_t NumRegion_{0};
    std::vector<packed64_t> RegionEpoch_;
    std::vector<uint8_t> RegionRel_;
    std::vector<std::vector<uint64_t>> RelBitmaps_; //[bit][word]

    //master clock
    MasterClockConf* MasterClkConfigaration_;
    struct Candidate_CO_
    {
        size_t Idx;
        packed64_t Obs;
        uint32_t EffPtr;
        uint16_t SlotSeq16;
    };
    static inline size_t& ADSAThreadLocalMID_() noexcept
    {
        static thread_local size_t id = SIZE_MAX;
        return id;
    }

    void InitZeroOCR_()
    {
        Occupancy_.store(0, MoStoreUnSeq_);
        ProducerCursor_.store(0, MoStoreUnSeq_);
        ConsumerCursor_.store(0, MoStoreUnSeq_);
        RelOffSet_.assign(Capacity_, packed64_t(0));
    }

    void InitRelRegionAndBitmap_()
    {
        RegionRel_.clear();
        RegionRel_.resize(NumRegion_);
        for (size_t i = 0; i < NumRegion_; i++)
        {
            RegionRel_[i] = static_cast<tag8_t>(0);
        }

        size_t words = (NumRegion_ +(MAX_VAL -1)) / MAX_VAL;
        RelBitmaps_.clear();
        RelBitmaps_.resize(LN_OF_BYTE_IN_BITS);
        for (size_t i = 0; i < LN_OF_BYTE_IN_BITS; i++)
        {
            RelBitmaps_[i].clear();
            RelBitmaps_[i].resize(words);
            for (size_t j = 0; j < words; j++)
            {
                RelBitmaps_[i][j] = 0ull;
            }
        }
        RegionEpoch_.assign(NumRegion_, 0ull);
    }

    inline bool IfAnyValid_() const noexcept
    {
        return BackingPtr && Capacity_ > 0;
    }
    inline bool IfIdxValid(size_t idx) const noexcept
    {
        return (BackingPtr && (idx < Capacity_));
    }

    inline void CommitPayloadBasedMODE_(packed64_t payload, packed64_t& committed, size_t idx) noexcept
    {
        if (!IfIdxValid(idx))
        {
            committed = PackedCell64_t::MakeInitialPacked(MODE);
            return;
        }
        if (payload == 0)
        {
            committed = PackedCell64_t::MakeInitialPacked(MODE);
            return;
        }

        strl16_t sr = PackedCell64_t::ExtractSTRL(payload);
        tag8_t st = ExtractLocalityFromSTRL(sr);
        if (st == ST_IDLE)
        {
            committed = PackedCell64_t::MakeInitialPacked(MODE);
            return;
        }
        committed = PackedCell64_t::SetLocalityInPacked(payload, ST_COMPLETE);

        BackingPtr[idx].store(committed, MoStoreSeq_);
        if (RegionSize_)
        {
            UpdateRegionRelForIndex(idx, PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(committed)));
        }
    }

public:
    AtomicDataSignalArray() noexcept :
        BackingPtr(nullptr), Capacity_(0), Owned_(false), UsedNode_(0),
        Cfg_(), Adaptivebkof_(typename AtomicAdaptiveBackoff::PCBCfg{}, MODE),
        RegionSize_(0), NumRegion_(0), MasterClkConfigaration_(nullptr)
    {}

    ~AtomicDataSignalArray()
    {
        FreeAll();
    }

    AtomicDataSignalArray(const AtomicDataSignalArray&) = delete;
    AtomicDataSignalArray& operator = (const AtomicDataSignalArray&) = delete;
    
    void InitOwned(size_t capacity, int node = REL_NONE, ADSAConfig cfg = {}, size_t alignment = 64)
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
        BackingPtr = reinterpret_cast<std::atomic<packed64_t>*>(mem);
        packed64_t idle = PackedCell64_t::MakeInitialPacked(MODE);
        for (size_t i = 0; i < capacity; i++)
        {
            new (&BackingPtr[i]) std::atomic<packed64_t>(idle); 
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

    void InitFromExisting(std::atomic<packed64_t>* backing, ADSAConfig cfg = {}, size_t capacity = 0)
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

        BackingPtr = backing;
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
        if (BackingPtr)
        {
            if (Owned_)
            {
                for (size_t i = 0; i < Capacity_; i++)
                {
                    BackingPtr[i].~atomic<packed64_t>();
                }
                size_t bytes = sizeof(std::atomic<packed64_t>) * Capacity_;
                AllocNW::FreeONNode(static_cast<void*>(BackingPtr), bytes);
            }
        }

        BackingPtr = nullptr;
        Capacity_ = 0;
        Owned_ = false;
        RelOffSet_.clear();
        //clear region
        for(auto& vec : RelBitmaps_)
        {
            vec.clear();
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
        ADSAThreadLocalMID_() = m_id;
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
                clk16_t clk16 = static_cast<clk16_t>((mc >> (Cfg_.TimerDownShift + CLK48TO16_PACKED_ERROR)) & MaskBits(CLK_B16));
                item = PackedCell64_t::PackV32x_64(v, clk16, ST_PUBLISHED, relbyte);
            }
            else if (Cfg_.UseTimerStamp)
            {
                packed64_t now = Adaptivebkof_.PublicTimer48.NowTicks();
                clk16_t clk16 = static_cast<clk16_t>((now >> (Cfg_.TimerDownShift + CLK48TO16_PACKED_ERROR)) & MaskBits(CLK_B16));
                item = PackedCell64_t::PackV32x_64(v, clk16, ST_PUBLISHED, relbyte);
            }
            else
            {
                clk16_t clk16 = static_cast<clk16_t>(seq);
                item = PackedCell64_t::PackV32x_64(v, clk16, ST_PUBLISHED, relbyte);
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
            step = static_cast<size_t>((mix % (Capacity_ - 1)) + 1);
        }
        
        SpinBackoff spin_backoff;
        spin_backoff.MaxTries = 16;

        int probs = 0;
        while (true)
        {
            packed64_t cur = BackingPtr[idx].load(MoLoad_);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            tag8_t stcur = ExtractLocalityFromSTRL(csr);
            if (stcur == ST_IDLE)
            {
                packed64_t to_write = item;
                if (Cfg_.AllowPublishClockFixup && (to_write == cur))
                {
                    if constexpr (MODE == PackedMode::MODE_VALUE32)
                    {
                        clk16_t oldclk = PackedCell64_t::ExtractClk16(to_write);
                        clk16_t nclk = static_cast<clk16_t>(oldclk + 1u);
                        strl16_t sr2 = PackedCell64_t::ExtractSTRL(to_write);
                        tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr2);
                        val32_t v = PackedCell64_t::ExtractValue32(to_write);
                        to_write = PackedCell64_t::PackV32x_64(v, nclk, ST_PUBLISHED, relbyte);
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
                if (BackingPtr[idx].compare_exchange_strong(expected, to_write, EXsuccess_, EXfailure_))
                {
                    if (RegionSize_)
                    {
                        UpdateRegionRelForIndex(idx, PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(to_write)));
                    }
                    Occupancy_.fetch_add(1, std::memory_order_acq_rel);
                    BackingPtr[idx].notify_all();
                    return {PublishStatus::OK, idx};
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
            packed64_t cur = BackingPtr[idx].load(MoLoad_);
            if (ExtractLocalityFromSTRL(PackedCell64_t::ExtractSTRL(cur)) != ST_IDLE)
            {
                return {PublishStatus::FULL, idx};
            }
            packed64_t expected = cur;
            if (!BackingPtr[idx].compare_exchange_strong(expected, items[i], EXsuccess_, EXfailure_))
            {
                return {PublishStatus::FULL, idx};
            }
            if (RegionSize_)
            {
                UpdateRegionRelForIndex(idx, PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(items[i])));
            }
            Occupancy_.fetch_add(1, std::memory_order_acq_rel);
        }
        BackingPtr[reserved_base % Capacity_].notify_all();
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
                    return res;
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
        uint64_t mix = (static_cast<uint64_t>(start) * ID_HASH_GOLDEN_CONST) ^(static_cast<uint64_t>(start) >> 29); // why shift 29 bit ??
        size_t step = 1;
        if (Capacity_ > 1)
        {
            step = static_cast<size_t>((mix % (Capacity_ - 1)) + 1);
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
            packed64_t cur = BackingPtr[idx].load(MoLoad_);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            if (ExtractLocalityFromSTRL(csr) == ST_PUBLISHED)
            {
                tag8_t relbyte = PackedCell64_t::RelationFromSTRL(csr);
                tag8_t slot_rel_mask = PackedCell64_t::RelMaskBSetFromRelation(relbyte);
                if ((slot_rel_mask & rel_mask_low_5) != 0)
                {
                    packed64_t desired = PackedCell64_t::SetLocalityInPacked(cur, ST_CLAIMED);
                    packed64_t expected = cur;
                    if (BackingPtr[idx].compare_exchange_strong(expected, desired, EXsuccess_, EXfailure_))
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
            if (max_scan >= 0 && scans >= max_scan)
            {
                return false;
            }
        }
        //Gather Phase
        size_t tls_cap = std::min<size_t>(Cfg_.MaxTlsCandidates, Cfg_.MaxTLS);
        std::vector<Candidate_CO_> cand_buf;
        cand_buf.reserve(std::min<size_t>(Cfg_.MaxGather, tls_cap));
        idx = start % Capacity_;
        scans = 0;

        uint16_t prod_seq16 = static_cast<uint16_t>((ProducerCursor_.load(MoLoad_)) & 0xFFFFu); // why 0xFFFFu ??
        while (scans < static_cast<int>(scan_limit) && cand_buf.size() < std::min<size_t>(Cfg_.MaxGather, tls_cap))
        {
            packed64_t cur_2 = BackingPtr[idx].load(MoLoad_);
            strl16_t csr_2 = PackedCell64_t::ExtractSTRL(cur_2);
            if (ExtractLocalityFromSTRL(csr_2) == ST_PUBLISHED)
            {
                tag8_t relbyte = PackedCell64_t::RelationFromSTRL(csr_2);
                tag8_t slot_rel_mask = PackedCell64_t::RelMaskBSetFromRelation(relbyte);
                if ((slot_rel_mask & rel_mask_low_5) != 0)
                {
                    uint16_t slot_seq16 = 0;
                    if constexpr (MODE == PackedMode::MODE_VALUE32)
                    {
                        slot_seq16 = static_cast<clk16_t>(PackedCell64_t::ExtractClk16(cur_2));
                    }
                    packed64_t effect_prio_packed = ComputeEffectivePriority(slot_seq16, prod_seq16);
                    strl16_t c_esr = PackedCell64_t::ExtractSTRL(effect_prio_packed);
                    if (ExtractLocalityFromSTRL(c_esr) == ST_PUBLISHED)
                    {
                        Candidate_CO_ cand {idx, cur_2, PackedCell64_t::ExtractValue32(effect_prio_packed), slot_seq16};
                        cand_buf.push_back(cand);
                    }
                }
            }
            ++scans;
            idx = idx + step;
            if (idx >= Capacity_)
            {
                idx = idx % Capacity_;
            }
        }

        if (cand_buf.empty())
        {
            return false;
        }
        size_t best = 0;
        for (size_t i = 0; i < cand_buf.size(); i++)
        {
            if (cand_buf[i].EffPtr > cand_buf[best].EffPtr)
            {
                best = i;
            }
        }
        Candidate_CO_ best_candidate = cand_buf[best];

        packed64_t desired = PackedCell64_t::SetLocalityInPacked(best_candidate.Obs, ST_CLAIMED);

        //continue from here
        packed64_t expected = best_candidate.Obs;
        if (BackingPtr[best_candidate.Idx].compare_exchange_strong(expected, desired, EXsuccess_, EXfailure_))
        {
            out_idx = best_candidate.Idx;
            out_observed = best_candidate.Obs;
            return true;
        }
        else
        {
            packed64_t latest = BackingPtr[best_candidate.Idx].load(MoLoad_);
            ApplyBackoffForSlot(latest, best_candidate.Idx);
            return false;
        }
    }

    packed64_t ClaimBatch(tag8_t rel_mask_low5, std::vector<std::pair<size_t, packed64_t>>& out, size_t max_count) noexcept
    {
        uint8_t this_prio = 4;
        out.clear();
        if (!IfAnyValid_() || max_count == 0)
        {
            return 0;
        }
        size_t start = ConsumerCursor_.fetch_add(1, MoStoreUnSeq_);
        size_t idx = start % Capacity_;
        uint64_t mix = (static_cast<uint64_t>(start) * ID_HASH_GOLDEN_CONST) ^(static_cast<uint64_t>(start) >> 31); // why 31??
        size_t step = 1;
        if (Capacity_ > 1)
        {
            step = static_cast<size_t>((mix % (Capacity_ - 1)) + 1);
        }
        size_t available = Occupancy_.load(MoLoad_);
        if (available == 0)
        {
            return 0;
        }
        size_t scan_limit = min(Capacity_, std::max<size_t>(available + 16, max_count * 8));
        size_t tls_cap = std::min<size_t>(Cfg_.MaxTlsCandidates, Cfg_.MaxTLS);
        thread_local Candidate_CO_ tls_buf[Cfg_.MaxTLS];
        std::vector<Candidate_CO_> buffer;
        buffer.reserve(std::min<size_t>(Cfg_.MaxGather, tls_cap));
        size_t buffer_count = 0;
        uint16_t producer_sequense16 = static_cast<uint16_t>(ProducerCursor_.load(MoLoad_) & 0xFFFFu); // why 0xFFFFu;
        size_t scans = 0;
        idx = start % Capacity_;
        while (scans < scan_limit && buffer_count < std::min<size_t>(Cfg_.MaxGather, tls_cap))
        {
            packed64_t cur = BackingPtr[idx].load(MoLoad_);
            strl16_t csr = PackedCell64_t::ExtractSTRL(cur);
            if (ExtractLocalityFromSTRL(csr) == ST_PUBLISHED)
            {
                tag8_t relbyte = PackedCell64_t::RelationFromSTRL(csr);
                tag8_t rel_mask_here = PackedCell64_t::RelMaskBSetFromRelation(relbyte);
                if ((rel_mask_here & rel_mask_low5) != 0)
                {
                    uint16_t slot_seq16 = 0;
                    if constexpr (MODE == PackedMode::MODE_VALUE32)
                    {
                        slot_seq16 = static_cast<uint16_t>(PackedCell64_t::ExtractClk16(cur));
                        packed64_t effec_prio_packed = ComputeEffectivePriority(slot_seq16, producer_sequense16);
                        strl16_t c_esr = PackedCell64_t::ExtractSTRL(effec_prio_packed);
                        if (ExtractLocalityFromSTRL(c_esr) == ST_PUBLISHED)
                        {
                            Candidate_CO_ cand {idx, cur, PackedCell64_t::ExtractValue32(effec_prio_packed), slot_seq16};
                            buffer.push_back(cand);
                            ++buffer_count;
                        }
                    }
                }
            }
            ++scans;
            idx = idx + step;
            if (idx >= Capacity_)
            {
                idx = idx % Capacity_;
            }
        }
        if (buffer.empty())
        {
            return 0; // I should define state and priority ???
        }
        size_t k = std::min<size_t>(max_count, buffer_count);
        auto comp = [](const Candidate_CO_& a, const Candidate_CO_& b) -> bool {
            if (a.EffPtr != b.EffPtr) return a.EffPtr > b.EffPtr;
            return a.Idx < b.Idx;
        };
        std::nth_element(buffer.begin(), buffer.begin() + k, buffer.begin() + buffer_count, comp);
        std::sort(buffer.begin(), buffer.begin() + k, comp);
        SpinBackoff in_claimbatch_spin;
        for (size_t i = 0; i < k; i++)
        {
            Candidate_CO_& c = buffer[i];
            packed64_t desired = PackedCell64_t::SetLocalityInPacked(c.Obs, ST_CLAIMED);
            packed64_t expected = c.Obs;
            if (BackingPtr[c.Idx].compare_exchange_strong(expected, desired, EXsuccess_, EXfailure_))
            {
                out.emplace_back(c.Idx, c.Obs);
            }
            else
            {
                packed64_t latest = BackingPtr[c.Idx].load(MoLoad_);
                ApplyBackoffForSlot(latest, c.Idx);
                in_claimbatch_spin.SpinOnce();
            }
            if (out.size() >= max_count)
            {
                break;
            }
        }
        
        size_t mt = ADSAThreadLocalMID_();
        clk16_t in_cb_clk16 = 0;
        GetNewClock16ForThread(mt, in_cb_clk16);

        packed64_t size_of_batch = PackedCell64_t::PackV32x_64(static_cast<val32_t>(out.size()),in_cb_clk16, ST_PUBLISHED, PackedCell64_t::PackRel8x_t(REL_NONE, this_prio));
        return size_of_batch;
    }
    
    void CommitIdxWithPayload(size_t idx, packed64_t payload) noexcept
    {
        if (!IfIdxValid(idx))
        {
            return;
        }
        packed64_t committed = 0;
        CommitPayloadBasedMODE_(payload, committed, idx);
        if (committed != PackedCell64_t::MakeInitialPacked(MODE))
        {
            BackingPtr[idx].notify_all();
        }        
    }
    
    void CommitMarkComplete(size_t idx) noexcept
    {
        if (!IfIdxValid(idx))
        {
            return;
        }
        packed64_t oldv = BackingPtr[idx].load(MoLoad_);
        packed64_t committed = 0;
        CommitPayloadBasedMODE_(oldv, committed, idx);
        if (committed != PackedCell64_t::MakeInitialPacked(MODE))
        {
            BackingPtr[idx].notify_all();
        }
    }

    inline void GetNewClock16ForThread(size_t mt, clk16_t& clk16)
    {
        if (MasterClkConfigaration_ && (mt != 0))
        {
            packed64_t mp = MasterClkConfigaration_->ReadMasterClockPacked(mt);
            packed64_t now = PackedCell64_t::ExtractClk48(mp);
            clk16 = static_cast<clk16_t>((now >> (Cfg_.TimerDownShift + CLK48TO16_PACKED_ERROR)) & MaskBits(CLK_B16));
        }
        else
        {
            throw std::runtime_error("MasterClock not configured or thread id not attached");
        }
    }

    packed64_t Recycle(size_t idx) noexcept
    {
        if (!IfIdxValid(idx))
        {
            return 0;
        }
        packed64_t prev = BackingPtr[idx].load(MoLoad_);
        BackingPtr[idx].store(PackedCell64_t::MakeInitialPacked(MODE), MoStoreSeq_);
        Occupancy_.fetch_sub(1, std::memory_order_acq_rel);
        BackingPtr[idx].notify_all();
        return prev;
        
    }

    inline packed64_t ComputeEffectivePriority(uint16_t slot_seq16, uint16_t prod_seq16) noexcept
    {
        
        uint8_t prio_of_this = 4;
        uint16_t age = static_cast<uint16_t>(prod_seq16 - slot_seq16);
        int age_bonus = std::min<int>(MAX_PRIORITY, (age >> 8)); //why the bit shift and why MAX_PRIORITY??(MAX_PRIORITY==7) 
        size_t mt = ADSAThreadLocalMID_();
        if (MasterClkConfigaration_ && (mt != 0))
        {
            clk16_t cep_clk16 = 0;
            GetNewClock16ForThread(mt, cep_clk16);
            packed64_t  packed_bonus = PackedCell64_t::PackV32x_64(static_cast<uint32_t>(age_bonus), cep_clk16, ST_PUBLISHED, PackedCell64_t::PackRel8x_t(REL_NONE, prio_of_this));
            return packed_bonus;
        }
        clk16_t clk16 = 0;
        packed64_t  packed_bonus = PackedCell64_t::PackV32x_64(static_cast<uint32_t>(age_bonus), clk16, ST_PUBLISHED, PackedCell64_t::PackRel8x_t(REL_NONE, prio_of_this));
        return packed_bonus;
    }

    bool TryIncrementClk16LowLevel(size_t idx, uint16_t delta, packed64_t& out_new)
    {
        static_assert(MODE == PackedMode::MODE_VALUE32, "TryIncrementClk16LowLevel is only valid for MODE_VALUE32");\
        if (!IfIdxValid(idx))
        {
            return false;
        }
        packed64_t oldv = BackingPtr[idx].load(MoLoad_);
        while (true)
        {

            val32_t v = PackedCell64_t::ExtractValue32(oldv);
            clk16_t clk16 = PackedCell64_t::ExtractClk16(oldv);
            clk16_t n_clk16 = static_cast<clk16_t>(clk16 + delta);
            strl16_t n_strl = MakeSTRL4_t(DEFAULT_INTERNAL_PRIORITY, ST_PUBLISHED, REL_NONE, REL_NONE);

            packed64_t desired = PackedCell64_t::ComposeValue32x_64(v, n_clk16, n_strl);
            packed64_t expect = oldv;
            if (BackingPtr[idx].compare_exchange_strong(expect, desired, EXsuccess_, EXfailure_))
            {
                if (n_clk16 < clk16 && RegionSize_)
                {
                    size_t r = idx / RegionSize_;
                    std::atomic_ref<packed64_t>aref(RegionEpoch_[r]);
                    aref.fetch_add(1, std::memory_order_acq_rel);
                }
                BackingPtr[idx].notify_all();
                out_new = desired;
                return true;
            }
            oldv = expect;
        }
    }

    bool WaitForSlotChange(size_t idx, packed64_t packed, packed64_t expected, int timeout_ms = -1)
    {
        if (!IfIdxValid(idx))
        {
            return false;
        }
        if (timeout_ms < 0)
        {
            BackingPtr[idx].wait(expected);
            return true;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            packed64_t cur = BackingPtr[idx].load(MoLoad_);
            if (cur != expected)
            {
                return true;
            }
            BackingPtr[idx].wait(expected);
        }
        return false;
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
                packed64_t cur = BackingPtr[idx].load(MoLoad_);
                if (cur != observed)
                {
                    break;
                }
            }
            return;
        }
        else if (decision.Action == PAct::PARK_FOR_US)
        {
            BackingPtr[idx].wait(observed);
            return;
        }
        else
        {
            BackingPtr[idx].wait(observed);
            return;
        }
    }

    void UpdateRegionRelForIndex(size_t idx, tag8_t relbyte) noexcept
    {
        if (RegionSize_ == 0)
        {
            return;
        }
        size_t r = idx / RegionSize_;
        tag8_t mask5 = PackedCell64_t::RelMaskBSetFromRelation(relbyte);
        std::atomic_ref<tag8_t>aref8(RegionRel_[r]);
        aref8.fetch_or(static_cast<tag8_t>(mask5), std::memory_order_acq_rel);
        size_t w = r / MAX_VAL;
        size_t b = r % MAX_VAL;
        uint64_t mask = (1ull << b);
        for (unsigned i = 0; i < LN_OF_BYTE_IN_BITS; i++)
        {
            if (mask5 & (1u << i))
            {
                std::atomic_ref<packed64_t>aref(RelBitmaps_[i][w]);
                aref.fetch_or(mask, std::memory_order_acq_rel);
            }
        }
    }


    void SetEncodeOffset(size_t idx, packed64_t offset) noexcept
    {
        if (!IfIdxValid(idx))
        {
            return;
        }
        std::atomic_ref<packed64_t> aref(RelOffSet_[idx]);
        aref.store(offset, MoStoreSeq_);
    }

    packed64_t GetEncodedOffset(size_t idx) const noexcept
    {
        if (!IfIdxValid(idx))
        {
            return 0;
        }
        std::atomic_ref<const packed64_t> aref(RelOffSet_[idx]);
        return aref.load(MoLoad_);
        
    }

    void InitRegionIndex(size_t region_size)
    {
        if (!IfAnyValid_())
        {
            throw std::runtime_error("DSA not initialized");
        }
        if (region_size == 0)
        {
            throw std::invalid_argument("region_size == 0");
        }
        RegionSize_ = region_size;
        NumRegion_ = ((Capacity_ + RegionSize_ - 1) / RegionSize_);
        InitRelRegionAndBitmap_();
        for (size_t r = 0; r < NumRegion_; r++)
        {
            size_t base = r * RegionSize_;
            size_t end = min(Capacity_, base + RegionSize_);
            uint8_t accum = 0;
            for (size_t i = base; i < end; i++)
            {
                packed64_t p = BackingPtr[i].load(MoLoad_);
                tag8_t relbyte = static_cast<tag8_t>(PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(p)));
                accum |= PackedCell64_t::RelMaskBSetFromRelation(relbyte);
            }
            RegionRel_[r] = accum;
            if (accum)
            {
                size_t w = r / MAX_VAL;
                size_t b = r % MAX_VAL;
                uint64_t mask = (1ull << b);
                for (unsigned bit = 0; bit < LN_OF_BYTE_IN_BITS; bit++)
                {
                    if (accum & (1u << bit))
                    {
                        std::atomic_ref<packed64_t> aref(RelBitmaps_[bit][w]);
                        aref.fetch_or(mask, std::memory_order_acq_rel);
                    }
                }
            }
        }
    }

    std::vector<std::pair<size_t, size_t>> ScanRelRanges(tag8_t rel_mas_low5) const noexcept
    {
        std::vector<std::pair<size_t, size_t>> out;
        if (!IfAnyValid_())
        {
            return out;
        }
        if (RegionSize_ == 0)
        {
            size_t i = 0;
            while (i < Capacity_)
            {
                packed64_t p = BackingPtr[i].load(MoLoad_);
                tag8_t relbyte = PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(p));
                if ((PackedCell64_t::RelMaskBSetFromRelation(relbyte) & rel_mas_low5) == 0)
                {
                    ++i;
                    continue;
                }
                size_t s = i++;
                while (i < Capacity_)
                {
                    packed64_t q = BackingPtr[i].load(MoLoad_);
                    if ((PackedCell64_t::RelMaskBSetFromRelation(PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(q))) & rel_mas_low5) == 0)
                    {
                        break;
                    }
                    ++i;
                }
                out.emplace_back(s, i - s);
            }
            return out;
        }
        
        size_t words = RelBitmaps_[0].size();
        std::vector<uint64_t> combined(words, 0ull);
        for (unsigned bit = 0; bit < LN_OF_BYTE_IN_BITS; bit++)
        {
            if (rel_mas_low5 &(1u << bit))
            {
                for (size_t w = 0; w < words; w++)
                {
                    combined[w] |= RelBitmaps_[bit][w];
                }
            }
        }

        for (size_t w = 0; w < words; w++)
        {
            uint64_t word = combined[w];
            while (word)
            {
                unsigned tz = static_cast<unsigned>(std::countr_zero(word));
                size_t region_idx = w * MAX_VAL + tz;
                if (region_idx >= NumRegion_)
                {
                    break;
                }
                size_t base = region_idx * RegionSize_;
                size_t end = min(Capacity_, base + RegionSize_);
                size_t i = base;
                while(i < end)
                {
                    packed64_t p = BackingPtr[i].load(MoLoad_);
                    strl16_t sr = PackedCell64_t::ExtractSTRL(p);
                    tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr);
                    if ((PackedCell64_t::RelMaskBSetFromRelation(relbyte) & rel_mas_low5) == 0)
                    {
                        ++i;
                        continue;
                    }
                    size_t s = i++;
                    while (i < end)
                    {
                        packed64_t q = BackingPtr[i].load(MoLoad_);
                        if ((PackedCell64_t::RelMaskBSetFromRelation(PackedCell64_t::RelationFromSTRL(PackedCell64_t::ExtractSTRL(q))) & rel_mas_low5) == 0)
                        {
                            break;
                        }
                        ++i;
                    }
                    out.emplace_back(s, i - s);
                }
                word &=  (word -1);
            }
        }
        return out;
    }

    void RebuildRegionBitmaps() noexcept
    {
        if (RegionSize_ == 0)
        {
            return;
        }
        for (unsigned bit = 0; bit < LN_OF_BYTE_IN_BITS; bit++)
        {
            for (auto& w : RelBitmaps_[bit])
            {
                std::atomic_ref<packed64_t>aref(w);
                aref.store(0ull, MoStoreSeq_);
            }
        }
    }

    std::vector<size_t> FindState(tag8_t st_filter) const noexcept
    {
        std::vector<size_t> out;
        if (!IfAnyValid_())
        {
            return out;
        }
        out.reserve(MAX_VAL);
        for (size_t i = 0; i < Capacity_; i++)
        {
            packed64_t p = BackingPtr[i].load(MoLoad_);
            tag8_t st = ExtractLocalityFromSTRL(PackedCell64_t::ExtractSTRL(p));
            if (st == st_filter)
            {
                out.push_back(i);
            }
        }
        return out;
    }

    size_t GetOccupancy() const noexcept
    {
        return Occupancy_.load(MoLoad_);
    }

};


}
