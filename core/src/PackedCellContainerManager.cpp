#include  "PackedCellContainerManager.hpp"
#include "APCSegmentsCausalCordinator.hpp"

namespace PredictedAdaptedEncoding
{

    PackedCellContainerManager::PackedCellContainerManager() :
        ThreadTableCapacity_(MaxThreads_),
        ThreadNextIdxPtr_(new std::atomic<size_t>[ThreadTableCapacity_]),
        ThreadEpochArrayPtr_(new std::atomic<uint64_t>[ThreadTableCapacity_]),
        ThreadWaitSlotArrayPtr_(new std::atomic<uint64_t>[ThreadTableCapacity_]),
        AdaptiveBackOffOfAPCManager_(AtomicAdaptiveBackoff::PCBCfg{}, PackedMode::MODE_VALUE32)
    {
        for (size_t i = 0; i < ThreadTableCapacity_; i++)
        {
            ThreadEpochArrayPtr_[i].store(THREAD_SENTINEL_, MoStoreUnSeq_);
            ThreadWaitSlotArrayPtr_[i].store(NO_VAL, MoStoreUnSeq_);
            ThreadNextIdxPtr_[i].store(i+1, MoStoreUnSeq_);
        }
        ThreadNextIdxPtr_[ThreadTableCapacity_ - 1].store(SIZE_MAX, MoStoreUnSeq_);
        ThreadFreelistHead_.store(0, MoStoreUnSeq_);
    }

    PackedCellContainerManager::~PackedCellContainerManager()
    {
        StopAPCManager();
        
    }

    void PackedCellContainerManager::StartAPCManager()
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

