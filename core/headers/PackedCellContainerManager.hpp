
#pragma once 
#include "AdaptivePackedCellContainer/SegmentIODefinition.hpp"
# include <functional>

namespace PredictedAdaptedEncoding
{
    class AdaptivePackedCellContainer;

    class PackedCellContainerManager
    {
        public :
            struct ThreadHandlePCCM
            {
                size_t QSBRIdx = SIZE_MAX;
                std::atomic<uint64_t>* WaitSlotPtr = nullptr;
                uint64_t NodeTokenOfAPC = 0;
            };

            static PackedCellContainerManager& Instance()
            {
                // intentionally leaked singleton to avoid static-destruction order problems
                static PackedCellContainerManager* inst = []() {
                    return new PackedCellContainerManager();
                }();
                return *inst;
            }

            void StartAPCManager();
            void StopAPCManager();
            ThreadHandlePCCM RegisterAPCThread();
            void UnRegisterAPCThread(const ThreadHandlePCCM& thread_handle) noexcept;
            void EnterCriticalContainer(const ThreadHandlePCCM& thread_handle) noexcept;
            void ExtitCriticalContainer(const ThreadHandlePCCM& thread_handle) noexcept;

            void NotifySlotIdxOfAPC(size_t idx, uint64_t thread_token) noexcept;
            void NotifyAllActiveAPCThreads(uint64_t thread_token) noexcept;

            void RegisterAPCFromManager_(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void UnRegisterAPCFromManager_(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void RequestAPCSegmentCreationFromManager_(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void ReclaimationRequestOfAPCSegmentFromManager_(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            AdaptivePackedCellContainer* GetAPCPtrFromBranchId(uint32_t) noexcept;
            uint64_t ComputeMinThreadEpoch() const noexcept;


            void PushTOAPCManagerStack_(
                std::atomic<AdaptivePackedCellContainer*>& head_stack,
                AdaptivePackedCellContainer* apc_ptr, bool is_cleanup_stack
            ) noexcept;

            AdaptivePackedCellContainer* PopAllAPC_S(std::atomic<AdaptivePackedCellContainer*>& head_stack) noexcept
            {
                return head_stack.exchange(nullptr, std::memory_order_acq_rel);
            }

            AtomicAdaptiveBackoff& GetManagersAdaptiveBackoff() noexcept
            {
                return AdaptiveBackOffOfAPCManager_;
            }

            AtomicAdaptiveBackoff::PCBDecision GetCellsAdaptiveBackoffFromManager(packed64_t packed_cell) noexcept
            {
                return AdaptiveBackOffOfAPCManager_.AdaptiveBackOffPacked(packed_cell);
            }


            void UsePreAllocatedNodePoolOfAdaptivePackedCellContainer(size_t pool_size_of_preallocated_adaptive_packed_cell_container) noexcept;

        private :
            PackedCellContainerManager();
            ~PackedCellContainerManager();
            PackedCellContainerManager(const PackedCellContainerManager&) = delete;
            PackedCellContainerManager& operator=(const PackedCellContainerManager&) = delete;

            std::atomic<AdaptivePackedCellContainer*> RegistryHeadAPC_{nullptr};
            std::atomic<AdaptivePackedCellContainer*> WorkStackHeadAPC_{nullptr};
            std::atomic<AdaptivePackedCellContainer*> CleanupStackHeadAPC_{nullptr};

            std::atomic<size_t>ThreadFreelistHead_{SIZE_MAX};
            size_t MaxThreads_ = 4096;
            size_t  ThreadTableCapacity_{0};
            std::unique_ptr<std::atomic<size_t>[]> ThreadNextIdxPtr_;
            std::unique_ptr<std::atomic<uint64_t>[]> ThreadEpochArrayPtr_;
            std::unique_ptr<std::atomic<uint64_t>[]> ThreadWaitSlotArrayPtr_;
            // bool UseNodePool_ = false;

            std::atomic<bool> RunningManager_{false};
            std::thread ManagerThread_;
            std::atomic<uint64_t>GlobalEpoch_{1};

            AtomicAdaptiveBackoff AdaptiveBackOffOfAPCManager_;
            std::atomic<uint64_t> ManagerWakeCounter_{0};


            size_t AllocateThreadSlots_() noexcept;
            void FreeThreadSlots_(size_t idx) noexcept;
            void PushFreeThreadIndex_(size_t idx) noexcept;
            size_t PopFreeThreadIndex_() noexcept;


            void ManagerManinLoop_() noexcept;
            void ProcessRemainingWorkOfAPC_(AdaptivePackedCellContainer* batch_head_of_adaptive_packed_cell_container_ptr, uint64_t min_epoch = 64) noexcept;
            void ProcessCleanUpBatchOfAdaptivePackedCellContainer_(AdaptivePackedCellContainer* batch_head_of_adaptive_packed_cell_container, uint64_t min_epoch) noexcept;
            void TryCompactRegistryInPlace_() noexcept;
            static constexpr uint64_t THREAD_SENTINEL_ = std::numeric_limits<uint64_t>::max();

    };
}