
#pragma once

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

#include "PackedCell.hpp"
#include "PackedStRel.h"
#include <latch> // For std::latch (C++20)

namespace AtomicCScompact
{
#define A_BILLION 1000000000ull
#define THRESHHOLD_64BIT 1e-12
struct Timer48
{
    uint64_t TicksPerSec_ = A_BILLION;

    inline uint64_t NowTicks() const noexcept
    {
        using  cns = std::chrono::nanoseconds;
        auto d = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t ns_count = static_cast<uint64_t>(std::chrono::duration_cast<cns>(d).count());
        return ns_count & MaskBits(CLK_B48);
    }
};


class EMAEstimatorAPC
{
public :
    struct EMAConfig 
    {
        double EMAAlpha = 0.08;
        unsigned EMAMinSamples = 9;
    };
private :
    EMAConfig EMACfg_;
    std::atomic<double>EMATicks_;
    std::atomic<uint64_t> Samples_;

public :
    EMAEstimatorAPC() noexcept :
        EMAEstimatorAPC(EMAConfig())
    {}
    explicit EMAEstimatorAPC(const EMAConfig& cfg) noexcept :
        EMACfg_(cfg), EMATicks_(0.0), Samples_(0)
    {}
    void ObserveTicks(uint64_t ticks) noexcept
    {
        Samples_.fetch_add(1, std::memory_order_relaxed);
        double sample = static_cast<double>(ticks);
        double oldv = EMATicks_.load(MoLoad_);
        while(true)
        {
            double newv = (1 - EMACfg_.EMAAlpha) * oldv + (EMACfg_.EMAAlpha * sample);
            if (oldv == 0)
            {
                newv = sample;
            }
            if (EMATicks_.compare_exchange_weak(oldv, newv, EXsuccess_, EXfailure_))
            {
                break;
            }
        }
    }

    std::optional<double>MeanTicks() const noexcept
    {
        auto s = Samples_.load(MoLoad_);
        if (s < EMACfg_.EMAMinSamples)
        {
            return std::nullopt;
        }
        return EMATicks_.load(MoLoad_);
    }

    std::optional<double>HazardPerSec(const Timer48& timer48) const noexcept
    {
        auto m = MeanTicks();
        if (!m.has_value())
        {
            return std::nullopt;
        }
        double mean_time = m.value();
        if (mean_time <= THRESHHOLD_64BIT)
        {
            return std::optional<double>(std::numeric_limits<double>::infinity());
        }
        return std::optional<double>(static_cast<double>(timer48.TicksPerSec_) / mean_time);
    }

    void ResetEMA() noexcept
    {
        EMATicks_.store(0.0, MoStoreUnSeq_);
        Samples_.store(0, MoStoreUnSeq_);
    }

};

class HazardEstimatorPC
{
public:
    struct HECfg
    {
        uint16_t BaseBinUS = 1;
        uint16_t BinCount = 32;
        uint16_t MinMass = 8;
        double HazardAlpha = 0.08;    
    };
public:
    static inline constexpr uint8_t MIN_BIN_COUNT = 8;
    static inline constexpr uint8_t MAX_BIN_COUNT = 64;

    HazardEstimatorPC() noexcept :
        HazardEstimatorPC(HECfg())
    {}

    explicit HazardEstimatorPC(const HECfg& cfg) noexcept :
        HeCfg_(cfg), HBins_(cfg.BinCount, 0.0), HMassEMA_(0.0), HSamples_(0)
    {
        assert(cfg.BinCount > MIN_BIN_COUNT && cfg.BinCount < MAX_BIN_COUNT);
        uint64_t ub = cfg.BaseBinUS;
        for (unsigned i = 0; i < cfg.BinCount; i++)
        {
            HBNUpperUS_.push_back(ub);
            ub <<= 1;
        }
    }
    void ObserveUS(uint64_t age_Us) noexcept
    {
        ++HSamples_;
        if (HMassEMA_ == 0.0)
        {
            HMassEMA_ = 1.0;
        }
        else
        {
            HMassEMA_ = (1.0 - HeCfg_.HazardAlpha) * HMassEMA_ + HeCfg_.HazardAlpha * 1.0;
        }
        unsigned bin_index = BinIndexForUS_(age_Us);
        HBins_[bin_index] = (1.0 - HeCfg_.HazardAlpha) * HBins_[bin_index] + HeCfg_.HazardAlpha * 1.0;
    }

