#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>

#include "AdaptivePackedCellContainer.hpp"

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
    // enable leak check and aggressive heap consistency checks
    int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    flags |= _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_ALWAYS_DF;
    _CrtSetDbgFlag(flags);
    // optionally break when a particular allocation number is hit:
    // _CrtSetBreakAlloc(12345);
#endif
    constexpr size_t CAP = 256;
    constexpr size_t N_ITEMS = 128;
    ContainerConf container_configuration;
    container_configuration.ProducerBlockSize = 8;
    container_configuration.BackgroundEpochAdvanceMS = 20;
    container_configuration.RetireBatchThreshold = 4;

    AdaptivePackedCellContainer producer_raw_APC;
    AdaptivePackedCellContainer publishing_APC_for_consumer;
    
    try
    {
        producer_raw_APC.InitOwned(CAP, REL_NODE0, container_configuration, MAX_VAL);
    }
    catch(const std::exception& e)
    {
        std::cerr << "producer_raw_APC.InitOwned() == failed" << e.what() << '\n';
        return 1;
    }
    
    try
    {
        publishing_APC_for_consumer.InitOwned(CAP, REL_NODE0, container_configuration, MAX_VAL);
    }
    catch(const std::exception& e)
    {
        std::cerr << "publishing_APC_for_consumer.InitOwned() == failed" << e.what() << '\n';
        producer_raw_APC.FreeAll();
        return 1;
    }

    std::atomic<size_t> produced{0}, processed{0}, reaped{0};
    std::atomic<bool> stop_flags{false};
    
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
                    std::cerr << "Producer :: Publish returned INVALID; aborting object" << i << "\n";
                    delete myobject_ptr;
                    break;
                }
            }
            std::this_thread::sleep_for(30us);
        }
    });

    //210
    std::thread ConsumerThread([&]()
    {
        size_t scan_idx = 0;
        while (processed.load(MoLoad_) < N_ITEMS)
        {
            RelOffsetMode position = RelOffsetMode::RELOFFSET_GENERIC_VALUE;
            auto probable_ptr_output = producer_raw_APC.TryAssemblePairedPtr_(scan_idx, position);
            if (probable_ptr_output)
            {
                uint64_t assembled = *probable_ptr_output;
                MyObject* assembeled_object = reinterpret_cast<MyObject*>(static_cast<std::uintptr_t>(assembled));
                if (assembeled_object)
                {
                    assembeled_object->Value *= 2.0;
                    assembeled_object->Processed = true;
                    PublishResult publish_result_2;
                    while (true)
                    {
                        publish_result_2 = publishing_APC_for_consumer.PublishHeapPtrPair_(reinterpret_cast<void*>(assembeled_object), REL_NODE0, 128);
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
            scan_idx = (scan_idx + 1) % producer_raw_APC.ContainerCapacity_;
            TinyPause();
        }
    });

    std::thread PointerTesterThread([&]()
    {
        size_t scan_idx = 0;
        while (reaped.load(MoLoad_) < N_ITEMS)
        {
            RelOffsetMode position = RelOffsetMode::RELOFFSET_GENERIC_VALUE;
            auto probable_ptr_output = publishing_APC_for_consumer.TryAssemblePairedPtr_(scan_idx, position);
            if (probable_ptr_output)
            {
                uint64_t assembeled = *probable_ptr_output;
                MyObject* object_ptr = reinterpret_cast<MyObject*>(static_cast<std::uintptr_t>(assembeled));
                if (object_ptr)
                {
                    if (!object_ptr->Processed)
                    {
                        std::cerr << "Object not marked processed in producer_raw_APC idx = "  << scan_idx  << "ID = " << object_ptr->Id <<"\n";
                    }
                    delete object_ptr;
                    publishing_APC_for_consumer.RetirePairedPtrAtIdx_(scan_idx, {});
                    reaped.fetch_add(1, std::memory_order_release);
                }
            }
            scan_idx = (scan_idx + 1) % publishing_APC_for_consumer.ContainerCapacity_;
            TinyPause();
        }
    });

    ProducerThread.join();
    ConsumerThread.join();
    PointerTesterThread.join();

    producer_raw_APC.ManualAdvanceEpoch(2);
    publishing_APC_for_consumer.ManualAdvanceEpoch(2);
    producer_raw_APC.TryReclaimRetired_();
    publishing_APC_for_consumer.TryReclaimRetired_();
    producer_raw_APC.StopBackgroundReclaimer();
    publishing_APC_for_consumer.StopBackgroundReclaimer();
    producer_raw_APC.FreeAll();
    publishing_APC_for_consumer.FreeAll();

    std::cout<< "Producer " << produced.load() << " Processed : " << processed.load() << " Check Psassed " << reaped.load() << '\n';
    return 0;
}