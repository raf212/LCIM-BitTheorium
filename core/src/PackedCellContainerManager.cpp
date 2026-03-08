#include  "PackedCellContainerManager.hpp"

namespace PredictedAdaptedEncoding
{
    inline PackedCellContainerManager::PackedCellContainerManager() :
        ThreadTableCapacity_(MaxThreads_),
        ThreadNextIdx_(ThreadTableCapacity_),
        ThreadEpochArrayPtr_(new std::atomic<uint64_t>[ThreadTableCapacity_]),
        ThreadWaitSlotArrayPtr_(new std::atomic<uint64_t>[ThreadTableCapacity_]),
        MasterClockConfAPCManager_(Timer48APCManager_, 0),
        AdaptiveBackOffOfAPCManager_(AtomicAdaptiveBackoff::PCBCfg{}, PackedMode::MODE_VALUE32)
    {
        for (size_t i = 0; i < ThreadTableCapacity_; i++)
        {
            ThreadEpochArrayPtr_[i].store(THREAD_SENTINEL_, MoStoreUnSeq_);
            ThreadWaitSlotArrayPtr_[i].store(NO_VAL, MoStoreUnSeq_);
            ThreadNextIdx_[i].store(i+1, MoStoreUnSeq_);
        }
        ThreadNextIdx_[ThreadTableCapacity_ - 1].store(SIZE_MAX, MoStoreUnSeq_);
        ThreadFreelistHead_.store(0, MoStoreUnSeq_);
        std::atexit([]()
        {
            try 
            {
                PackedCellContainerManager::Instance().StopPCCManager();
            }
            catch (...)
            {

            }
        }
        );
    }

    inline PackedCellContainerManager::~PackedCellContainerManager()
    {
        StopPCCManager();
        if (UseNodePool_)
        {
            NodeOfAdaptivePackedCellContainer_* node_of_apc_ptr = NodePoolHeadOfAPC_.load(MoLoad_);
            while (node_of_apc_ptr)
            {
                NodeOfAdaptivePackedCellContainer_* next_node_of_apc_ptr = node_of_apc_ptr->StackNextPtr.load(std::memory_order_relaxed);
                ::operator delete(static_cast<void*>(node_of_apc_ptr));
                node_of_apc_ptr = next_node_of_apc_ptr;
            }
            
        }
        
    }

    inline void PackedCellContainerManager::StartPCCManager()
    {
        bool expect = false;
        if (!RunningManager_.compare_exchange_strong(expect, true, std::memory_order_acq_rel))
        {
            return;
        }
        ManagerThread_ = std::thread([this]
        {
            ManagerManinLoop_();
        }
        );
    }

    inline void PackedCellContainerManager::StopPCCManager()
    {
        bool expect = true;
        if (!RunningManager_.compare_exchange_strong(expect, false, std::memory_order_acq_rel))
        {
            return;
        }
        ManagerWakeCounter_.fetch_add(1, std::memory_order_release);
        ManagerWakeCounter_.notify_one();
        if (ManagerThread_.joinable())
        {
            ManagerThread_.join();
        }
        
    }

    size_t PackedCellContainerManager::PopFreeThreadIndex_() noexcept
    {
        size_t head_of_thread_freeList = ThreadFreelistHead_.load(MoLoad_);
        while (head_of_thread_freeList != SIZE_MAX)
        {
            size_t next_thread_idx = ThreadNextIdx_[head_of_thread_freeList].load(std::memory_order_relaxed);
            if (ThreadFreelistHead_.compare_exchange_strong(head_of_thread_freeList, next_thread_idx, EXsuccess_, EXfailure_))
            {
                return head_of_thread_freeList;
            }            
        }
        return SIZE_MAX;
    }

    void PackedCellContainerManager::PushFreeThreadIndex_(size_t idx) noexcept
    {
        size_t head_thread_freelist = ThreadFreelistHead_.load(MoLoad_);
        do
        {
            ThreadNextIdx_[idx].store(head_thread_freelist, MoStoreUnSeq_);
        } while (!ThreadFreelistHead_.compare_exchange_weak(head_thread_freelist, idx, std::memory_order_release, std::memory_order_relaxed));
        
    }

    size_t PackedCellContainerManager::AllocateThreadSlots_() noexcept
    {
        size_t idx = PopFreeThreadIndex_();
        if (idx == SIZE_MAX)
        {
            return SIZE_MAX;
        }
        ThreadWaitSlotArrayPtr_[idx].store(NO_VAL, MoStoreSeq_);
        ThreadEpochArrayPtr_[idx].store(THREAD_SENTINEL_, MoStoreSeq_);
        return idx;
    }

    void PackedCellContainerManager::FreeThreadSlots_(size_t idx) noexcept
    {
        if (idx == SIZE_MAX || idx >= ThreadTableCapacity_)
        {
            return;
        }
        ThreadWaitSlotArrayPtr_[idx].store(NO_VAL, MoStoreSeq_);
        ThreadEpochArrayPtr_[idx].store(THREAD_SENTINEL_, MoStoreSeq_);
        PushFreeThreadIndex_(idx);
    }


} // namespace PredictedAdaptedEncoding
