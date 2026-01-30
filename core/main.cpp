// main.cpp — test driver to exercise AtomicDataSignalArray.hpp
// Purpose: trigger compiler errors in your header so you can fix them iteratively.

#include <iostream>
#include <vector>
#include <thread>
#include <cassert>

#include "AtomicDataSignalArray.hpp"   // your header under test
#include "PackedStRel.h"              // bring in macros/constants used by the header

using namespace AtomicCScompact;

int main()
{
    std::cout << "AtomicDataSignalArray compile smoke test\n";

    // instantiate both modes to exercise template code paths
    AtomicDataSignalArray<PackedMode::MODE_VALUE32> dsa_v32;
    AtomicDataSignalArray<PackedMode::MODE_CLKVAL48> dsa_c48;

    // Attempt to initialize (this exercises InitOwned path)
    try {
        dsa_v32.InitOwned(128);     // small capacity
        dsa_c48.InitOwned(64);
    } catch (const std::exception &e) {
        std::cerr << "init exception: " << e.what() << "\n";
    }

    // Make an idle packed cell and publish it (exercises PublishPackedOfADSA)
    packed64_t idle_v32 = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
    PublishResult pres = dsa_v32.PublishPackedOfADSA(idle_v32);
    std::cout << "Publish result status: " << static_cast<int>(pres.status) << " idx: " << pres.index << "\n";

    // Try publishing with offset / batch (exercises SetEncodeOffset, PublishBatchFromReserved)
    PublishResult pres2 = dsa_v32.PublishWithOffset(idle_v32, idle_v32);
    std::cout << "PublishWithOffset: status=" << static_cast<int>(pres2.status) << " idx=" << pres2.index << "\n";

    // Reserve a block then publish a batch (exercises reservation & batch API)
    size_t base = dsa_v32.ReserveProducerSlots(8);
    std::vector<packed64_t> items(8, idle_v32);
    PublishResult presBatch = dsa_v32.PublishBatchFromReserved(base, items.data(), items.size());
    std::cout << "PublishBatchFromReserved: status=" << static_cast<int>(presBatch.status) << " base=" << presBatch.index << "\n";

    // Claim a single item (exercises ClaimOne + ClaimBatch)
    size_t out_idx = SIZE_MAX;
    packed64_t out_obs = 0;
    bool claimed = dsa_v32.ClaimOne(REL_ALL_LOW5, out_idx, out_obs);
    std::cout << "ClaimOne returned: " << claimed << " idx=" << out_idx << "\n";

    // ClaimBatch usage
    std::vector<std::pair<size_t, packed64_t>> outvec;
    // note: signature in header might differ; try to call the ClaimBatch variant you expect.
    // This line is intentionally here to provoke compile errors where API mismatches exist.
    auto batch_sz = dsa_v32.ClaimBatch(REL_ALL_LOW5, reinterpret_cast<std::vector<std::pair<size_t, size_t>> &>(outvec), 4);
    (void)batch_sz;

    // Region APIs (InitRegionIndex, ScanRelRanges, RebuildRegionBitmaps, FindState)
    try {
        dsa_v32.InitRegionIndex(16);
    } catch(...) {}

    auto ranges = dsa_v32.ScanRelRanges(REL_ALL_LOW5);
    std::cout << "Ranges count: " << ranges.size() << "\n";

    dsa_v32.RebuildRegionBitmaps();
    auto states = dsa_v32.FindState(ST_PUBLISHED);
    std::cout << "Found " << states.size() << " slots in ST_PUBLISHED\n";

    // Offset APIs
    dsa_v32.SetEncodeOffset(0, idle_v32);
    auto off = dsa_v32.GetEncodedOffset(0);
    (void)off;

    // Try recycle / commit APIs
    auto prev = dsa_v32.Recycle(0);
    (void)prev;
    dsa_v32.CommitIdxWithPayload(0, idle_v32);
    dsa_v32.CommitMarkComplete(0);

    // Try TryIncrementClock (MODE-specific)
    packed64_t out_new = 0;
    bool inc_ok = dsa_v32.TryIncrementClk16LowLevel(0, 1, out_new);
    std::cout << "TryIncrementClk16LowLevel returned: " << inc_ok << "\n";

    // Basic occupancy query
    std::cout << "Occupancy: " << dsa_v32.GetOccupancy() << "\n";

    // Tidy
    // destructor will call FreeAll

    std::cout << "Done test driver\n";
    return 0;
}
