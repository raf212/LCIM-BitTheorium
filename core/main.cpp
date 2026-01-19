// main.cpp — updated: adds published_count, done_producing, diagnostics & safe termination
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <cassert>
#include <random>
#include <immintrin.h>
#include <atomic>
#include <iomanip>

#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp"

using namespace AtomicCScompact;

static inline void CpuRelaxHint() noexcept
{
#if defined(_MSC_VER)
    YieldProcessor();
#else
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

int main()
{
    static_assert(std::atomic<packed64_t>::is_always_lock_free, "Platform must provide 64bit lock freedom");
    if (!std::atomic<packed64_t>::is_always_lock_free) {
        std::cerr << "Error: atomic<packed64_t> not lock-free on this platform.\n";
        return 2;
    }

    const unsigned PRODUCE_CNT = 200;
    const unsigned CONSUMER_THREADS = 4;
    const unsigned PRODUCER_INTERVAL_MS = 5;
    const uint64_t PROCESSING_MS_MIN = 1;
    const uint64_t PROCESSING_MS_MAX = 4;

    std::atomic<packed64_t> slot;

    // initial idle packed value
    packed64_t initial = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
    slot.store(initial, MoStoreUnSeq_);
    slot.notify_all();

    AtomicAdaptiveBackoff::PCBCfg cfg;
    cfg.BaseUS = 50;
    cfg.SpinThresholUS = 300;
    cfg.MaxParkUS = 200000;
    cfg.Jitter = true;
    AtomicAdaptiveBackoff aback(cfg, PackedMode::MODE_VALUE32, Timer48());

    std::atomic<uint64_t> decision_spin{0}, decision_park{0}, decision_immediate_spin{0};
    std::atomic<uint64_t> claimed_count{0}, completed_count{0};
    std::atomic<uint64_t> published_count{0};
    std::atomic<bool> done_producing{false};

    std::mt19937_64 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> proc_dist((int)PROCESSING_MS_MIN, (int) PROCESSING_MS_MAX);

    // Producer: publish only when slot is IDLE. CAS from IDLE -> PUBLISHED.
    std::thread Producer([&]()
    {
        for (unsigned i = 0; i < PRODUCE_CNT; i++)
        {
            bool published = false;
            int publish_attempts = 0;
            while (!published)
            {
                packed64_t cur = slot.load(MoLoad_);
                strl16_t sr = PackedCell64_t::ExtractSTRL(cur);
                tag8_t st = PackedCell64_t::StateFromSTRL(sr);

                if (st == ST_IDLE)
                {
                    // stamp timestamp bits for VALUE32 mode
                    uint64_t now_ticks = aback.PublicTimer48.NowTicks();
                    uint64_t downshifted = (now_ticks >> cfg.DownShift);
                    clk16_t clk16 = static_cast<clk16_t>(downshifted & MaskBits(CLK_B16));
                    packed64_t publish = PackedCell64_t::PackV32x_64(static_cast<val32_t>(i), clk16, ST_PUBLISHED, REL_PAGE);

                    packed64_t expected = cur;
                    if (slot.compare_exchange_strong(expected, publish, EXsuccess_, EXfailure_))
                    {
                        slot.notify_all();
                        published = true;
                        published_count.fetch_add(1, std::memory_order_acq_rel);
                        // debug
                        std::cerr << "[P] published idx=" << i << " attempts=" << publish_attempts << " published_count="
                                  << published_count.load() << std::endl << std::flush;
                    } else {
                        // CAS failed - someone else changed slot; try again (loop)
                        publish_attempts++;
                        CpuRelaxHint();
                    }
                }
                else
                {
                    // not idle — wait for change to avoid busy-wait
                    packed64_t observed = cur;
                    // If std::atomic::wait supported, this blocks efficiently; else it's a no-op in some impls
                    slot.wait(observed);
                }
            } // end while(!published)

            std::this_thread::sleep_for(std::chrono::milliseconds(PRODUCER_INTERVAL_MS));
        } // produce loop

        // Mark done and wake everyone
        done_producing.store(true, std::memory_order_release);
        // final notify to wake any waiters
        slot.notify_all();
        std::cerr << "[P] done producing. published_count=" << published_count.load() << std::endl << std::flush;
    });

    // Consumer worker: claim PUBLISHED then process, commit COMPLETE and recycle to IDLE.
    auto ConsumerWorker = [&](unsigned id)
    {
        std::mt19937_64 lg((unsigned)id * ID_HASH_GOLDEN_CONST + (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        while (true)
        {
            // Termination condition: processed enough OR producer done AND no outstanding publishes
            uint64_t done_now = completed_count.load(MoLoad_);
            if (done_now >= PRODUCE_CNT) break;
            if (done_producing.load(std::memory_order_acquire) &&
                (published_count.load(std::memory_order_acquire) == done_now))
            {
                // Nothing outstanding and producer finished -> exit
                break;
            }

            packed64_t cur = slot.load(MoLoad_);
            strl16_t sr = PackedCell64_t::ExtractSTRL(cur);
            tag8_t st = PackedCell64_t::StateFromSTRL(sr);

            if (st == ST_PUBLISHED)
            {
                tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr);
                packed64_t desired = PackedCell64_t::SetSTRLInPacked(cur, PackedCell64_t::PackSTRL16x_t(ST_CLAIMED, relbyte));
                packed64_t expected = cur;
                if (slot.compare_exchange_weak(expected, desired, EXsuccess_, EXfailure_))
                {
                    // claimed successfully
                    claimed_count.fetch_add(1, MoStoreUnSeq_);

                    // Decide spin/park based on adaptive backoff
                    AtomicAdaptiveBackoff::PCBDecision decision = aback.DecideForSlot(desired);
                    if (decision.Action == AtomicAdaptiveBackoff::PCBAction::SPIN_IMMEDIATE)
                    {
                        decision_immediate_spin.fetch_add(1, MoStoreUnSeq_);
                        std::this_thread::sleep_for(std::chrono::milliseconds(proc_dist(lg)));
                    }
                    else if (decision.Action == AtomicAdaptiveBackoff::PCBAction::SPIN_FOR_US)
                    {
                        decision_spin.fetch_add(1, MoStoreUnSeq_);
                        auto start = std::chrono::steady_clock::now();
                        auto sug = std::chrono::microseconds(decision.SuggestedUs);
                        while (std::chrono::steady_clock::now() - start < sug)
                        {
                            CpuRelaxHint();
                            packed64_t cur2 = slot.load(MoLoad_);
                            strl16_t sr2 = PackedCell64_t::ExtractSTRL(cur2);
                            if (PackedCell64_t::StateFromSTRL(sr2) != ST_CLAIMED)
                            {
                                break;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(proc_dist(lg)));
                    }
                    else
                    {
                        decision_park.fetch_add(1, MoStoreUnSeq_);
                        // park (block) until change
                        packed64_t before = slot.load(MoLoad_);
                        slot.wait(before);
                        std::this_thread::sleep_for(std::chrono::milliseconds(proc_dist(lg)));
                    }

                    // After "processing", commit COMPLETE and then recycle to IDLE
                    uint64_t now_ticks = aback.PublicTimer48.NowTicks();
                    packed64_t commit = PackedCell64_t::PackCLK48x_64(now_ticks & MaskBits(CLK_B48), ST_COMPLETE, REL_PAGE);
                    slot.store(commit, MoStoreSeq_);
                    slot.notify_all();

                    aback.ObserveCompletation(desired, std::optional<uint64_t>(now_ticks));

                    // mark completed
                    completed_count.fetch_add(1, MoStoreSeq_);

                    // recycle the slot back to IDLE so producer can publish the next item
                    packed64_t idle = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
                    slot.store(idle, MoStoreSeq_);
                    slot.notify_all();

                    // debug
                    std::cerr << "[C" << std::setw(2) << id << "] processed -> completed_count=" << completed_count.load()
                              << " published=" << published_count.load() << " claimed=" << claimed_count.load()
                              << std::endl << std::flush;
                }
                else
                {
                    // CAS failed - someone else claimed or changed; relax lightly
                    CpuRelaxHint();
                }
            }
            else
            {
                // not published -> wait for slot change
                packed64_t observed = slot.load(MoLoad_);
                // if done_producing and observed is IDLE or COMPLETE with no outstanding work then allow exit
                slot.wait(observed);
            }
        } // end while

        // consumer exiting
        std::cerr << "[C exiting] thread done. completed_count=" << completed_count.load() << std::endl << std::flush;
    };

    // spawn consumers
    std::vector<std::thread> ConsumersThis;
    for (unsigned i = 0; i < CONSUMER_THREADS; i++)
    {
        ConsumersThis.emplace_back(ConsumerWorker, i+1);
    }

    // watchdog thread (diagnostics) — prints if system stalls > 2s
    std::atomic<bool> stop_watch{false};
    std::thread Watchdog([&]() {
        using clock = std::chrono::steady_clock;
        auto last_progress_t = clock::now();
        uint64_t last_completed = completed_count.load();
        while (!stop_watch.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t cur_completed = completed_count.load();
            if (cur_completed != last_completed) {
                last_progress_t = clock::now();
                last_completed = cur_completed;
                continue;
            }
            auto now = clock::now();
            if (now - last_progress_t > std::chrono::seconds(2)) {
                // print diagnostic
                packed64_t cur = slot.load(MoLoad_);
                strl16_t sr = PackedCell64_t::ExtractSTRL(cur);
                tag8_t st = PackedCell64_t::StateFromSTRL(sr);
                tag8_t rel = PackedCell64_t::RelationFromSTRL(sr);
                std::cerr << "*** WATCHDOG: no progress for 2s ***\n"
                          << " completed=" << completed_count.load()
                          << " published=" << published_count.load()
                          << " claimed=" << claimed_count.load()
                          << " slot-state=0x" << std::hex << static_cast<int>(st) << std::dec
                          << " slot-rel=0x" << std::hex << static_cast<int>(rel) << std::dec << "\n"
                          << " done_producing=" << done_producing.load() << std::endl << std::flush;
                // refresh timer to avoid spamming
                last_progress_t = now;
            }
        }
    });

    // join
    Producer.join();
    for (auto& t: ConsumersThis) t.join();
    // stop watchdog
    stop_watch.store(true);
    Watchdog.join();

    std::cout << "Test Finished\n";
    std::cout << "Producer Count : " << PRODUCE_CNT << "\n";
    std::cout << "Published count : " << published_count.load() << "\n";
    std::cout << "Claimed count : " << claimed_count.load() << "\n";
    std::cout << "Completed : " << completed_count.load() << "\n";
    std::cout << "Decision : SPIN_IMEDIATE = " << decision_immediate_spin.load()
              << " Spin = " << decision_spin.load()
              << " Park = " << decision_park.load() << "\n";
    std::cout << "EMA mean Ticks(if any) : ";
    if (auto m = aback.EmaEstAPC().MeanTicks(); m.has_value())
    {
        std::cout << m.value() << " ticks\n";
    }
    else
    {
        std::cout << "N/A\n";
    }

    return 0;
}
