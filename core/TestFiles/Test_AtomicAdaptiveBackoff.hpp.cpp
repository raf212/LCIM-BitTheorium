#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <cassert>
#include <random>
#include <mutex>
#include <iomanip>

#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp"


using namespace AtomicCScompact;
static std::mutex Logmu;
static inline void LogPrint(const std::string& s)
{
    std::lock_guard<std::mutex>g(Logmu);
    std::cerr << s << std::flush;
}



static const char* PCBActionName(AtomicAdaptiveBackoff::PCBAction action)
{
    switch(action)
    {
        case AtomicAdaptiveBackoff::PCBAction::SPIN_IMMEDIATE : return "SPIN_IMMEDIATE";
        case AtomicAdaptiveBackoff::PCBAction::SPIN_FOR_US : return "SPIN_FOR_US";
        case AtomicAdaptiveBackoff::PCBAction::PARK_FOR_US : return "PARK_FOR_US";
        case AtomicAdaptiveBackoff::PCBAction::BLOCK_WAIT : return "BLOCK_WAIT";
        default: return "UNKNOWN";
    }
}

int main()
{
    static_assert(std::atomic<packed64_t>::is_always_lock_free, "Platform must provide 64-bit lock-free atomics");
    if (!std::atomic<packed64_t>::is_always_lock_free) {
        std::cerr << "Error: atomic<packed64_t> not lock-free on this platform.\n";
        return 2;
    }

    //config
    const unsigned PRODUCE_CNT = 200;
    const unsigned CONSUMER_THREADS = 4;
    const unsigned PRODUCER_INTERVAL_MS = 2;
    const uint64_t PROCESSING_MS_MIN = 1;
    const uint64_t PROCESSING_MS_MAX = 4;

    //shared-signals
    std::atomic<packed64_t>slot;
    packed64_t idle = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
    slot.store(idle, MoStoreUnSeq_); // std::memory_order_release - MoStoreSeq_ might me better option ? 
    slot.notify_all();

    //ADAPTIVE BACKOFF INSTANCES
    AtomicAdaptiveBackoff::PCBCfg cfg;
    cfg.BaseUS = 50;
    cfg.SpinThresholUS = 300;
    //adaptors
    AtomicAdaptiveBackoff abac_v32(cfg, PackedMode::MODE_VALUE32, Timer48());
    AtomicAdaptiveBackoff abac_clk48(cfg, PackedMode::MODE_CLKVAL48, Timer48());

    //counters
    std::atomic<uint64_t> published_count{0}, claimed_count{0}, completed_count{0};
    std::atomic<bool> done_producing{false};

    //utlity-RNG??
    std::mt19937_64 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int>proc_dist((int)PROCESSING_MS_MIN, (int)PROCESSING_MS_MAX);

    //producer thread
    std::thread producer([&]{
        for (unsigned i = 0; i < PRODUCE_CNT; i++)
        {
            bool published = false;
            int attempts = 0;
            while (!published)
            {
                packed64_t cur = slot.load(MoLoad_);
                strl16_t sr = PackedCell64_t::ExtractSTRL(cur);
                tag8_t st = ExtractLocalityFromSTRL(sr);
                if (st == ST_IDLE)
                {
                    uint64_t now_ticks = abac_v32.PublicTimer48.NowTicks();
                    uint64_t down = (now_ticks >> cfg.DownShift);
                    clk16_t clk16 = static_cast<clk16_t>(down & MaskBits(CLK_B16));
                    packed64_t publish = PackedCell64_t::PackV32x_64(static_cast<val32_t>(i), clk16, ST_PUBLISHED, REL_PAGE);
                    packed64_t expected = cur;
                    if (slot.compare_exchange_strong(expected, publish, EXsuccess_, EXfailure_))
                    {
                        slot.notify_all(); // thundaring thread problem ?? prefered -> slot.notify_one()
                        published_count.fetch_add(1, std::memory_order_acq_rel);
                        {
                            std::ostringstream oss;
                            oss << "[P] Published IDX = " << i << "Attempts = " << attempts 
                            << "Published Count = " << published_count.load() << "\n";
                            LogPrint(oss.str()); 
                        }
                        published = true;
                    }
                    else
                    {
                        ++attempts;
                        CpuRelaxHint();
                    }
                }
                else
                {
                    packed64_t observed = cur;
                    slot.wait(observed);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(PRODUCER_INTERVAL_MS));
        }
        done_producing.store(true, MoStoreSeq_);
        slot.notify_all();
        LogPrint("[P] Done Producing.\n");
    });

    auto ConsumerWorker = [&](unsigned id)
    {
        uint64_t seed = (uint64_t(id) * uint64_t(ID_HASH_GOLDEN_CONST))^
                        (uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::mt19937_64 lg(seed);
        while(true)
        {
            uint64_t done_now = completed_count.load(MoLoad_);
            if (done_now >= PRODUCE_CNT)
            {
                break;
            }
            if (done_producing.load(MoLoad_) && (published_count.load(MoLoad_) == done_now))
            {
                break;
            }
            packed64_t cur = slot.load(MoLoad_);
            strl16_t sr = PackedCell64_t::ExtractSTRL(cur);
            tag8_t st = ExtractLocalityFromSTRL(sr);
            if (st == ST_PUBLISHED)
            {
                packed64_t desired = PackedCell64_t::SetLocalityInPacked(cur, ST_PUBLISHED);
                packed64_t expected  = cur;
                if (slot.compare_exchange_weak(expected, desired, EXsuccess_, EXfailure_))
                {
                    claimed_count.fetch_add(1, MoStoreUnSeq_);
                    AtomicAdaptiveBackoff::PCBDecision decision = abac_v32.DecideForSlot(desired);
                    {
                        std::ostringstream oss ;
                        oss << "[C" << id << "] Decide = " << PCBActionName(decision.Action) 
                        << " Suggested(US) = " << decision.SuggestedUs << "Hazard = " << decision.EstHazPerSec << "\n";
                        LogPrint(oss.str());
                    }
                    if (decision.Action == AtomicAdaptiveBackoff::PCBAction::SPIN_IMMEDIATE)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(proc_dist(lg)));
                    }
                    else if (decision.Action == AtomicAdaptiveBackoff::PCBAction::SPIN_FOR_US)
                    {
                        auto start = std::chrono::steady_clock::now();
                        auto suggested = std::chrono::microseconds(decision.SuggestedUs);
                        while ((std::chrono::steady_clock::now() - start) < suggested)
                        {
                            CpuRelaxHint();
                            packed64_t cur2 = slot.load(MoLoad_);
                            strl16_t sr2 = PackedCell64_t::ExtractSTRL(cur2);
                            if (ExtractLocalityFromSTRL(sr2) != ST_CLAIMED)
                            {
                                break;
                            }
                        }
                    }
                    else
                    {
                        packed64_t before = slot.load(MoLoad_);
                        slot.wait(before);
                        std::this_thread::sleep_for(std::chrono::milliseconds(proc_dist(lg)));
                    }
                    uint64_t now_ticks = abac_v32.PublicTimer48.NowTicks();
                    packed64_t commit = PackedCell64_t::PackCLK48x_64((now_ticks & MaskBits(CLK_B48)), ST_COMPLETE, REL_PAGE);
                    slot.store(commit, MoStoreSeq_);
                    slot.notify_all();

                    abac_v32.ObserveCompletation(desired, std::optional<uint64_t>(now_ticks));
                    completed_count.fetch_add(1, MoStoreSeq_);

                    packed64_t new_idle = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
                    slot.store(new_idle, MoStoreSeq_);
                    slot.notify_all();
                    {
                        std::ostringstream oss;
                        oss << "[C" << id << "] Processed-> Compleated Count = " << completed_count.load()
                        << "Published = " << published_count.load() << " Claimed = " << claimed_count.load() << "\n";
                        LogPrint(oss.str());
                    }
                }
                else
                {
                    CpuRelaxHint();
                }
            }
            else
            {
                packed64_t observed = slot.load(MoLoad_);
                slot.wait(observed);
            }
        }
        {
            std::ostringstream oss;
            oss << "[C" << id << "-> exiting] Compleated Count = " << completed_count.load() << "\n";
            LogPrint(oss.str());
        }
    };

    //spawn consumers
    std::vector<std::thread>consumers;
    for (unsigned i = 0; i < CONSUMER_THREADS; i++)
    {
        consumers.emplace_back(ConsumerWorker, (i + 1));
    }
    
    //watchdog
    std::atomic<bool>stop_watch{false};
    std::thread watchdog([&]{
        auto last_progress = std::chrono::steady_clock::now();
        uint64_t last_compleated = completed_count.load();
        while (!stop_watch.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t cur_completed = completed_count.load();
            if (cur_completed != last_compleated)
            {
                last_compleated = cur_completed;
                last_progress = std::chrono::steady_clock::now();
                continue;
            }
            if ((std::chrono::steady_clock::now() - last_progress) > std::chrono::seconds(2))
            {
                packed64_t cur = slot.load(MoLoad_);
                strl16_t sr = PackedCell64_t::ExtractSTRL(cur);
                tag8_t st = ExtractLocalityFromSTRL(sr);
                tag8_t rel = PackedCell64_t::ExtractFullRelFromPacked(sr);
                std::ostringstream oss;
                oss << "*** WATCHDOG: no progress 2s -- completed=" << completed_count.load()
                    << " published=" << published_count.load() << " claimed=" << claimed_count.load()
                    << " slot-state=0x" << std::hex << static_cast<int>(st) << std::dec
                    << " slot-rel=0x" << std::hex << static_cast<int>(rel) << std::dec << "\n";            
                LogPrint(oss.str());
                last_progress = std::chrono::steady_clock::now();
            }
            if (done_producing.load(MoLoad_) && (completed_count.load(MoLoad_) == published_count.load(MoLoad_)))
            {
                break;
            }
        }
        
    });

    producer.join();
    for (auto& t : consumers)
    {
        t.join();
    }
    stop_watch.store(true);
    watchdog.join();

    //validation
    {
        std::ostringstream oss;
        oss << "\n Functional Single Slot Test Finished\n"
            << "Producer : " << PRODUCE_CNT << "\n"
            << "Published Count : " << published_count.load() << "\n"
            << "Claimed Count : " << claimed_count.load() << "\n"
            << "Completed Count : " << completed_count.load() << "\n\n";
        LogPrint(oss.str());
    }
    assert(published_count.load() == completed_count.load() && "Published != Compleated :: BUG Detected");

    LogPrint("====ABA Vurnability Test(demonstration)====\n");

    //show native ABA
    {
        packed64_t A = PackedCell64_t::PackV32x_64(0xAAu, clk16_t(0x1234), ST_PUBLISHED, REL_SELF);
        slot.store(A, MoStoreSeq_);
        slot.notify_all();

        //consumer threads reads
        std::atomic<bool> consumer_done{false};
        std::atomic<bool> swap_done{false};
        std::thread consumer([&]{
            packed64_t expected = slot.load(MoLoad_);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            packed64_t desired = PackedCell64_t::SetLocalityInPacked(expected, ST_CLAIMED);
            bool ok = slot.compare_exchange_strong(expected, desired, EXsuccess_, EXfailure_);
            std::ostringstream oss;
            oss << "[ABA-NATIVE] Consumer CAS Result : " << (ok ? "SUCCESS (ABA EXPLOITED)" : "FAILED ->To Exploit") << "\n";
            LogPrint(oss.str());
            consumer_done.store(true);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        packed64_t B = PackedCell64_t::PackV32x_64(0xBBu, clk16_t(0x2222), ST_PUBLISHED, REL_SELF);
        slot.store(B, MoStoreSeq_);
        slot.notify_all();
        slot.store(A, MoStoreSeq_);
        slot.notify_all();
        swap_done.store(true);
        consumer.join();
    }

    LogPrint("=== ADAPTIVE BACKOFF DECISION MODES--(SIMULATED)--===\n\n");
    auto print_decision = [&](AtomicAdaptiveBackoff &ab, packed64_t payload, const std::string& label)
    {
        AtomicAdaptiveBackoff::PCBDecision d = ab.DecideForSlot(payload);
        std::ostringstream oss;
        oss << "[" << label << "] Decision = " << PCBActionName(d.Action)
            << " Suggested microsecond = " << d.SuggestedUs << " Hazard = " << d.EstHazPerSec << "\n";
        LogPrint(oss.str());
    };

    uint64_t now = abac_clk48.PublicTimer48.NowTicks();
    //simulate short ages -> Expect:SPIN_IMMIDIATE
    {
        packed64_t payload = PackedCell64_t::PackCLK48x_64(((now - 1000) & MaskBits(CLK_B48)), ST_PUBLISHED, REL_PAGE);
        for (int i = 0; i < 200; i++)
        {
            uint64_t observed_now = ((now - 500) + (i & 0xFF));
            abac_clk48.ObserveCompletation(payload, std::optional<uint64_t>(observed_now));
        }
        print_decision(abac_clk48, payload, "FAST");
    }
    // Modarate AGE -> Expect::SPIN_FOR_US
    {
        packed64_t payload = PackedCell64_t::PackCLK48x_64(((now - 2000000) & MaskBits(CLK_B48)), ST_PUBLISHED, REL_PAGE);
        for (int i = 0; i < 200; i++)
        {
            uint64_t observed_now = (now - 2000000) + 100 + (i & 0xFF); //modarate age
            abac_clk48.ObserveCompletation(payload, std::optional<uint64_t>(observed_now));
        }
        print_decision(abac_clk48, payload, "MODARATE");
    }

    //very large ages -> EXPECTED:: PARK_FOR_US
    {
        packed64_t payload = PackedCell64_t::PackCLK48x_64(((now - (uint64_t)1e9) & MaskBits(CLK_B48)), ST_PUBLISHED, REL_PAGE);
        for (int i = 0; i < 200; i++)
        {
            uint64_t observed_now = (now - (uint64_t)1e9) + 100000 + (i & 0xFFF); //Very Large Age
            abac_clk48.ObserveCompletation(payload, std::optional<uint64_t>(observed_now));
        }
        print_decision(abac_clk48, payload, "SLOW");
    }

    LogPrint("===Single Slot Test Done ===\n\n");
    return 0;
    
}