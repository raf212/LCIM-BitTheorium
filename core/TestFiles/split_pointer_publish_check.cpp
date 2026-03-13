// main.cpp
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>

#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

using namespace PredictedAdaptedEncoding;
using namespace std::chrono_literals;

struct MyObject
{
    int Id;
    double Value;
    bool Processed;
    MyObject(int i, double f) :
        Id(i), Value(f), Processed(false)
    {}
};

static inline void TinyPause()
{
    std::this_thread::sleep_for(50us);
}

int main()
{
#ifdef _MSC_VER
    // Optional CRT debug flags (MSVC)
    int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    flags |= _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_ALWAYS_DF;
    _CrtSetDbgFlag(flags);
#endif

    // ------------- Manager initialization (important) -------------
    // Start the singleton manager early so Register/Unregister calls inside
    // AdaptivePackedCellContainer::InitOwned() succeed without throwing.
    PackedCellContainerManager& mgr = PackedCellContainerManager::Instance();

    // Start manager thread (must be done before containers register)
    mgr.StartPCCManager();

    // Allocate master-clock slots used by containers (avoid throwing inside InitOwned)
    // This returns true on success; prefer to check return rather than rely on exceptions.
    constexpr size_t MASTER_SLOTS = 64;
    bool mc_ok = mgr.GetMasterClockAdaptivePackedCellContainerManager().InitMasterClockSlots(MASTER_SLOTS, 64);
    if (!mc_ok) {
        std::cerr << "Failed to initialize MasterClock slots. Exiting.\n";
        mgr.StopPCCManager();
        return 1;
    }

    // Prime a small node pool so registration won't contend with allocator
    constexpr size_t APC_NODE_POOL = 256;
    mgr.UsePreAllocatedNodePoolOfAdaptivePackedCellContainer(APC_NODE_POOL);

    // (Optional) tune compaction threshold for this test
    mgr.SetRegistryCompectionThreshold(512);

    // ------------- Test containers & workload -------------
    constexpr size_t CAP = 256;
    constexpr size_t N_ITEMS = 128;
    ContainerConf container_configuration;
    container_configuration.ProducerBlockSize = 8;
    container_configuration.BackgroundEpochAdvanceMS = 20;
    container_configuration.RetireBatchThreshold = 4;

    AdaptivePackedCellContainer producer_raw_APC;
    AdaptivePackedCellContainer publishing_APC_for_consumer;

    // InitOwned will register the container with the manager (manager already running
    // and master-clock slots allocated — so the internal try-blocks should be no-ops).
    try
    {
        producer_raw_APC.InitOwned(CAP, REL_NODE0, container_configuration, MAX_VAL);
    }
    catch(const std::exception& e)
    {
        std::cerr << "producer_raw_APC.InitOwned() failed -> " << e.what() << '\n';
        return 1;
    }

    try
    {
        publishing_APC_for_consumer.InitOwned(CAP, REL_NODE0, container_configuration, MAX_VAL);
    }
    catch(const std::exception& e)
    {
        std::cerr << "publishing_APC_for_consumer.InitOwned() failed -> " << e.what() << '\n';
        producer_raw_APC.FreeAll();
        return 1;
    }

    std::atomic<size_t> produced{0}, processed{0}, reaped{0};

    // Producer: publishes objects into producer_raw_APC
    std::thread ProducerThread([&]()
    {
        for (size_t i = 0; i < N_ITEMS; i++)
        {
            MyObject* myobject_ptr = new MyObject(static_cast<int>(i), double(i) * 1.5);
            while (true)
            {
                PublishResult publish_result = producer_raw_APC.PublishHeapPtrPair_(reinterpret_cast<void*>(myobject_ptr), REL_NODE0, 128);
                if (publish_result.ResultStatus == PublishStatus::OK)
                {
                    produced.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                else if (publish_result.ResultStatus == PublishStatus::FULL)
                {
                    std::this_thread::sleep_for(100us);
                }
                else
                {
                    std::cerr << "Producer :: Publish returned INVALID; aborting object " << i << "\n";
                    delete myobject_ptr;
                    break;
                }
            }
            std::this_thread::sleep_for(30us);
        }
    });

    // Consumer: reads from producer_raw_APC, updates object, republishes into publishing_APC_for_consumer
    std::thread ConsumerThread([&]()
    {
        size_t scan_idx = 0;
        while (processed.load(std::memory_order_acquire) < N_ITEMS)
        {
            RelOffsetMode position = RelOffsetMode::RELOFFSET_GENERIC_VALUE;
            auto probable_ptr_output = producer_raw_APC.TryAssemblePairedPtr_(scan_idx, position);
            if (probable_ptr_output)
            {
                uint64_t assembled = *probable_ptr_output;
                MyObject* assembled_object = reinterpret_cast<MyObject*>(static_cast<std::uintptr_t>(assembled));
                if (assembled_object)
                {
                    assembled_object->Value *= 2.0;
                    assembled_object->Processed = true;
                    PublishResult publish_result_2;
                    while (true)
                    {
                        publish_result_2 = publishing_APC_for_consumer.PublishHeapPtrPair_(reinterpret_cast<void*>(assembled_object), REL_NODE0, 128);
                        if (publish_result_2.ResultStatus == PublishStatus::OK)
                        {
                            break;
                        }
                        if (publish_result_2.ResultStatus == PublishStatus::FULL)
                        {
                            std::this_thread::sleep_for(100us);
                        }
                        else
                        {
                            std::cerr << "Consumer: target publish INVALID for idx " << scan_idx << "\n";
                            break;
                        }
                    }
                    producer_raw_APC.RetirePairedPtrAtIdx_(scan_idx, {});
                    processed.fetch_add(1, std::memory_order_release);
                }
            }
            scan_idx = (scan_idx + 1) % producer_raw_APC.GetOrSetContainerCapacity();
            TinyPause();
        }
    });

    // Pointer tester / reaper: consumes from publishing_APC_for_consumer, deletes objects
    std::thread PointerTesterThread([&]()
    {
        size_t scan_idx = 0;
        while (reaped.load(std::memory_order_acquire) < N_ITEMS)
        {
            RelOffsetMode position = RelOffsetMode::RELOFFSET_GENERIC_VALUE;
            auto probable_ptr_output = publishing_APC_for_consumer.TryAssemblePairedPtr_(scan_idx, position);
            if (probable_ptr_output)
            {
                uint64_t assembled = *probable_ptr_output;
                MyObject* object_ptr = reinterpret_cast<MyObject*>(static_cast<std::uintptr_t>(assembled));
                if (object_ptr)
                {
                    if (!object_ptr->Processed)
                    {
                        std::cerr << "Object not marked processed in publishing_APC_for_consumer idx = " << scan_idx << " ID = " << object_ptr->Id << "\n";
                    }
                    delete object_ptr;
                    publishing_APC_for_consumer.RetirePairedPtrAtIdx_(scan_idx, {});
                    reaped.fetch_add(1, std::memory_order_release);
                }
            }
            scan_idx = (scan_idx + 1) % publishing_APC_for_consumer.GetOrSetContainerCapacity();
            TinyPause();
        }
    });

    ProducerThread.join();
    ConsumerThread.join();
    PointerTesterThread.join();

    // Ask managers to advance epoch & attempt final reclaim (best-effort)
    producer_raw_APC.ManualAdvanceEpoch(2);
    publishing_APC_for_consumer.ManualAdvanceEpoch(2);
    producer_raw_APC.TryReclaimRetired_();
    publishing_APC_for_consumer.TryReclaimRetired_();

    // Unregister & cleanup
    producer_raw_APC.FreeAll();
    publishing_APC_for_consumer.FreeAll();

    // Stop manager after containers freed (clean shutdown)
    mgr.StopPCCManager();

    std::cout << "Produced: " << produced.load() << " Processed: " << processed.load() << " Reaped: " << reaped.load() << '\n';
    return 0;
}