    std::optional<double>ProbHazardAtUS(uint64_t age_us) const noexcept
    {
        if (HMassEMA_ < static_cast<double>(HeCfg_.MinMass))
        {
            return std::nullopt;
        }
        unsigned idx = BinIndexForUS_(age_us);
        double tot = 0.0;
        for (double v : HBins_)
        {
            tot += v;
        }
        if (tot < 0.0)
        {
            return std::nullopt;
        }
        double f = HBins_[idx] / (tot + THRESHHOLD_64BIT);
        uint64_t bw = BinWidthForIndex_(idx);
        if (bw == 0)
        {
            return std::nullopt;
        }
        double f_per_sec = (f / static_cast<double>(bw)) * 1e6;
        double cum = 0.0;
        for (unsigned i = 0; i < idx; i++)
        {
            cum += HBins_[i];
        }
        double s_hat = 1.0 - (cum / (tot + THRESHHOLD_64BIT));
        if (s_hat <= THRESHHOLD_64BIT)
        {
            return std::optional<double>(0.0);
        }
        double hazard = f_per_sec / s_hat;
        if (!std::isfinite(hazard))
        {
            return std::optional<double>(0.0);
        }
        
        return hazard;
    }
private :
    HECfg HeCfg_;
    double HMassEMA_;
    std::vector<double> HBins_;
    std::vector<uint64_t> HBNUpperUS_;
    uint64_t HSamples_;

    inline unsigned BinIndexForUS_(uint64_t tus) const noexcept
    {
        unsigned i = 0;
        while(i + 1 < HBNUpperUS_.size() && tus >= HBNUpperUS_[i]) ++i;
        return i;
    }

    inline uint64_t BinWidthForIndex_(unsigned idx) const noexcept
    {
        if (idx == 0)
        {
            return HBNUpperUS_[0];
        }
        return HBNUpperUS_[idx] - HBNUpperUS_[idx -1];
    }
};

class AtomicAdaptiveBackoff
{
public:
    Timer48 PublicTimer48;
    struct PCBCfg
    {
        unsigned DownShift = 10u;
        uint64_t BaseUS = 50;
        uint64_t SpinThresholUS = 200;
        uint64_t MaxParkUS = 200000;
        double CostSpinPerSec = 1.0;
        double CostPark = 1000.0;
        double PriorityGama = 0.15;
        EMAEstimatorAPC::EMAConfig EMACfg{};
        HazardEstimatorPC::HECfg HazardCfg{};
        bool Jitter = true;
    };
    enum class PCBAction : uint8_t
    {
        SPIN_IMMEDIATE = 0, SPIN_FOR_US = 1,
        PARK_FOR_US = 2, BLOCK_WAIT = 3
    };
    struct PCBDecision
    {
        PCBAction Action;
        uint64_t SuggestedUs;
        double EstHazPerSec;
    };
private:
    PCBCfg Cfg_;
    PackedMode PCMode_;
    EMAEstimatorAPC Ema_;
    HazardEstimatorPC Hist_;
    mutable std::mt19937_64 Rng_;
    double C_OVER_P_{0.0};

    static inline uint64_t JitterUS_(uint64_t base) noexcept
    {
        thread_local static std::mt19937_64 t_rand((std::random_device())());
        if (base <= 2)
        {
            return base;
        }
        uint64_t range = std::min<uint64_t>(base / 4u, 16384u); // why this 2 number ?
        std::uniform_int_distribution<uint64_t> dist(0, range);
        int64_t center = static_cast<int64_t>(range / 2);
        int64_t off_center = static_cast<int64_t>(dist(t_rand)) - center;
        int64_t v = static_cast<int64_t>(base) + off_center;
        if (v < 1)
        {
            v = 1;
        }
        return v;
    }
    void Recall_C_Over_p_() noexcept
    {
        if (Cfg_.CostPark <= 0.0)
        {
            C_OVER_P_ = std::numeric_limits<double>::infinity();
        }
        else
        {
            C_OVER_P_ = Cfg_.CostSpinPerSec / Cfg_.CostPark;;
        }
    }
    inline uint64_t ReconstructPublishTicks_(uint64_t now_ticks, packed64_t packed) const noexcept
    {
        if (PCMode_ == PackedMode::MODE_CLKVAL48)
        {
            return (PackedCell64_t::ExtractClk48(packed) & MaskBits(CLK_B48));
        }
        else
        {
            clk16_t stored = PackedCell64_t::ExtractClk16(packed);
            unsigned ds = Cfg_.DownShift;
            uint64_t now_down = (now_ticks >> ds) & MaskBits(TOTAL_LOW);
            uint64_t candidate = (((now_down & ~uint64_t(0xFFFFu)) | (static_cast<uint64_t>(stored))));
            if (candidate > now_down)
            {
                candidate -= (1ull << 16); //why?
            }
            uint64_t pub_ticks = (candidate << ds) & MaskBits(TOTAL_LOW);
            return pub_ticks;
        }
        
    }
    inline uint64_t ReconstructPublishTicks_(packed64_t p) const noexcept
    {
        uint64_t now = PublicTimer48.NowTicks();
        return ReconstructPublishTicks_(now, p);
    }
    
    inline double FallbackHazard_(uint64_t age_ticks) const noexcept
    {
        uint64_t age_us = age_ticks / 1000u; // ticks to micro sec
        double age_s = static_cast<double>(age_us) / 1e6;
        if (age_s <= 0.0)
        {
            return 1e6;
        }
        return 1.0 / age_s;
    }
    /* data */
public:
    AtomicAdaptiveBackoff() noexcept :
        AtomicAdaptiveBackoff(PCBCfg())
    {}
    explicit AtomicAdaptiveBackoff
                (
                    const PCBCfg& cfg,
                    PackedMode mode = PackedMode::MODE_VALUE32,
                    Timer48 timer = Timer48()
                ) :
        PublicTimer48(timer), Cfg_(cfg), PCMode_(mode), Ema_(cfg.EMACfg),
        Hist_(cfg.HazardCfg), Rng_(std::random_device{}())
    {
        Recall_C_Over_p_();
    }
    ~AtomicAdaptiveBackoff() = default;

