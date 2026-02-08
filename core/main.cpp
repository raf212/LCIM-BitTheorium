// main.cpp — comprehensive test driver for AtomicDataSignalArray
#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <chrono>
#include <mutex>
#include <sstream>
#include <atomic>

#include "AtomicDataSignalArray.hpp"
#include "AtomicAdaptiveBackoff.hpp"
#include "MasterClockConf.hpp"
#include "PackedStRel.h" // for REL_* and ST_*

using namespace AtomicCScompact;

static std::mutex g_logmu;
static inline void LOG(const std::string &s) {
    std::lock_guard<std::mutex> g(g_logmu);
    std::cerr << "[" << std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()
              << "ms] " << s << std::endl;
}

static inline void CHECK(bool cond, const std::string &msg) {
    if (!cond) {
        LOG(std::string("CHECK FAILED: ") + msg);
    } else {
        LOG(std::string("CHECK OK: ") + msg);
    }
}

// helper to print packed payload state
static std::string DumpPacked(packed64_t p) {
    std::ostringstream oss;
    strl16_t sr = PackedCell64_t::ExtractSTRL(p);
    tag8_t st = ExtractLocalityFromSTRL(sr);
    tag8_t rel = PackedCell64_t::ExtractFullRelFromPacked(sr);
    oss << "P=0x" << std::hex << p << std::dec
        << " st=" << int(st) << " rel=0x" << int(rel);
    if constexpr(true) {
        if (st == ST_PUBLISHED) {
            if constexpr(true) {
                val32_t v = PackedCell64_t::ExtractValue32(p);
                oss << " val=" << v;
            }
        }
    }
    return oss.str();
}

