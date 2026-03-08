#include  "PackedCellContainerManager.hpp"

namespace PredictedAdaptedEncoding
{
#define BIT_PATTERN_THREAD_TOKEN_GENERATOR 0xA5A5A5A5u

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

    PackedCellContainerManager::ThreadHandlePCCM PackedCellContainerManager::RegisterAPCThread()
    {
        ThreadHandlePCCM thread_handle;
        size_t idx = AllocateThreadSlots_();
        if (idx == SIZE_MAX)
        {
            return thread_handle;
        }
        thread_handle.QSBRIdx = idx;
        thread_handle.WaitSlotPtr = &ThreadWaitSlotArrayPtr_[idx];
        thread_handle.NodeTokenOfAPC =  static_cast<uint64_t>(idx) ^ BIT_PATTERN_THREAD_TOKEN_GENERATOR; //xor
        ManagerWakeCounter_.fetch_add(1, std::memory_order_release);
        ManagerWakeCounter_.notify_one();
        return thread_handle;
    }

    void PackedCellContainerManager::UnRegisterAPCThread(const ThreadHandlePCCM& thread_handle) noexcept
    {
        if (thread_handle.QSBRIdx == SIZE_MAX)
        {
            return;
        }
        FreeThreadSlots_(thread_handle.QSBRIdx);
        size_t unregister_counts = UnregistersSinceCompact_.fetch_add(1, std::memory_order_acq_rel);
        if (unregister_counts >= CompactionTriggerThreshold_)
        {
            ManagerWakeCounter_.fetch_add(1, std::memory_order_release);
            ManagerWakeCounter_.notify_one();
        }
    }

    void PackedCellContainerManager::EnterCriticlContainer(const ThreadHandlePCCM& thread_handle) noexcept
    {
        if (thread_handle.QSBRIdx == SIZE_MAX)
        {
            return;
        }
        uint64_t global_epoch = GlobalEpoch_.load(MoLoad_);
        ThreadEpochArrayPtr_[thread_handle.QSBRIdx].store(global_epoch, MoStoreSeq_);        
    }

    void PackedCellContainerManager::ExtitCriticalContainer(const ThreadHandlePCCM& thread_handle) noexcept
    {
        if (thread_handle.QSBRIdx == SIZE_MAX)
        {
            return;
        }
        ThreadEpochArrayPtr_[thread_handle.QSBRIdx].store(THREAD_SENTINEL_, MoStoreSeq_);
    }

    void PackedCellContainerManager::NotifySlotIdxOfAPC(size_t idx, uint64_t thread_token) noexcept
    {
        if (idx >= ThreadTableCapacity_)
        {
            return;
        }
        ThreadWaitSlotArrayPtr_[idx].store(thread_token, MoStoreSeq_);
        ThreadWaitSlotArrayPtr_[idx].notify_one();
    }

    void PackedCellContainerManager::NotifyAllActiveAPCThreads(uint64_t thread_token) noexcept
    {
        for (size_t i = 0; i < ThreadTableCapacity_; i++)
        {
            uint64_t epoch = ThreadEpochArrayPtr_[i].load(MoLoad_);
            if (epoch != THREAD_SENTINEL_)
            {
                NotifySlotIdxOfAPC(i, thread_token);
            }
        }
    }

    void PackedCellContainerManager::UsePreAllocatedNodePoolOfAdaptivePackedCellContainer(size_t pool_size_of_apc)
    {
        if (pool_size_of_apc == 0)
        {
            return;
        }
        UseNodePool_ = true;
        for (size_t i = 0; i < pool_size_of_apc; i++)
        {
            NodeOfAdaptivePackedCellContainer_* node_of_apc_ptr = static_cast<NodeOfAdaptivePackedCellContainer_*>(::operator new(sizeof(NodeOfAdaptivePackedCellContainer_)));
            node_of_apc_ptr->APCContainerPtr = nullptr;
            node_of_apc_ptr->ReclaimationNeededAPC.store(NO_VAL, MoStoreUnSeq_);
            node_of_apc_ptr->RequestedBranchedAPC.store(NO_VAL, MoStoreUnSeq_);
            node_of_apc_ptr->DeadAPC.store(NO_VAL, MoStoreUnSeq_);
            node_of_apc_ptr->RegistryNextPtr = nullptr;
            node_of_apc_ptr->StackNextPtr.store(nullptr, MoStoreUnSeq_);
            node_of_apc_ptr->DebugId = reinterpret_cast<uint64_t>(node_of_apc_ptr);
            NodeOfAdaptivePackedCellContainer_* head_of_apc_node_pool_ptr = NodePoolHeadOfAPC_.load(MoLoad_);
            do
            {
                node_of_apc_ptr->StackNextPtr.store(head_of_apc_node_pool_ptr, MoStoreUnSeq_);
            } while (!NodePoolHeadOfAPC_.compare_exchange_weak(head_of_apc_node_pool_ptr, node_of_apc_ptr, std::memory_order_release, std::memory_order_relaxed));
            
        }
    }
    
} // namespace PredictedAdaptedEncoding
