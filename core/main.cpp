// main.cpp
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <deque>
#include <algorithm>
#include <cmath>

#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

#define FIRST_PRIME 2u
#define MAX_TAIL_ATTEMPTS 20000u


using namespace PredictedAdaptedEncoding;

constexpr uint64_t NTH_ELEMENT = 5000000ull;
constexpr uint64_t BLOCK_SIZE = 20000ull;
constexpr unsigned PRODUCER_COUNT = 2u;
constexpr unsigned CONSUMER_COUNT = 8u;
constexpr size_t MIN_APC_CAPACITY = 4096;
constexpr size_t RESULT_APC_CAPACITY = 4096;
constexpr size_t RESULT_BRANCH_THRESHOLD = 2048;
constexpr size_t SORT_OFFLOAD_SIZE = 4096;
constexpr size_t GPU_THRESHOLD = 100000;



struct RangedTaskConf
{
    uint64_t StartingPoint;
    uint64_t EndPoint;
    RangedTaskConf(uint64_t start, uint64_t end) :
        StartingPoint(start), EndPoint(end)
    {}
};

static void SegmentedSiveSimplified(uint64_t left, uint64_t right, std::vector<uint64_t>& primes_out)
{
    if (right < FIRST_PRIME || left > right)
    {
        return;
    }
    uint64_t low = left;
    uint64_t high = right;
    uint64_t width = (high - low) + 1;
    std::vector<char> is_prime(width, 1);
    uint64_t limit = static_cast<uint64_t>(std::sqrt(high)) + 1;
    std::vector<char> mark(limit +1, 1);
    std::vector<uint64_t> small;
    for (uint64_t p = FIRST_PRIME; p <= limit; p++)
    {
        if (mark[p])
        {
            small.push_back(p);
            for (uint64_t q = p * p; q <= limit; q +=p)
            {
                mark[q] = 0;
            }
        }
    }
    for (uint64_t p : small)
    {
        uint64_t start = std::max(p * p, (uint64_t)(std::ceil((double)low / p) * p));
        if (start < p * p)
        {
            start = p * p;
        }
        for (uint64_t j = start; j <= high; j +=p)
        {
            is_prime[j - low] = 0;
        }
    }
    for (uint64_t i = 0; i < width; i++)
    {
        if (is_prime[i])
        {
            uint64_t value = low + i;
            if (value >= 2)
            {
                primes_out.push_back(value);
            }
        }
    }
}

struct StopWatch
{
    decltype(std::chrono::steady_clock::now()) StartStopWatch;
    StopWatch() noexcept :
        StartStopWatch(std::chrono::steady_clock::now())
    {}
    void ResetStopWatch() noexcept 
    {
        StartStopWatch = std::chrono::steady_clock::now();
    }
    uint64_t ElapsedMicroSecond() const noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - StartStopWatch).count()
        );
    }
};

struct AtomicTimerStatus
{
    std::atomic<uint64_t> Count{0};
    std::atomic<uint64_t> TotalMicroSecond{0};
    std::atomic<uint64_t> MaxMicroSecond{0};
    void AddSamplMicroSecond(uint64_t micro_second) noexcept
    {
        Count.fetch_add(1, std::memory_order_relaxed);
        TotalMicroSecond.fetch_add(micro_second);
        uint64_t previous = MaxMicroSecond.load(std::memory_order_relaxed);
        while (previous < micro_second && !MaxMicroSecond.compare_exchange_weak(previous, micro_second, std::memory_order_relaxed))
        {
        }
    }
    uint64_t GetSampleCount() const noexcept
    {
        return Count.load(std::memory_order_relaxed);
    }
    uint64_t GetTotalMicroSeconds() const noexcept
    {
        return TotalMicroSecond.load(std::memory_order_relaxed);
    }
    uint64_t GetMaxMicroSecond() const noexcept
    {
        return MaxMicroSecond.load(std::memory_order_relaxed);
    }

    double AvarageMiliSecond() const noexcept
    {
        uint64_t samples = GetSampleCount();
        if (samples ==0)
        {
            return 0.0;
        }
        return static_cast<double>(GetTotalMicroSeconds()) / static_cast<double>(samples);
    }
    
};