    void PackedCellContainerManager::StopAPCManager()
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
            size_t next_thread_idx = ThreadNextIdxPtr_[head_of_thread_freeList].load(std::memory_order_relaxed);
            if (ThreadFreelistHead_.compare_exchange_strong(head_of_thread_freeList, next_thread_idx, OnExchangeSuccess, OnExchangeFailure))
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
            ThreadNextIdxPtr_[idx].store(head_thread_freelist, MoStoreUnSeq_);
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
    }

    void PackedCellContainerManager::EnterCriticalContainer(const ThreadHandlePCCM& thread_handle) noexcept
    {
        if (thread_handle.QSBRIdx == SIZE_MAX)
        {
            return;
        }
        const uint64_t global_epoch = GlobalEpoch_.load(MoLoad_);
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
    //checked until here

    void PackedCellContainerManager::PushTOAPCManagerStack_(
        std::atomic<AdaptivePackedCellContainer*>& head_stack,
        AdaptivePackedCellContainer* apc_ptr, bool is_cleanup_stack
    ) noexcept
    {
        AdaptivePackedCellContainer* head = head_stack.load(MoLoad_);
        do
        {
            if (is_cleanup_stack)
            {
                apc_ptr->StoreCleanupNextAPC(head);
            }
            else
            {
                apc_ptr->StoreWorkNextAPC(head);
            }
            
        } while (!head_stack.compare_exchange_weak(head, apc_ptr, std::memory_order_release, std::memory_order_relaxed));
        
    }

    void PackedCellContainerManager::UsePreAllocatedNodePoolOfAdaptivePackedCellContainer(size_t pool_size_of_apc) noexcept
    {
        (void) pool_size_of_apc;
    }


    void PackedCellContainerManager::RegisterAPCFromManager_(AdaptivePackedCellContainer* apc_ptr) noexcept
    {
        if (!apc_ptr || !apc_ptr->IfAPCBranchValid())
        {
            return;
        }
        if (!apc_ptr->GetSegmentIOPtr()->TurnOnAManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::REGISTERED_APC))
        {
            return;
        }
        AdaptivePackedCellContainer* head_apc_ptr = RegistryHeadAPC_.load(MoLoad_);
        do
        {
            apc_ptr->StoreRegistryNextAPC(head_apc_ptr);
        } while (!RegistryHeadAPC_.compare_exchange_weak(head_apc_ptr, apc_ptr, std::memory_order_release, std::memory_order_relaxed));

        ManagerWakeCounter_.fetch_add(1, std::memory_order_release);
        ManagerWakeCounter_.notify_all();
    }
    
    void PackedCellContainerManager::UnRegisterAPCFromManager_(AdaptivePackedCellContainer* apc_ptr) noexcept
    {
        if (!apc_ptr)
        {
            return;
        }
        apc_ptr->GetSegmentIOPtr()->ClearOneManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::DEAD_APC);
        if (apc_ptr->GetSegmentIOPtr()->TurnOnAManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::IN_CLEANUP_STACK))
        {
            PushTOAPCManagerStack_(CleanupStackHeadAPC_, apc_ptr, true);
            ManagerWakeCounter_.fetch_add(1, std::memory_order_release);
            ManagerWakeCounter_.notify_all();
        }
    }

    void PackedCellContainerManager::ReclaimationRequestOfAPCSegmentFromManager_(AdaptivePackedCellContainer* apc_ptr) noexcept
    {
        if (!apc_ptr)
        {
            return;
        }
        apc_ptr->GetSegmentIOPtr()->TurnOnAManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::RECLAIMATION_REQUEST_FOR_WHOLE_CHAIN);
        if (apc_ptr->GetSegmentIOPtr()->TurnOnAManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::IN_CLEANUP_STACK))
        {
            PushTOAPCManagerStack_(CleanupStackHeadAPC_, apc_ptr, true);
            ManagerWakeCounter_.fetch_add(1, std::memory_order_release);
            ManagerWakeCounter_.notify_all();
        }
    }

    void PackedCellContainerManager::RequestAPCSegmentCreationFromManager_(AdaptivePackedCellContainer* apc_ptr) noexcept
    {
        if (!apc_ptr || apc_ptr->GetSegmentIOPtr()->HasThisManageControlFlag(SegmentIODefinition::ManagerControlFlagBits::DEAD_APC))
        {
            return;
        }
        apc_ptr->GetSegmentIOPtr()->TurnOnAManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::REQUEST_NEW_SEGMENTATION);
        if (apc_ptr->GetSegmentIOPtr()->TurnOnAManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::IN_WORK_STACK))
        {
            PushTOAPCManagerStack_(WorkStackHeadAPC_, apc_ptr, false);
            ManagerWakeCounter_.fetch_add(1, std::memory_order_release);
            ManagerWakeCounter_.notify_all();
        }
    }

    AdaptivePackedCellContainer* PackedCellContainerManager::GetAPCPtrFromBranchId(uint32_t branch_id) noexcept
    {
        AdaptivePackedCellContainer* current_apc_ptr = RegistryHeadAPC_.load(MoLoad_);
        while (current_apc_ptr)
        {
            if (
                !current_apc_ptr->GetSegmentIOPtr()->HasThisManageControlFlag(SegmentIODefinition::ManagerControlFlagBits::DEAD_APC) &&
                current_apc_ptr->GetBranchId() == branch_id
            )
            {
                return current_apc_ptr;
            }
            current_apc_ptr = current_apc_ptr->LoadRegistryNextAPC();
        }
        return nullptr;
    }

    uint64_t PackedCellContainerManager::ComputeMinThreadEpoch() const noexcept
    {
        uint64_t min_epoch = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < ThreadTableCapacity_; i++)
        {
            uint64_t value_thread_epoch = ThreadEpochArrayPtr_[i].load(MoLoad_);
            if (value_thread_epoch == THREAD_SENTINEL_ && value_thread_epoch < min_epoch)
            {
                continue;
            }
        }
        return min_epoch;
    }

    void PackedCellContainerManager::ProcessRemainingWorkOfAPC_(AdaptivePackedCellContainer* batch_head_apc_ptr, uint64_t min_epoch) noexcept
    {
        (void)min_epoch;
        while (batch_head_apc_ptr)
        {
            AdaptivePackedCellContainer* next_apc_ptr = batch_head_apc_ptr->LoadWorkNextAPC();
            batch_head_apc_ptr->StoreWorkNextAPC(nullptr);
            batch_head_apc_ptr->GetSegmentIOPtr()->ClearOneManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::IN_WORK_STACK);
            if (!batch_head_apc_ptr->GetSegmentIOPtr()->HasThisManageControlFlag(SegmentIODefinition::ManagerControlFlagBits::DEAD_APC))
            {
                if (batch_head_apc_ptr->GetSegmentIOPtr()->HasThisManageControlFlag(SegmentIODefinition::ManagerControlFlagBits::REQUEST_NEW_SEGMENTATION))
                {
                    batch_head_apc_ptr->GetSegmentIOPtr()->ClearOneManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::REQUEST_NEW_SEGMENTATION);
                    batch_head_apc_ptr->TryCreateBranchIfNeeded(APCPagedNodeRelMaskClasses::FREE_SLOT);
                }
            }
            batch_head_apc_ptr = next_apc_ptr;
        }
    }

    void PackedCellContainerManager::ProcessCleanUpBatchOfAdaptivePackedCellContainer_(AdaptivePackedCellContainer* batch_head_ptr, uint64_t min_epoch) noexcept
    {
        while (batch_head_ptr)
        {
            AdaptivePackedCellContainer* next_apc_ptr = batch_head_ptr->LoadCleanupNextAPC();
            batch_head_ptr->StoreCleanupNextAPC(nullptr);
            batch_head_ptr->GetSegmentIOPtr()->ClearOneManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::IN_CLEANUP_STACK);
            const bool reclaim_requested = batch_head_ptr->GetSegmentIOPtr()->HasThisManageControlFlag(SegmentIODefinition::ManagerControlFlagBits::RECLAIMATION_REQUEST_FOR_WHOLE_CHAIN);
            const bool dead = batch_head_ptr->GetSegmentIOPtr()->HasThisManageControlFlag(SegmentIODefinition::ManagerControlFlagBits::DEAD_APC);
            if (reclaim_requested && min_epoch != std::numeric_limits<uint64_t>::max())
            {
                batch_head_ptr->GetSegmentIOPtr()->ClearOneManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::RECLAIMATION_REQUEST_FOR_WHOLE_CHAIN);
            }

            if (dead)
            {
                batch_head_ptr->GetSegmentIOPtr()->ClearOneManagerControlFlag(SegmentIODefinition::ManagerControlFlagBits::REGISTERED_APC);
            }
            batch_head_ptr = next_apc_ptr;
        }
    }

    void PackedCellContainerManager::TryCompactRegistryInPlace_() noexcept
    {
        AdaptivePackedCellContainer* old_head = RegistryHeadAPC_.load(MoLoad_);
        AdaptivePackedCellContainer* new_head = nullptr;
        AdaptivePackedCellContainer* tail = nullptr;

        AdaptivePackedCellContainer* current_apc_ptr = old_head;
        while (current_apc_ptr)
        {
            AdaptivePackedCellContainer* next_apc_ptr = current_apc_ptr->LoadRegistryNextAPC();
            current_apc_ptr->StoreCleanupNextAPC(nullptr);
            if (current_apc_ptr->GetSegmentIOPtr()->HasThisManageControlFlag(SegmentIODefinition::ManagerControlFlagBits::DEAD_APC))
            {
                if (!new_head)
                {
                    new_head = current_apc_ptr;
                    tail = current_apc_ptr;
                }
                else
                {
                    tail->StoreRegistryNextAPC(current_apc_ptr);
                    tail = current_apc_ptr;
                }
            }
            current_apc_ptr = next_apc_ptr;
        }
        
    }

    void PackedCellContainerManager::ManagerManinLoop_() noexcept
    {
        while (RunningManager_.load(MoLoad_))
        {
            GlobalEpoch_.fetch_add(1, std::memory_order_acq_rel);
            const uint64_t min_epoch = ComputeMinThreadEpoch();
            AdaptivePackedCellContainer* work_batch = PopAllAPC_S(WorkStackHeadAPC_);
            if (work_batch)
            {
                ProcessRemainingWorkOfAPC_(work_batch, min_epoch);
                continue;
            }

            AdaptivePackedCellContainer* cleanup_batch = PopAllAPC_S(CleanupStackHeadAPC_);
            if (cleanup_batch)
            {
                ProcessCleanUpBatchOfAdaptivePackedCellContainer_(cleanup_batch, min_epoch);
                TryCompactRegistryInPlace_();
                continue;
            }
            AdaptiveBackOffOfAPCManager_.AutoBackoff();
            const uint64_t wake_snapshot = ManagerWakeCounter_.load(MoLoad_);
            if (AdaptiveBackOffOfAPCManager_.IsDeepSleep())
            {
                ManagerWakeCounter_.wait(wake_snapshot);
            }
        }
        const uint64_t min_epoch = ComputeMinThreadEpoch();
        if (AdaptivePackedCellContainer* work_batch = PopAllAPC_S(WorkStackHeadAPC_))
        {
            ProcessRemainingWorkOfAPC_(work_batch, min_epoch);
        }
        if (AdaptivePackedCellContainer* cleanup_batch = PopAllAPC_S(CleanupStackHeadAPC_))
        {
            ProcessCleanUpBatchOfAdaptivePackedCellContainer_(cleanup_batch, min_epoch);
        }
        TryCompactRegistryInPlace_();
    }
    


    
} // namespace PredictedAdaptedEncoding
