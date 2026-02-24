// apc_pointer_roundtrip_test.cpp
#include <iostream>
#include <iomanip>
#include <random>
#include <vector>
#include <cassert>
#include <cstdint>

#include "AdaptivePackedCellContainer.hpp"
#include "PackedCell.hpp"

using namespace AtomicCScompact;

static inline std::string hex64(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setfill('0') << std::setw(16) << v;
    return ss.str();
}
static inline std::string hex32(uint32_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setfill('0') << std::setw(8) << v;
    return ss.str();
}

static inline std::pair<uint32_t,uint32_t> SplitPointer64(uint64_t x) noexcept {
    uint32_t low = static_cast<uint32_t>(x & 0xFFFFFFFFULL);
    uint32_t high = static_cast<uint32_t>((x >> 32) & 0xFFFFFFFFULL);
    return { low, high };
}
static inline uint64_t JoinPointer64(uint32_t low, uint32_t high) noexcept {
    uint64_t lo = static_cast<uint64_t>(low);
    uint64_t hi = static_cast<uint64_t>(high);
    return (hi << 32) | lo;
}

int main() {
    std::cout << "APC pointer roundtrip test\n";

    static_assert(VALBITS == 32u, "This test expects VALBITS==32");

    AdaptivePackedCellContainer cont;
    ContainerConf cfg;
    cfg.ProducerBlockSize = 8;
    cfg.BackgroundEpochAdvanceMS = 0; // deterministic
    cfg.InitialMode = PackedMode::MODE_VALUE32;

    try {
        cont.InitOwned(128, 0, cfg, 64);
    } catch (const std::exception& e) {
        std::cerr << "InitOwned failed: " << e.what() << "\n";
        return 2;
    }

    // If you set UNSAFE_RAW_PUBLISH = true, this test will publish raw integers (NOT heap pointers)
    // and attempt to retire them. That triggers the exact crash you found unless you apply the
    // RelEntry fix (switch to PACKED_NODE). Keep this false to run the safe test.
    constexpr bool UNSAFE_RAW_PUBLISH = false;

    auto do_case = [&](uint64_t test_val)->bool {
        void* obj_ptr = nullptr;
        // If unsafe mode: publish raw value cast to pointer (this causes crash in current code).
        // Otherwise allocate an actual heap object so delete is valid.
        if (UNSAFE_RAW_PUBLISH) {
            obj_ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(test_val));
        } else {
            // allocate using operator new so later deletion is valid
            uint64_t* heapp = new uint64_t(test_val);
            obj_ptr = static_cast<void*>(heapp);
        }

        PublishResult pr = cont.PublishHeapPtrPair_(obj_ptr, /*rel_mask*/0u, /*max_probes*/ 8);
        if (pr.ResultStatus != PublishStatus::OK) {
            std::cout << "Publish failed for " << hex64(test_val) << " status=" << static_cast<int>(pr.ResultStatus) << "\n";
            if (!UNSAFE_RAW_PUBLISH) delete static_cast<uint64_t*>(obj_ptr);
            return false;
        }
        size_t lead_idx = pr.Index;
        size_t tail_idx = (lead_idx + 1) % cont.ContainerCapacity_;

        packed64_t raw_head = cont.BackingPtr[lead_idx].load(MoLoad_);
        packed64_t raw_tail = cont.BackingPtr[tail_idx].load(MoLoad_);

        val32_t head_val32 = PackedCell64_t::ExtractValue32(raw_head);
        val32_t tail_val32 = PackedCell64_t::ExtractValue32(raw_tail);

        // Assemble using container API
        RelOffsetMode pos = RelOffsetMode::RELOFFSET_GENERIC_VALUE;
        auto opt = cont.TryAssemblePairedPtr_(lead_idx, pos);
        uint64_t assembled_api = opt ? *opt : UINT64_MAX;

        // Assemble defensively from extracted halves
        uint64_t assembled_defensive = (static_cast<uint64_t>(tail_val32) << VALBITS) | static_cast<uint64_t>(head_val32);

        // expected from original test_val (if we published raw) or from heap pointer value
        uint64_t expected;
        if (UNSAFE_RAW_PUBLISH) {
            expected = test_val;
        } else {
            expected = reinterpret_cast<uint64_t>(obj_ptr);
        }

        bool ok_api = (opt && assembled_api == expected);
        bool ok_defensive = (assembled_defensive == expected);

        if (!ok_api || !ok_defensive) {
            std::cout << "==== MISMATCH for published " << hex64(expected) << " ====\n";
            std::cout << "lead_idx=" << lead_idx << " tail_idx=" << tail_idx << "\n";
            std::cout << "raw_head  = " << hex64(raw_head) << "  raw_tail = " << hex64(raw_tail) << "\n";
            std::cout << "head_val32 = " << hex32(static_cast<uint32_t>(head_val32)) << ", tail_val32 = " << hex32(static_cast<uint32_t>(tail_val32)) << "\n";
            std::cout << "assembled_api      = " << (opt ? hex64(assembled_api) : std::string("<none>")) << " pos=" << static_cast<int>(pos) << "\n";
            std::cout << "assembled_defensive= " << hex64(assembled_defensive) << "\n";
            std::cout << "expected_rejoined  = " << hex64(expected) << "\n";
            std::cout << "==== end mismatch ====\n\n";
            // retire and clean up (if safe)
            cont.RetirePairedPtrAtIdx_(lead_idx);
            if (!UNSAFE_RAW_PUBLISH) {
                // heap allocation will be freed by reclaimer eventually; to be deterministic free here
                // but container expects to reclaim it; we already enqueued it for retire so avoid double delete.
            }
            return false;
        }

        // retire now (enqueue for reclaim). For deterministic immediate reclaim you may need to ManualAdvanceEpoch().
        cont.RetirePairedPtrAtIdx_(lead_idx);

        // If we allocated heap memory manually in this test (UNSAFE_RAW_PUBLISH == false), the reclaimer may
        // eventually delete it. To avoid memory leak in the test run (since background reclaimer is off),
        // manually trigger epoch advance + reclaim:
        cont.ManualAdvanceEpoch(1);
        cont.TryReclaimRetired_();

        return true;
    };

    std::vector<uint64_t> cases = {
        0ull,
        1ull,
        0x1234567890ABCDEFull,
        0x50ABCDEF12345678ull,
        0x8000000080000000ull
    };

    std::mt19937_64 rng(1234567);
    for (int i=0;i<50;i++) cases.push_back(rng());

    size_t passed = 0, failed = 0;
    for (auto v : cases) {
        bool ok = do_case(v);
        if (ok) ++passed; else ++failed;
    }

    std::cout << "Tests finished. passed=" << passed << " failed=" << failed << "\n";

    cont.FreeAll();
    return (failed == 0) ? 0 : 1;
}