    void ObserveCompletation(packed64_t pub_p, std::optional<uint64_t>observe_time_ticks = std::nullopt) noexcept
    {
        uint64_t now = observe_time_ticks.value_or(PublicTimer48.NowTicks());
        uint64_t pub_ticks = ReconstructPublishTicks_(now, pub_p);
        uint64_t age_ticks = (now - pub_ticks) & MaskBits(TOTAL_LOW);
        uint64_t age_us = age_ticks / 1000u; //micro sec conver
        Ema_.ObserveTicks(age_ticks);
        Hist_.ObserveUS(age_us);
    }

    PCBDecision DecideForSlot(packed64_t slot_payload, std::optional<uint64_t>now_ticks_opt = std::nullopt) const noexcept
    {
        uint64_t now = now_ticks_opt.value_or(PublicTimer48.NowTicks());
        strl16_t sr = PackedCell64_t::ExtractSTRL(slot_payload);
        tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr);
        int8_t priority = static_cast<int8_t>(PackedCell64_t::PriorityFromRelation(relbyte));
        uint64_t pub_ticks = ReconstructPublishTicks_(now, slot_payload);
        uint64_t age_ticks = (now - pub_ticks) & MaskBits(TOTAL_LOW);
        uint64_t age_us = age_ticks / 1000u;
        std::optional<double> hazard_hist = Hist_.ProbHazardAtUS(age_us);
        double hazard = 0.0;
        if (hazard_hist.has_value())
        {
            hazard = hazard_hist.value();
        }
        else
        {
            auto hema = Ema_.HazardPerSec(PublicTimer48);
            if (hema.has_value())
            {
                hazard = hema.value();
            }
            else
            {
                hazard = FallbackHazard_(age_ticks);
            }
        }
        double priority_scale = 1.0 + Cfg_.PriorityGama * static_cast<double>(priority);
        double effective_threshold = C_OVER_P_ / priority_scale;
        PCBDecision dec{};
        dec.EstHazPerSec = hazard;
        bool should_spin = (hazard > effective_threshold);
        if (should_spin)
        {
            double expected_s = 0;
            if (hazard > 0.0)
            {
                expected_s = 1.0 / hazard;
            }
            uint64_t expect_us = static_cast<uint64_t>(std::fmax(1.0, expected_s * 1e6));
            uint64_t spin_us = std::min<uint64_t>(expect_us, Cfg_.SpinThresholUS);
            if (Cfg_.Jitter && spin_us > 1)
            {
                spin_us = JitterUS_(spin_us);
            }
            if (spin_us <= 1)
            {
                dec.Action = PCBAction::SPIN_IMMEDIATE;
                dec.SuggestedUs = 0;
            }
            else
            {
                dec.Action = PCBAction::SPIN_FOR_US;
                dec.SuggestedUs = spin_us;
            }
            return dec;
        }
        else
        {
            uint64_t park_us = Cfg_.BaseUS;
            auto mean_opt = Ema_.MeanTicks();
            if (mean_opt.has_value())
            {
                uint64_t mean_us = static_cast<uint64_t>(mean_opt.value()) / 1000u;
                park_us = std::min<uint64_t>(mean_us, Cfg_.MaxParkUS);
            }
            else
            {
                double s = 0.0;
                if (age_us > 0)
                {
                    s = std::log2(static_cast<double>(age_us) + 1);
                }
                uint64_t exp = Cfg_.BaseUS * (1ull << std::min<unsigned>(30u, static_cast<unsigned>(std::floor(s))));
                if (exp == 0)
                {
                    exp = Cfg_.BaseUS;
                }
                park_us = std::min<uint64_t>(exp, Cfg_.MaxParkUS);
            }
            if (Cfg_.Jitter && park_us > 8)
            {
                park_us = JitterUS_(park_us);
            }
            if (park_us <= Cfg_.SpinThresholUS)
            {
                dec.Action = PCBAction::PARK_FOR_US;
                dec.SuggestedUs = park_us;
            }
            else
            {
                dec.Action = PCBAction::PARK_FOR_US;
                dec.SuggestedUs = park_us;
            }
            return dec;
        }
    }

    const EMAEstimatorAPC& EmaEstAPC() const noexcept
    {
        return Ema_;
    }
    const HazardEstimatorPC& HazEstPC() const noexcept
    {
        return Hist_;
    }

    void SetCost(double spin_cost_per_second, double static_park_cost) noexcept
    {
        Cfg_.CostSpinPerSec = spin_cost_per_second;
        Cfg_.CostPark = static_park_cost;
        Recall_C_Over_p_();
    }
    
};



} // namespace name