int main() {
    LOG("Starting AtomicDataSignalArray tests");

    // -------------------
    // Basic init
    // -------------------
    AtomicDataSignalArray<PackedMode::MODE_VALUE32> dsa_v32;
    AtomicDataSignalArray<PackedMode::MODE_CLKVAL48> dsa_c48;
    try {
        dsa_v32.InitOwned(128);
        dsa_c48.InitOwned(64);
        LOG("InitOwned succeeded for both modes");
    } catch (const std::exception &e) {
        LOG(std::string("InitOwned threw: ") + e.what());
        return 2;
    }

    CHECK(dsa_v32.GetOccupancy() == 0, "Occupancy starts at 0");

    // -------------------
    // Publish idle payload (should return OK)
    // -------------------
    packed64_t idle_v32 = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
    PublishResult pres = dsa_v32.PublishPackedOfADSA(idle_v32);
    LOG("Publish result status: " + std::to_string(int(pres.status)) + " idx: " + std::to_string(pres.index));
    CHECK(pres.status == PublishStatus::OK, "Publish idle returns OK");

    // PublishWithOffset test
    PublishResult pres2 = dsa_v32.PublishWithOffset(idle_v32, idle_v32);
    LOG("PublishWithOffset: status=" + std::to_string(int(pres2.status)) + " idx=" + std::to_string(pres2.index));
    CHECK(pres2.status == PublishStatus::OK, "PublishWithOffset OK");

    // Reserve + Batch publish
    size_t base = dsa_v32.ReserveProducerSlots(8);
    LOG("ReserveProducerSlots returned base=" + std::to_string(base));
    std::vector<packed64_t> items(8, idle_v32);
    PublishResult presBatch = dsa_v32.PublishBatchFromReserved(base, items.data(), items.size());
    LOG("PublishBatchFromReserved: status=" + std::to_string(int(presBatch.status)) + " base=" + std::to_string(presBatch.index));
    CHECK(presBatch.status == PublishStatus::OK, "PublishBatchFromReserved OK");

    CHECK(dsa_v32.GetOccupancy() >= 0, "Occupancy non-negative after publishes");

    // -------------------
    // ClaimOne with matching relation test
    // publish an item with REL_NODE0 and try claim with REL_NODE0 mask
    // -------------------
    // Prepare payload with REL_NODE0
    strl16_t n0_sr = MakeSTRL4_t(3, ST_PUBLISHED, REL_NODE0, REL_NONE);
    packed64_t payload_node0 = PackedCell64_t::ComposeValue32u_64(42u, 1u, n0_sr);
    PublishResult p3 = dsa_v32.PublishPackedOfADSA(payload_node0);
    LOG("Published payload_node0 idx=" + std::to_string(p3.index));
    size_t out_idx = SIZE_MAX;
    packed64_t out_obs = 0;
    bool claimed = dsa_v32.ClaimOne(REL_NODE0, out_idx, out_obs);
    LOG(std::string("ClaimOne returned: ") + (claimed ? "true" : "false") + " idx=" + (out_idx==SIZE_MAX ? "SIZE_MAX" : std::to_string(out_idx)));
    CHECK(claimed == true, "ClaimOne matched REL_NODE0 should succeed");

    if (claimed) {
        LOG("Claim observed: " + DumpPacked(out_obs));
        // mark complete / recycle
        dsa_v32.CommitIdxWithPayload(out_idx, out_obs);
        packed64_t prev = dsa_v32.Recycle(out_idx);
        LOG("Recycled idx=" + std::to_string(out_idx) + " prev=" + DumpPacked(prev));
    }

    // -------------------
    // ClaimOne relation mismatch (expected fail)
    // Publish REL_NONE and try ClaimOne(REL_ALL_LOW_4) -> should fail (intentional behaviour)
    // -------------------
    strl16_t drn_sr = MakeSTRL4_t(0, ST_PUBLISHED, REL_NONE, REL_NONE);
    packed64_t payload_none = PackedCell64_t::ComposeValue32u_64(99u, 2u, drn_sr);
    PublishResult p4 = dsa_v32.PublishPackedOfADSA(payload_none);
    LOG("Published payload_none idx=" + std::to_string(p4.index));
    size_t out_idx2 = SIZE_MAX;
    packed64_t out_obs2 = 0;
    bool claimed2 = dsa_v32.ClaimOne(REL_ALL_LOW_4, out_idx2, out_obs2);
    LOG(std::string("ClaimOne with REL_ALL_LOW_4 on REL_NONE returned: ") + (claimed2 ? "true" : "false"));
    CHECK(claimed2 == false, "ClaimOne should not match REL_NONE when caller requests REL_ALL_LOW_4");

    // -------------------
    // ClaimBatch basic test
    // -------------------
    // publish several items with REL_NODE1
    std::vector<packed64_t> many;
    for (uint32_t i = 0; i < 10; ++i) {
        strl16_t pa_sr = MakeSTRL4_t(2, ST_PUBLISHED, REL_NODE1, REL_NONE);
        packed64_t pa = PackedCell64_t::ComposeValue32u_64(100 + i, static_cast<clk16_t>(i + 10), pa_sr);
        dsa_v32.PublishPackedOfADSA(pa);
        many.push_back(pa);
    }
    std::vector<std::pair<size_t, packed64_t>> outvec;
    packed64_t batch_info = dsa_v32.ClaimBatch(REL_NODE1, outvec, 4);
    LOG("ClaimBatch returned batch_info=" + std::to_string(PackedCell64_t::ExtractValue32(batch_info))
         + " claimed=" + std::to_string(outvec.size()));
    CHECK(outvec.size() <= 4, "ClaimBatch returned up to requested count");

    // cleanup claimed items
    for (auto &pr : outvec) {
        dsa_v32.CommitIdxWithPayload(pr.first, pr.second);
        dsa_v32.Recycle(pr.first);
    }

    // -------------------
    // TryIncrementClk16LowLevel test
    // -------------------
    // publish a value to idx_inc directly via PublishPackedOfADSA to set clk
    strl16_t ppub_sr = MakeSTRL4_t(1, ST_PUBLISHED, REL_SELF, REL_NONE);
    packed64_t ppub = PackedCell64_t::ComposeValue32u_64(0xABCu, 0xFFF0u, ppub_sr);
    PublishResult presIdx = dsa_v32.PublishPackedOfADSA(ppub);
    LOG("Published for increment test idx=" + std::to_string(presIdx.index));
    packed64_t out_new = 0;
    bool inc_ok = dsa_v32.TryIncrementClk16LowLevel(presIdx.index, 5, out_new);
    LOG(std::string("TryIncrementClk16LowLevel returned: ") + (inc_ok ? "true" : "false") + " out_new=" + DumpPacked(out_new));
    CHECK(inc_ok, "TryIncrementClk16LowLevel should succeed on a published slot");

    // -------------------
    // RegionIndex and ScanRelRanges test
    // -------------------
    try {
        dsa_v32.InitRegionIndex(16);
        auto ranges_all = dsa_v32.ScanRelRanges(REL_ALL_LOW_4);
        LOG("ScanRelRanges returned " + std::to_string(ranges_all.size()) + " ranges");
        // not asserting exact number: just ensure call works
        CHECK(true, "ScanRelRanges callable");
    } catch (const std::exception &e) {
        LOG(std::string("InitRegionIndex threw: ") + e.what());
        CHECK(false, "InitRegionIndex should not throw with valid region size");
    }

    // -------------------
    // ABA demonstration (consumer reads A, writer swaps A->B->A), check CAS result
    // -------------------
    LOG("ABA demonstration starting");
    // pick slot S
    size_t aba_slot = 3;
    // Make A and B distinct published words
    strl16_t ab_sr = MakeSTRL4_t(1, ST_PUBLISHED, REL_SELF, REL_NONE);
    packed64_t A = PackedCell64_t::ComposeValue32u_64(0xAAu, clk16_t(0x1234), ab_sr);
    packed64_t B = PackedCell64_t::ComposeValue32u_64(0xBBu, clk16_t(0x2222), ab_sr);
    dsa_v32.CommitIdxWithPayload(aba_slot, A); // install A as committed (store)
    dsa_v32.BackingPtr[aba_slot].store(A, MoStoreSeq_); // ensure published A
    dsa_v32.BackingPtr[aba_slot].notify_all();
    std::atomic<bool> consumer_done{false};
    std::atomic<bool> swap_done{false};
    std::thread aba_cons([&]{
        packed64_t expected = dsa_v32.BackingPtr[aba_slot].load(MoLoad_);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        packed64_t desired = PackedCell64_t::SetLocalityInPacked(expected, ST_CLAIMED);
        bool ok = dsa_v32.BackingPtr[aba_slot].compare_exchange_strong(expected, desired, EXsuccess_, EXfailure_);
        LOG(std::string("[ABA] Consumer CAS result: ") + (ok ? "SUCCESS (ABA EXPLOITED)" : "FAILED -> not exploited"));
        consumer_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    dsa_v32.BackingPtr[aba_slot].store(B, MoStoreSeq_);
    dsa_v32.BackingPtr[aba_slot].notify_all();
    dsa_v32.BackingPtr[aba_slot].store(A, MoStoreSeq_);
    dsa_v32.BackingPtr[aba_slot].notify_all();
    swap_done.store(true);
    aba_cons.join();

    // -------------------
    // MasterClockConf integration test
    // -------------------
    LOG("MasterClockConf integration test");
    Timer48 timer;
    MasterClockConf mc(timer, REL_NODE0);
    bool created = mc.InitMasterClockSlots(8);
    CHECK(created, "MasterClockConf InitMasterClockSlots OK");
    size_t mid = mc.RegisterMasterClockSlot();
    LOG("MasterClockConf registered slot id=" + std::to_string(mid));
    size_t prev = dsa_v32.AttachThreadMasterClockID(mid);
    LOG("Attached thread master clock id (prev=" + std::to_string(prev) + ")");
    // Now publish with DSA: item should pick up master clock when AttachThreadMasterClockID used in producer path
    strl16_t mp_sr = MakeSTRL4_t(5, ST_PUBLISHED, REL_NODE0, REL_NONE);
    packed64_t master_pub = PackedCell64_t::ComposeValue32u_64(777u, 0u, mp_sr);
    PublishResult pmc = dsa_v32.PublishPackedOfADSA(master_pub);
    LOG("Published master_pub idx=" + std::to_string(pmc.index));
    CHECK(pmc.status == PublishStatus::OK, "Publish with MasterClockConf attached ok");

    // -------------------
    // Small concurrency stress test
    // -------------------
    LOG("Starting small concurrency stress test");
    const unsigned NPROD = 4;
    const unsigned NCONS = 4;
    const unsigned PER_PROD = 100;
    std::atomic<unsigned> produced{0}, consumed{0};
    std::atomic<bool> produce_done{false};

    auto producer = [&](unsigned id) {
        for (unsigned i = 0; i < PER_PROD; ++i) {
            strl16_t p_sr = MakeSTRL4_t(1, ST_PUBLISHED, REL_NONE, REL_PATTERN);
            packed64_t p = PackedCell64_t::ComposeValue32u_64(static_cast<uint32_t>(id*1000 + i),
                                                       static_cast<clk16_t>(i & 0xFFFF),
                                                    p_sr
                                                    );
            auto r = dsa_v32.PublishPackedOfADSA(p);
            if (r.status == PublishStatus::OK) produced.fetch_add(1, std::memory_order_relaxed);
            else {
                // backpressure: small sleep and retry a few times
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                r = dsa_v32.PublishPackedOfADSA(p);
                if (r.status == PublishStatus::OK) produced.fetch_add(1);
            }
        }
    };

    auto consumer = [&](unsigned /*id*/) {
        size_t out_i;
        packed64_t o;
        while (!produce_done.load(std::memory_order_acquire) ||
            consumed.load() < produced.load()) {

            bool ok = dsa_v32.ClaimOne(REL_ALL_LOW_4, out_i, o, 16);
            if (ok) {
                dsa_v32.CommitIdxWithPayload(out_i, o);
                dsa_v32.Recycle(out_i);
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    };

    std::vector<std::thread> prodt, constt;
    for (unsigned i = 0; i < NPROD; ++i) prodt.emplace_back(producer, i);
    for (unsigned i = 0; i < NCONS; ++i) constt.emplace_back(consumer, i);
    for (auto &t : prodt) t.join();
    produce_done.store(true, std::memory_order_release);
    for (auto &t : constt) t.join();

    LOG("Concurrency test produced=" + std::to_string(produced.load()) + " consumed=" + std::to_string(consumed.load()));
    CHECK(consumed.load() == produced.load(), "Concurrency consumed == produced");

    // -------------------
    // AdaptiveBackoff sample decisions
    // -------------------
    LOG("AdaptiveBackoff decision sample:");
    AtomicAdaptiveBackoff::PCBCfg cfg;
    AtomicAdaptiveBackoff ab(cfg, PackedMode::MODE_VALUE32, Timer48());
    // simulate payload with an old clock
    strl16_t op_sr = MakeSTRL4_t(1, ST_PUBLISHED, REL_SELF, REL_NONE);
    packed64_t oldpub = PackedCell64_t::ComposeValue32u_64(1u, clk16_t(0x1000), op_sr);
    auto dec = ab.DecideForSlot(oldpub);
    LOG(std::string("Dec.Action=") + std::to_string((int)dec.Action) + " SuggestedUs=" + std::to_string(dec.SuggestedUs) + " Haz=" + std::to_string(dec.EstHazPerSec));

    // final tidy
    LOG("Tests completed. Final occupancy: " + std::to_string(dsa_v32.GetOccupancy()));
    LOG("Driver finished");
    return 0;
}
