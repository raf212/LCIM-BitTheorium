#include <iostream>
#include<thread>
#include <vector>
#include <chrono>
#include <cassert>
#include <random>
#include <immintrin.h>



#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp"

using namespace AtomicCScompact;

static inline void CpuRelaxHint() noexcept
{
#if defined(_MSC_VER)
	YieldProcessor();
#else
	try
	{
		__asm__ __volatile__("pause" ::: "memory");
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		std::this_thread::yield();
	}
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

	packed64_t initial = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
	slot.store(initial, MoStoreUnSeq_);

	AtomicAdaptiveBackoff::PCBCfg cfg;
	cfg.BaseUS = 50;
	cfg.SpinThresholUS = 300;
	cfg.MaxParkUS = 200000;
	cfg.Jitter = true;
	AtomicAdaptiveBackoff aback(cfg, PackedMode::MODE_VALUE32, Timer48());

	std::atomic<uint64_t>decision_spin{0}, decision_park{0}, decision_immediate_spin{0};
	std::atomic<uint64_t>claimed_count{0}, completed_count{0};

	std::mt19937_64 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
	std::uniform_int_distribution<int>proc_dist((int)PROCESSING_MS_MIN, (int) PROCESSING_MS_MAX);

	std::thread Producer([&]()
	{
		for (unsigned i = 0; i < PRODUCE_CNT; i++)
		{
			uint64_t now_ticks = aback.PublicTimer48.NowTicks();
			uint64_t downshifted = (now_ticks >> cfg.DownShift);
			clk16_t clk16 = static_cast<clk16_t>(downshifted & MaskBits(CLK_B16));
			packed64_t publish = PackedCell64_t::PackV32x_64(static_cast<val32_t>(i), clk16, ST_PUBLISHED, REL_PAGE);
			slot.store(publish, MoStoreSeq_);
			slot.notify_all();
			std::this_thread::sleep_for(std::chrono::milliseconds(PRODUCER_INTERVAL_MS));
		}
	}
	);

	auto ConsumerWorker = [&](unsigned id)
	{
		std::mt19937_64 lg((unsigned)id * ID_HASH_GOLDEN_CONST + (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
		while (completed_count.load(MoLoad_) < PRODUCE_CNT)
		{
			packed64_t cur = slot.load(MoLoad_);
			strl16_t sr = PackedCell64_t::ExtractSTRL(cur);
			tag8_t st = PackedCell64_t::StateFromSTRL(sr);
			if (st == ST_PUBLISHED)
			{
				tag8_t relbyte = PackedCell64_t::RelationFromSTRL(sr);
				packed64_t desired = PackedCell64_t::SetSTRLInPacked(cur, PackedCell64_t::PackSTRL16x_t(ST_CLAIMED, relbyte));
				packed64_t expected = cur;
				if (slot.compare_exchange_weak(expected, desired, EXsuccess_, MoLoad_))
				{
					claimed_count.fetch_add(1, MoStoreUnSeq_);
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
						auto sug = std::chrono::milliseconds(decision.SuggestedUs ? decision.SuggestedUs : cfg.BaseUS);
						packed64_t before = slot.load(MoLoad_);
						slot.wait(before);
						std::this_thread::sleep_for(std::chrono::milliseconds(proc_dist(lg)));
					}
					uint64_t now_ticks = aback.PublicTimer48.NowTicks();
					packed64_t commit = PackedCell64_t::PackCLK48x_64(now_ticks & MaskBits(CLK_B48), ST_COMPLETE, REL_PAGE);
					slot.store(commit, MoStoreSeq_);
					slot.notify_all();
					aback.ObserveCompletation(desired, std::optional<uint64_t>(now_ticks));
					completed_count.fetch_add(1, MoStoreSeq_);
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
	};

	std::vector<std::thread> ConsumersThis;
	for (size_t i = 0; i < CONSUMER_THREADS; i++)
	{
		ConsumersThis.emplace_back(ConsumerWorker, i+1);
	}
	Producer.join();
	for (auto& t: ConsumersThis)
	{
		t.join();
	}

	std::cout << "Test Finished\n";
	std::cout << "Producer Count : " << PRODUCE_CNT << "\n";
	std::cout << "Claimed count : " << claimed_count.load() << "\n";
	std::cout << "Completed : " << completed_count.load() << "\n";
	std::cout << "Decision : SPIN_IMEDIATE = " << decision_immediate_spin.load()
			<< " Spin = " << decision_spin.load()
			<< " Park = " << decision_park.load() << "\n";
	std::cout << "EMA mean Ticks(if any) : ";
	if (auto m = aback.EmaEstAPC().MeanTicks(); m.has_value())
	{
		std::cout << m.value() << "ticks\n";
	}
	else
	{
		std::cout << "N/A\n";
	}
	
	return 0;
}