int main()
{
    std::ios::sync_with_stdio(true);
    std ::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::cout << "AdaptivePackedCellContainer + Manager + AtomicAdaptiveBackoff :: Prime number find and sort test\n";
    std::cout << "Finding Upto Element : " << NTH_ELEMENT << ", Eack Block Size : " << BLOCK_SIZE << "\n";

    StopWatch runtime_stop_watch;

    PackedCellContainerManager& apc_mannager_prime_test = PackedCellContainerManager::Instance();
    apc_mannager_prime_test.StartPCCManager();

    std::vector<std::pair<uint64_t, uint64_t>> task_ranges;
    for (size_t lo = FIRST_PRIME; lo <= NTH_ELEMENT; lo += BLOCK_SIZE)
    {
        uint64_t hi = std::min(NTH_ELEMENT, lo + BLOCK_SIZE - 1);
        task_ranges.emplace_back(lo, hi);
    }
    
    size_t num_of_tasks = task_ranges.size();
    size_t apc_capacity = std::max<size_t>(MIN_APC_CAPACITY, num_of_tasks * 2 + 16); //what is 2 + 16 and why??
    //TASK_APC (producer-> consumer)
    AdaptivePackedCellContainer TASK_APC;
    ContainerConf task_cfg;
    task_cfg.ProducerBlockSize = 64;
    task_cfg.MaxTlsCandidates = 1024;
    task_cfg.InitialMode = PackedMode::MODE_VALUE32;
    TASK_APC.InitOwned(apc_capacity, REL_NODE0, task_cfg);
    TASK_APC.SetManagerForGlobalAPC(&apc_mannager_prime_test);
    apc_mannager_prime_test.RequestForReclaimationOfTheAdaptivePackedCellContainer(&TASK_APC);
    //RESULT_APC (consumer -> collector)
    AdaptivePackedCellContainer RESULT_APC;
    ContainerConf result_cfg = task_cfg;
    result_cfg.ProducerBlockSize = 32;
    RESULT_APC.InitOwned(RESULT_APC_CAPACITY, REL_NODE0, result_cfg);
    RESULT_APC.SetManagerForGlobalAPC(&apc_mannager_prime_test);
    apc_mannager_prime_test.RequestForReclaimationOfTheAdaptivePackedCellContainer(&RESULT_APC);
    //mutexes and conditional veriables
    std::mutex collector_mutex;
    std::vector<uint64_t> all_primes_array;
    std::mutex sort_queue_mutex;
    std::deque<std::vector<uint64_t>*> sort_task_queue;
    std::condition_variable sort_cv;
    std::mutex mutex_of_collector_cv;
    std::condition_variable collector_cv;
    std::atomic<size_t> producer_done{0};

    std::atomic<size_t> published_to_result{0};
    std::atomic<size_t> retired_from_task{0};

    //Timing timer
    AtomicTimerStatus producer_publish_timer;
    AtomicTimerStatus consumer_compute_timer;
    AtomicTimerStatus consumer_result_publish_timer;
    AtomicTimerStatus collector_marge_timer;

    std::atomic<uint64_t> task_processed{0};

    //PRODUCER
    auto producer_function = [&](unsigned producer_id)
    {
        std::cout << " Producer ID : " << producer_id << " -> starting\n";
        StopWatch local_stop_watch_producer;
        size_t start_idx = (task_ranges.size() * producer_id) / PRODUCER_COUNT;
        size_t end_idx = (task_ranges.size() * (producer_id + 1)) / PRODUCER_COUNT;
        for (size_t i = start_idx ; i < end_idx; i++)
        {
            uint64_t high = task_ranges[i].second;
            uint64_t low = task_ranges[i].first;
            RangedTaskConf* range_task_conf_ptr = new RangedTaskConf(low, high);
            StopWatch oparation_stop_watch;
            bool ok = TASK_APC.PublishHeapPtrWithAdaptiveBackoff(reinterpret_cast<void*>(range_task_conf_ptr));
            uint64_t elepsed_us = oparation_stop_watch.ElapsedMicroSecond();
            producer_publish_timer.AddSamplMicroSecond(elepsed_us);
            if (!ok)
            {
                std::cerr<< "Producer ID : " << producer_id << "failed to publish the pointer of range from->" << low << " to->" << high << "\n";
                delete range_task_conf_ptr;
            }
        }
        uint64_t total_loop_us = local_stop_watch_producer.ElapsedMicroSecond();
        producer_done.fetch_add(1, std::memory_order_release);
        sort_cv.notify_all();
        collector_cv.notify_all();
        std::cout << "Producer ID : " << producer_id << "-> Done || Elapsed Time : " << total_loop_us << "us\n";
    };

    std::atomic<bool> collector_running{true};
    std::atomic<bool> stop_collector{false};
    std::thread collector_thread([&](){
        std::cout << "Collectot -> Started\n";
        PackedCellContainerManager::ThreadHandlePCCM thread_handle = apc_mannager_prime_test.RegisterAPCThread();
        while(true)
        {
            bool did_work = false;
            size_t capacity_of_result_apc = RESULT_APC.GetOrSetContainerCapacity();
            if (capacity_of_result_apc == 0)
            {
                break;
            }
            for (size_t idx = 0; idx < capacity_of_result_apc; idx++)
            {
                RelOffsetMode ptr_position;
                auto maybe_ptr_unsigned = RESULT_APC.TryAssemblePairedPtr_(idx, ptr_position);//TryAssemblePairedPtr_ directly returns the position via ptr_position
                if (!maybe_ptr_unsigned)
                {
                    continue;
                }
                //why I am just checking head-half??
                size_t head_idx;
                if (ptr_position == RelOffsetMode::REL_OFFSET_HEAD_PTR)
                {
                    head_idx = idx;
                }
                else
                {
                    head_idx = (idx + capacity_of_result_apc - 1) % capacity_of_result_apc;
                }
                uint64_t ptr_value = *maybe_ptr_unsigned;
                auto vector_ptr = reinterpret_cast<std::vector<uint64_t>*>(ptr_value);
                if (!vector_ptr)
                {
                    RESULT_APC.RetirePairedPtrAtIdx_(head_idx); 
                    did_work = true;
                    continue;
                }
                StopWatch marge_stop_watch;
                //why lock how to efficiently use APC?
                {
                    std::lock_guard<std::mutex> lok(collector_mutex);
                    all_primes_array.insert(all_primes_array.end(), vector_ptr->begin(), vector_ptr->end());
                }
                uint64_t marge_us = marge_stop_watch.ElapsedMicroSecond();
                collector_marge_timer.AddSamplMicroSecond(marge_us);
                //is there any way to autometically rfetire based on 16 bit clock by mannager ??
                RESULT_APC.RetirePairedPtrAtIdx_(head_idx);
                published_to_result.fetch_add(1, std::memory_order_relaxed);
                did_work = true;
            }//end scan
            bool no_sort_task = false;
            //why lock how to efficiently use APC?
            {
                std::lock_guard<std::mutex>lok(sort_queue_mutex);
                no_sort_task = sort_task_queue.empty();
            }
            bool result_apc_has_any = false;
            size_t cur_cap_of_result_apc = RESULT_APC.GetOrSetContainerCapacity();
            //why another scan??
            for (size_t i = 0; i < cur_cap_of_result_apc && result_apc_has_any; i++)
            {
                RelOffsetMode current_ptr_position_getter;
                if (RESULT_APC.TryAssemblePairedPtr_(i, current_ptr_position_getter))
                {
                    result_apc_has_any = true;
                    break;
                }
                
            }
            
            if (stop_collector.load(MoLoad_) && !result_apc_has_any && no_sort_task)
            {
                std::cout << "Collector shutting down condition met-----\n";
                break;
            }
            std::unique_lock<std::mutex> uniqlock(mutex_of_collector_cv);
            collector_cv.wait_for(uniqlock, std::chrono::milliseconds(25));
        }
        apc_mannager_prime_test.UnRegisterAPCThread(thread_handle);
        collector_running.store(false);
        std::cout << "Collector :: Stoped \n";
    });

    auto consumer_function = [&](int worker_id)
    {
        std::cout << "Consumer ID : " << worker_id << " started\n";
        PackedCellContainerManager::ThreadHandlePCCM thread_handle_consumer = apc_mannager_prime_test.RegisterAPCThread();
        size_t task_apc_capacity = TASK_APC.GetOrSetContainerCapacity();
        if (task_apc_capacity == 0)
        {
            apc_mannager_prime_test.UnRegisterAPCThread(thread_handle_consumer);
            std::cout << "Consumer ID " << worker_id << "No Work -> Early EXIT\n";
            return;
        }
        size_t scan_offset = worker_id % task_apc_capacity;
        size_t local_loop_counter = 0;
        while (true)
        {
            bool did_work = false;
            std::vector<uint64_t>* steal_task_ptr = nullptr;
            {
                std::lock_guard<std::mutex>lok(sort_queue_mutex);
                if (!sort_task_queue.empty())
                {
                    steal_task_ptr = sort_task_queue.front();
                    sort_task_queue.pop_front();
                }
            }
            if (steal_task_ptr)
            {
                StopWatch consumer_stopwatch;
                std::sort(steal_task_ptr->begin(), steal_task_ptr->end());
                steal_task_ptr->erase(std::unique(steal_task_ptr->begin(), steal_task_ptr->end()), steal_task_ptr->end());
                bool published_ok = RESULT_APC.PublishHeapPtrWithAdaptiveBackoff(reinterpret_cast<void*>(steal_task_ptr));
                uint64_t publish_us = consumer_stopwatch.ElapsedMicroSecond();
                consumer_result_publish_timer.AddSamplMicroSecond(publish_us);
                if (!published_ok)
                {
                    {
                        std::lock_guard<std::mutex>lok(collector_mutex);
                        all_primes_array.insert(all_primes_array.end(), steal_task_ptr->begin(), steal_task_ptr->end());
                    }
                    delete steal_task_ptr;
                }
                else
                {
                    collector_cv.notify_one();
                }
                did_work = true;
            }
            for (size_t probe = 0; probe < task_apc_capacity; probe++)
            {
                size_t idx = (scan_offset + probe) % task_apc_capacity;
                RelOffsetMode current_position;
                auto maybe_ptr = TASK_APC.TryAssemblePairedPtr_(idx, current_position);
                if (!maybe_ptr)
                {
                    packed64_t observed = TASK_APC.BackingPtr ? TASK_APC.BackingPtr[idx].load(MoLoad_) : 0;
                    apc_mannager_prime_test.GetCellsAdaptiveBackoffFromManager(observed);
                    continue;
                }
                size_t head_idx;
                if (current_position == RelOffsetMode::REL_OFFSET_HEAD_PTR)
                {
                    head_idx = idx;
                }
                else if(current_position == RelOffsetMode::RELOFFSET_TAIL_PTR)
                {
                    head_idx = (idx + task_apc_capacity - 1) % task_apc_capacity;
                }
                else
                {
                    continue;
                }
                
                packed64_t current_head = TASK_APC.BackingPtr[head_idx].load(MoLoad_);
                tag8_t locality_of_current_head = PackedCell64_t::ExtractLocalityFromPacked(current_head);
                if(locality_of_current_head != ST_PUBLISHED)
                {
                    continue;
                }


                // ST_CLAIMED -- Invarint not working have to fix only ST_PUBLISHED working --error 01
                packed64_t claimed_current_head = PackedCell64_t::SetLocalityInPacked(current_head, ST_CLAIMED);
                packed64_t expected = current_head;
                if (!TASK_APC.BackingPtr[head_idx].compare_exchange_strong(expected, claimed_current_head, EXsuccess_, EXfailure_))
                {
                    continue;
                }
                size_t tail_idx = (head_idx + 1) % task_apc_capacity;
                packed64_t current_tail = TASK_APC.BackingPtr[tail_idx].load(MoLoad_);
                int attempts = 0;
                bool tail_claimed = false;
                while (attempts++ < MAX_TAIL_ATTEMPTS)
                {
                    packed64_t claimed_tail = PackedCell64_t::SetLocalityInPacked(current_tail, ST_CLAIMED);
                    if (TASK_APC.BackingPtr[tail_idx].compare_exchange_strong(current_tail, claimed_tail, EXsuccess_, EXfailure_))
                    {
                        tail_claimed = true;
                        break;
                    }
                    apc_mannager_prime_test.GetCellsAdaptiveBackoffFromManager(current_tail);
                }                
                // ----- end ---error 01
                if (!tail_claimed)
                {
                    TASK_APC.BackingPtr[head_idx].store(current_head, MoStoreSeq_);
                    TASK_APC.BackingPtr[head_idx].notify_all();
                    continue;
                }

                RelOffsetMode position_of_ptr_2;
                auto maybe_ptr_2 = TASK_APC.TryAssemblePairedPtr_(head_idx, position_of_ptr_2);
                if (!maybe_ptr_2)
                {
                    packed64_t idle = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
                    TASK_APC.BackingPtr[head_idx].store(idle, MoStoreSeq_);
                    TASK_APC.BackingPtr[tail_idx].store(idle, MoStoreSeq_);
                    TASK_APC.BackingPtr[head_idx].notify_all();
                    TASK_APC.BackingPtr[tail_idx].notify_all();
                    TASK_APC.OccupancyAddSubOrGetAfterChange(1);
                    did_work = true;
                    continue;
                }
                uint64_t ptr_value_unsigned = *maybe_ptr_2;
                RangedTaskConf* range_task_ptr = reinterpret_cast<RangedTaskConf*>(ptr_value_unsigned);
                if (range_task_ptr == nullptr)
                {
                    TASK_APC.RetirePairedPtrAtIdx_(head_idx);
                    retired_from_task.fetch_add(1, std::memory_order_relaxed);
                    did_work = true;
                    goto CONSUMER_EXIT;
                }
                StopWatch compute_stop_watch;
                std::vector<uint64_t>* local_primes = new std::vector<uint64_t>();
                SegmentedSiveSimplified(range_task_ptr->StartingPoint, range_task_ptr->EndPoint, *local_primes);
                uint64_t compute_us = compute_stop_watch.ElapsedMicroSecond();
                consumer_compute_timer.AddSamplMicroSecond(compute_us);
                task_processed.fetch_add(1, std::memory_order_relaxed);
                TASK_APC.RetirePairedPtrAtIdx_(head_idx);
                
                if (local_primes->empty())
                {
                    delete local_primes;
                }
                
                if (local_primes->size() > SORT_OFFLOAD_SIZE)
                {
                    {
                        std::lock_guard<std::mutex>lok(collector_mutex);
                        all_primes_array.insert(all_primes_array.end(), local_primes->begin(), local_primes->end());
                    }
                    sort_cv.notify_one();
                    collector_cv.notify_one();
                }
                else
                {
                    StopWatch publishing_consumer_stop_watch;
                    bool published_ok = RESULT_APC.PublishHeapPtrWithAdaptiveBackoff(reinterpret_cast<void*>(local_primes));
                    uint64_t publishing_consumer_us = publishing_consumer_stop_watch.ElapsedMicroSecond();
                    consumer_result_publish_timer.AddSamplMicroSecond(publishing_consumer_us);
                    if (!published_ok)
                    {
                        {
                            std::lock_guard<std::mutex> lok(collector_mutex);
                            all_primes_array.insert(all_primes_array.end(), local_primes->begin(), local_primes->end());
                        }
                        delete local_primes;
                    }
                    else
                    {
                        published_to_result.fetch_add(1, std::memory_order_relaxed);
                        collector_cv.notify_one();
                    }
                }
                did_work = true;
            }
            if (!did_work)
            {
                std::unique_lock<std::mutex> uniqlock(collector_mutex);
                if (sort_task_queue.empty())
                {
                    sort_cv.wait_for(uniqlock, std::chrono::milliseconds(10));
                }
            }
            //what dose 0x3f means and why??
            if ((++local_loop_counter & 0x3f) == 0)
            {
                std::cout << "consumer " << worker_id << " heartbeat loops=" << local_loop_counter
                          << " retired_tasks=" << retired_from_task.load()
                          << " producers_done=" << producer_done.load() << "\n";
            }
        }//end consumer
CONSUMER_EXIT :
        apc_mannager_prime_test.UnRegisterAPCThread(thread_handle_consumer);
        std::cout << "Consumer stopped ID : " << worker_id << "\n";
    };

    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < PRODUCER_COUNT; producer++)
    {
        producers.emplace_back(producer_function, producer);
    }

    unsigned number_of_consumers = CONSUMER_COUNT;
    if (number_of_consumers == 0)
    {
        number_of_consumers = 4;
    }
    std::vector<std::thread> consumers;
    for (unsigned consumer = 0; consumer < number_of_consumers; consumer++)
    {
        consumers.emplace_back(consumer_function, static_cast<int>(consumer));
    }

    for (auto& thrd : producers)
    {
        thrd.join();
    }
    
    std::cout<< "All producers joined\n";

    for (unsigned i = 0; i < number_of_consumers; i++)
    {
        bool ok = TASK_APC.PublishHeapPtrWithAdaptiveBackoff(reinterpret_cast<void*>(nullptr));
        int retry_null_ptr = 0;
        while (!ok && retry_null_ptr < MAX_TAIL_ATTEMPTS)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            ok = TASK_APC.PublishHeapPtrWithAdaptiveBackoff(reinterpret_cast<void*>(nullptr));
            ++retry_null_ptr;
        }

        if (!retry_null_ptr)
        {
            std::cerr << "Warning failed to publish sentinal " << i << " ->TASK_APC\n";
        }
    }
    
    sort_cv.notify_all();

    for (auto& thrd : consumers)
    {
        thrd.join();
    }
    std::cout << "All consumers joined \n";

    stop_collector.store(true, std::memory_order_release);
    collector_cv.notify_all();
    sort_cv.notify_all();

    size_t wait_itars = 0;
    while (collector_running.load() && wait_itars < 1000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++wait_itars;
    }
    if (collector_running.load())
    {
        std::cout << "WARNING :: Collector still Running, forcing notify and joining....\n";
        collector_cv.notify_all();
    }
    collector_thread.join();

    {
        //marge sort
        std::cout << "Final Marge Sort Begain Total Items Before :: " << all_primes_array.size() << "\n";
        std::sort(all_primes_array.begin(), all_primes_array.end());
        all_primes_array.erase(std::unique(all_primes_array.begin(), all_primes_array.end()), all_primes_array.end());
    }
    
    // print some sample primes
    std::cout << "Found number of Primes : " << all_primes_array.size() << "\n";
    for (size_t i = 0; i < std::min<size_t>(20u, all_primes_array.size()); ++i)
    {
        std::cout << "Position : " << i << " Prime number : " << all_primes_array[i] << "\n";
    }

    // Print instrumentation summary (human-friendly)
    auto print_us = [](uint64_t us) {
        std::ostringstream ss;
        ss << us << " us (" << std::fixed << std::setprecision(3) << static_cast<double>(us) / 1e6 << " s)";
        return ss.str();
    };

    std::cout << "\n==== Instrumentation Summary ====\n";
    std::cout << "Total runtime (steady): " << print_us(runtime_stop_watch.ElapsedMicroSecond()) << "\n";

    std::cout << "\nProducer publish stats:\n";
    std::cout << "  samples: " << producer_publish_timer.GetSampleCount()
              << "  total: " << print_us(producer_publish_timer.GetTotalMicroSeconds())
              << "  avg: " << std::fixed << std::setprecision(3) << producer_publish_timer.AvarageMiliSecond() << " us"
              << "  max: " << print_us(producer_publish_timer.GetMaxMicroSecond()) << "\n";

    std::cout << "\nConsumer compute stats (per-task):\n";
    std::cout << "  tasks processed: " << task_processed.load()
              << "  total compute: " << print_us(consumer_compute_timer.GetTotalMicroSeconds())
              << "  avg compute: " << std::fixed << std::setprecision(3) << consumer_compute_timer.AvarageMiliSecond() << " us"
              << "  max compute: " << print_us(consumer_compute_timer.GetMaxMicroSecond()) << "\n";

    std::cout << "\nConsumer result publish stats:\n";
    std::cout << "  samples: " << consumer_result_publish_timer.GetSampleCount()
              << "  total: " << print_us(consumer_result_publish_timer.GetTotalMicroSeconds())
              << "  avg: " << std::fixed << std::setprecision(3) << consumer_result_publish_timer.AvarageMiliSecond() << " us"
              << "  max: " << print_us(consumer_result_publish_timer.GetMaxMicroSecond()) << "\n";

    std::cout << "\nCollector merge stats:\n";
    std::cout << "  samples: " << collector_marge_timer.GetSampleCount()
              << "  total: " << print_us(collector_marge_timer.GetTotalMicroSeconds())
              << "  avg: " << std::fixed << std::setprecision(3) << collector_marge_timer.AvarageMiliSecond() << " us"
              << "  max: " << print_us(collector_marge_timer.GetMaxMicroSecond()) << "\n";

    std::cout << "\nAPC counts:\n";
    std::cout << "  published_to_result: " << published_to_result.load() << "\n";
    std::cout << "  retired_from_task: " << retired_from_task.load() << "\n";

    // housekeeping
    TASK_APC.FreeAll();
    RESULT_APC.FreeAll();

    apc_mannager_prime_test.StopPCCManager();
    std::cout << "Adaptive Packed Cell Container :: Prime Test ::DONE\n";
    return 0;

}