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






} // namespace PredictedAdaptedEncoding
