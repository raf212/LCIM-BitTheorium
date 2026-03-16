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
    decltype(std::chrono::steady_clock::now()) StartStopWatch_;
    StopWatch() noexcept :
        StartStopWatch_(std::chrono::steady_clock::now())
    {}
    void ResetStopWatch() noexcept 
    {
        StartStopWatch_ = std::chrono::steady_clock::now();
    }
    uint64_t ElapsedMicroSecond() const noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - StartStopWatch_).count()
        );
    }
};


int main()
{
    std::ios::sync_with_stdio(true);
    std ::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::cout << "AdaptivePackedCellContainer + Manager + AtomicAdaptiveBackoff :: Prime number find and sort test\n";
    std::cout << "Finding Upto Element : " << NTH_ELEMENT << ", Eack Block Size : " << BLOCK_SIZE << "\n";
    PackedCellContainerManager& apc_mannager_prime_test = PackedCellContainerManager::Instance();
    apc_mannager_prime_test.StartPCCManager();

    std::vector<std::pair<uint64_t, uint64_t>> task_ranges;
    for (size_t lo = FIRST_PRIME; lo <= NTH_ELEMENT; lo += BLOCK_SIZE)
    {
        uint64_t hi = std::min(NTH_ELEMENT, lo + BLOCK_SIZE - 1);
        task_ranges.emplace_back(lo, hi);
    }
    
    size_t num_of_tasks = task_ranges.size();
    size_t apc_capacity = std::max<size_t>(MIN_APC_CAPACITY, num_of_tasks * 2 + 16);
    AdaptivePackedCellContainer TASK_APC;
    ContainerConf task_cfg;
    task_cfg.ProducerBlockSize = 64;
    task_cfg.MaxTlsCandidates = 1024;
    task_cfg.InitialMode = PackedMode::MODE_VALUE32;
    TASK_APC.InitOwned(apc_capacity, REL_NODE0, task_cfg);
    TASK_APC.SetManagerForGlobalAPC(&apc_mannager_prime_test);
    apc_mannager_prime_test.RequestForReclaimationOfTheAdaptivePackedCellContainer(&TASK_APC);

    AdaptivePackedCellContainer RESULT_APC;
    ContainerConf result_cfg = task_cfg;
    result_cfg.ProducerBlockSize = 32;
    RESULT_APC.InitOwned(RESULT_APC_CAPACITY, REL_NODE0, result_cfg);
    RESULT_APC.SetManagerForGlobalAPC(&apc_mannager_prime_test);
    apc_mannager_prime_test.RequestForReclaimationOfTheAdaptivePackedCellContainer(&RESULT_APC);


    std::mutex collector_mutex;
    std::vector<uint64_t> all_primes_array;

    std::mutex sort_queue_mutex;
    std::deque<std::vector<uint64_t>*> sort_task_queue;
    std::condition_variable sort_cv;
    std::mutex mutex_of_collector_cv;
    std::condition_variable collector_cv;
    std::atomic<size_t> producer_done{0};
    std::atomic<bool> stop_collecting{false};

    auto producer_function = [&](unsigned producer_id)
    {
        size_t start_idx = (task_ranges.size() * producer_id) / PRODUCER_COUNT;
        size_t end_idx = (task_ranges.size() * (producer_id + 1)) / PRODUCER_COUNT;
        for (size_t i = start_idx ; i < end_idx; i++)
        {
            uint64_t high = task_ranges[i].second;
            uint64_t low = task_ranges[i].first;
            RangedTaskConf* range_task_conf_ptr = new RangedTaskConf(low, high);
            bool ok = TASK_APC.PublishHeapPtrWithAdaptiveBackoff(reinterpret_cast<void*>(range_task_conf_ptr));
            if (!ok)
            {
                std::cerr<< "Producer ID : " << producer_id << "failed to publish the pointer of range from->" << low << " to->" << high << "\n";
                delete range_task_conf_ptr;
            }
        }
        producer_done.fetch_add(1, std::memory_order_release);
    };

    std::atomic<bool> collector_running{true};
    std::thread collector_thread([&](){
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
                size_t head_idx = 0;
                if (ptr_position == RelOffsetMode::REL_OFFSET_HEAD_PTR)
                {
                    head_idx = idx;
                }
                auto maybe_ptr_2_unsigned = RESULT_APC.TryAssemblePairedPtr_(head_idx, ptr_position);
                if (!maybe_ptr_2_unsigned)
                {
                    continue;
                }
                uint64_t ptr_value = *maybe_ptr_2_unsigned;
                auto vector_ptr = reinterpret_cast<std::vector<uint64_t>*>(ptr_value);
                if (!vector_ptr)
                {
                    RESULT_APC.RetirePairedPtrAtIdx_(head_idx); 
                    did_work = true;
                    continue;
                }
                
                //why lock how to efficiently use APC?
                {
                    std::lock_guard<std::mutex> lok(collector_mutex);
                    all_primes_array.insert(all_primes_array.end(), vector_ptr->begin(), vector_ptr->end());
                }
                //is there any way to autometically rfetire based on 16 bit clock by mannager ??
                RESULT_APC.RetirePairedPtrAtIdx_(head_idx);
                did_work = true;
            }//end scan
            size_t occupancy_of_task_apc = TASK_APC.OccupancyAddSubOrGetAfterChange();
            size_t occupency_of_result_apc = RESULT_APC.OccupancyAddSubOrGetAfterChange();
            bool all_produced = (producer_done.load(MoLoad_) >= PRODUCER_COUNT);
            bool no_sort_task = false;
            //why lock how to efficiently use APC?
            {
                std::lock_guard<std::mutex>lok(sort_queue_mutex);
                no_sort_task = sort_task_queue.empty();
            }
            if (all_produced && occupancy_of_task_apc == 0 && occupency_of_result_apc == 0 && no_sort_task)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));//default sleep
        }
        apc_mannager_prime_test.UnRegisterAPCThread(thread_handle);
        collector_running.store(false);
    });

    auto consumer_function = [&](int worker_id)
    {
        PackedCellContainerManager::ThreadHandlePCCM thread_handle_consumer = apc_mannager_prime_test.RegisterAPCThread();
        size_t task_apc_capacity = TASK_APC.GetOrSetContainerCapacity();
        if (task_apc_capacity == 0)
        {
            apc_mannager_prime_test.UnRegisterAPCThread(thread_handle_consumer);
            return;
        }
        size_t scan_offset = worker_id % task_apc_capacity;
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
                std::sort(steal_task_ptr->begin(), steal_task_ptr->end());
                steal_task_ptr->erase(std::unique(steal_task_ptr->begin(), steal_task_ptr->end()), steal_task_ptr->end());
                bool published_ok = RESULT_APC.PublishHeapPtrWithAdaptiveBackoff(reinterpret_cast<void*>(steal_task_ptr));
                if (!published_ok)
                {
                    {
                        std::lock_guard<std::mutex>lok(collector_mutex);
                        all_primes_array.insert(all_primes_array.end(), steal_task_ptr->begin(), steal_task_ptr->end());
                    }
                    delete steal_task_ptr;
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
                packed64_t claimed_current_head = PackedCell64_t::SetLocalityInPacked(current_head, ST_CLAIMED);
                packed64_t expected = current_head;
                if (!TASK_APC.BackingPtr[head_idx].compare_exchange_strong(expected, claimed_current_head, EXsuccess_, EXfailure_))
                {
                    continue;
                }
                size_t tail_idx = (head_idx + 1) % task_apc_capacity;
                packed64_t current_tail = TASK_APC.BackingPtr[tail_idx].load(MoLoad_);
                packed64_t claimed_tail = PackedCell64_t::SetLocalityInPacked(current_tail, ST_CLAIMED);
                while (!TASK_APC.BackingPtr[tail_idx].compare_exchange_strong(current_tail, claimed_tail, EXsuccess_, EXfailure_))
                {
                    apc_mannager_prime_test.GetCellsAdaptiveBackoffFromManager(current_tail);
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
                    continue;
                }
                uint64_t ptr_valu_unsigned = *maybe_ptr_2;
                RangedTaskConf* range_task_ptr = reinterpret_cast<RangedTaskConf*>(ptr_valu_unsigned);
                if (!range_task_ptr)
                {
                    TASK_APC.RetirePairedPtrAtIdx_(head_idx);
                    did_work = true;
                    continue;
                }

                std::vector<uint64_t>* local_primes = new std::vector<uint64_t>();
                SegmentedSiveSimplified(range_task_ptr->StartingPoint, range_task_ptr->EndPoint, *local_primes);
                TASK_APC.RetirePairedPtrAtIdx_(head_idx);
                if (local_primes->size() > SORT_OFFLOAD_SIZE)
                {
                    std::lock_guard<std::mutex>lok(collector_mutex);
                    all_primes_array.insert(all_primes_array.end(), local_primes->begin(), local_primes->end());
                }
                delete local_primes;
            }
            bool all_produced = (producer_done.load(MoLoad_) >= PRODUCER_COUNT);
            size_t occupency_of_task_apc = TASK_APC.OccupancyAddSubOrGetAfterChange();
            {
                std::lock_guard<std::mutex> lok(sort_queue_mutex);
                if (all_produced && occupency_of_task_apc == 0 && sort_task_queue.empty())
                {
                    break;
                }
            }
            if (!did_work)
            {
                packed64_t sample = 0;
                if (TASK_APC.BackingPtr && TASK_APC.GetOrSetContainerCapacity() > 0)
                {
                    sample = TASK_APC.BackingPtr[scan_offset].load(MoLoad_);
                }
                apc_mannager_prime_test.GetCellsAdaptiveBackoffFromManager(sample);
            }
        }
        apc_mannager_prime_test.UnRegisterAPCThread(thread_handle_consumer);
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

    for (auto& thrd : consumers)
    {
        thrd.join();
    }

    std::cout << "All consumers joined \n";

    while (collector_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    collector_thread.join();

    {
        //marge sort
        if (all_primes_array.size() > GPU_THRESHOLD)
        {
            std::sort(all_primes_array.begin(), all_primes_array.end());
            all_primes_array.erase(std::unique(all_primes_array.begin(), all_primes_array.end()), all_primes_array.end());
        }
        else
        {
            std::sort(all_primes_array.begin(), all_primes_array.end());
            all_primes_array.erase(std::unique(all_primes_array.begin(), all_primes_array.end()), all_primes_array.end());
        }
    }
    
    std::cout << "Found number of Primes : " << all_primes_array.size() << "\n";
    for (size_t i = 0; i < std::min<size_t>(20u, all_primes_array.size()); i++)
    {
        std::cout << "Position : " << i << "Prime number : " << all_primes_array[i] << "\n";
    }
    TASK_APC.FreeAll();
    RESULT_APC.FreeAll();

    apc_mannager_prime_test.StopPCCManager();
    std::cout << "Adaptive Packed Cell Container :: Prime Test ::DONE" << "\n";
    return 0;

}