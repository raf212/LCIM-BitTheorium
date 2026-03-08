
#include "AtomicAdaptiveBackoff.hpp"
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
                static PackedCellContainerManager pcc_mannager;
                return pcc_mannager;
            }

            void StartPCCManager();
            void StopPCCManager();
            ThreadHandlePCCM RegisterAPCThread();
            void UnRegisterAPCThread(const ThreadHandlePCCM& thread_handle) noexcept;
            void EnterCriticlContainer(const ThreadHandlePCCM& thread_handle) noexcept;
            void ExtitCriticalContainer(const ThreadHandlePCCM& thread_handle) noexcept;

            inline void NotifyAPCThread(const ThreadHandlePCCM& thread_handle, uint64_t thread_token) noexcept
            {
                NotifySlotIdxOfAPC(thread_handle.QSBRIdx, thread_token);
            }

            inline void NotifySlotIdxOfAPC(size_t idx, uint64_t thread_token) noexcept;
            void NotifyAllActiveAPCThreads(uint64_t thread_token) noexcept;

            void RegisterAdaptivePackedCellContainer(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void RequestBanchCreationForTheAdaptivePackedCellContainer(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void RequestFoReclaimationOfTheAdaptivePackedCellContainer(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void RequestBranchCreationForTheAdaptivePackedCellContainer(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;

            uint64_t ComputeMinThreadEpoch() const noexcept;

            MasterClockConf& GetMasterClockAdaptivePackedCellContainerManager() noexcept
            {
                return MasterClockConfAPCManager_;
            }

            AtomicAdaptiveBackoff& GetAdaptiveBackoffOfAdaptivePackedCellContainerManager() noexcept
            {
                return AdaptiveBackOffOfAPCManager_;
            }

            void SetLogger(std::function<void(const char*, const char*)> logger_for_apc_and_mannager) noexcept
            {
                Logger_ = std::move(logger_for_apc_and_mannager);
            }

            void UsePreAllocatedNodePoolOfAdaptivePackedCellContainer(size_t pool_size_of_preallocated_adaptive_packed_cell_container) noexcept;

            void SetRegistryCompectionThreshold(size_t unregister_threshold) noexcept 
            {
                CompactionTriggerThreshold_ = unregister_threshold;
            }


        private :
            PackedCellContainerManager();
            ~PackedCellContainerManager();
            PackedCellContainerManager(const PackedCellContainerManager&) = delete;
            PackedCellContainerManager& operator=(const PackedCellContainerManager&) = delete;
            struct NodeOfAdaptivePackedCellContainer_
            {
                AdaptivePackedCellContainer* APCContainerPtr{nullptr};
                std::atomic<uint32_t> ReclaimationNeededAPC{0};
                std::atomic<uint32_t> RequestedBranchedAPC{0};
                std::atomic<uint32_t> DeadAPC{0};

                NodeOfAdaptivePackedCellContainer_* RegistryNextPtr{nullptr};
                std::atomic<NodeOfAdaptivePackedCellContainer_*> StackNextPtr{nullptr};
                uint64_t DebugId = 0;
            };
            std::atomic<NodeOfAdaptivePackedCellContainer_*> WorkStackHeadPtr_{nullptr};
            std::atomic<NodeOfAdaptivePackedCellContainer_*> CleanUpStackHeaD_{nullptr};

            std::atomic<size_t>ThreadFreelistHead_{SIZE_MAX};
            std::vector<std::atomic<size_t>> ThreadNextIdx_;
            std::unique_ptr<std::atomic<uint64_t>[]> ThreadEpochArrayPtr_;
            std::unique_ptr<std::atomic<uint64_t>[]> ThreadWaitSlotArrayPtr_;
            size_t  ThreadTableCapacity_{0};
            std::atomic<NodeOfAdaptivePackedCellContainer_*> RegistryHeadOfAPCNodesPtr_{nullptr};
            std::atomic<NodeOfAdaptivePackedCellContainer_*> NodePoolHeadOfAPC_{nullptr};
            bool UseNodePool_ = false;

            std::atomic<bool> RunningManager_{false};
            std::thread ManagerThread_;
            unsigned ManagersIntervalMilliSecond_{25};
            std::atomic<uint64_t>GlobalEpoch_{1};

            Timer48 Timer48APCManager_;
            MasterClockConf MasterClockConfAPCManager_;
            AtomicAdaptiveBackoff AdaptiveBackOffOfAPCManager_;
            size_t MaxThreads_ = 4096;
            std::atomic<size_t> UnregistersSinceCompact_{0};
            size_t CompactionTriggerThreshold_ = 1024;
            std::atomic<uint64_t> ManagerWakeCounter_{0};
            std::function<void(const char*, const char*)> Logger_;
            size_t AllocateThreadSlots_() noexcept;
            void FreeThreadSlots_(size_t idx) noexcept;
            void PushFreeThreadIndex_(size_t idx) noexcept;
            size_t PopFreeThreadIndex_() noexcept;

            NodeOfAdaptivePackedCellContainer_* AllocateNewAdaptivePackedCellContainerNode_(AdaptivePackedCellContainer* apc_ptr) noexcept;
            void FreePointedAdaptivePackedCellContainerNode_(NodeOfAdaptivePackedCellContainer_* node_of_apc_ptr) noexcept;

            static inline NodeOfAdaptivePackedCellContainer_* PopAllStackOfAdaptivePackedCellContainers_(std::atomic<NodeOfAdaptivePackedCellContainer_*>& head_node_of_adaptive_packed_cell_container) noexcept
            {
                return head_node_of_adaptive_packed_cell_container.exchange(nullptr, std::memory_order_acq_rel);
            }

            static inline void PushANodeAtHeadInStackOfAdaptivePackedCellContainer_(std::atomic<NodeOfAdaptivePackedCellContainer_*>& head_node_of_adaptive_packed_cell_container, 
                NodeOfAdaptivePackedCellContainer_* new_node_of_adaptive_packed_cell_container_ptr
            ) noexcept
            {
                NodeOfAdaptivePackedCellContainer_* head_node_ptr = head_node_of_adaptive_packed_cell_container.load(MoLoad_);
                do
                {
                    new_node_of_adaptive_packed_cell_container_ptr->StackNextPtr.store(head_node_ptr, MoStoreUnSeq_);
                } while (!head_node_of_adaptive_packed_cell_container.compare_exchange_weak(head_node_ptr, new_node_of_adaptive_packed_cell_container_ptr, std::memory_order_release, std::memory_order_relaxed));
            }

            void ManagerManinLoop_() noexcept;
            void ProcessWorkerBatchOfAdaptivePackedCellContainer_(NodeOfAdaptivePackedCellContainer_* batch_head_of_adaptive_packed_cell_container_ptr, uint64_t min_epoch) noexcept;
            void ProcessCleanUpBatchOfAdaptivePackedCellContainer_(NodeOfAdaptivePackedCellContainer_* batch_head_of_adaptive_packed_cell_container) noexcept;
            void TryCompactRegistryOnce_() noexcept;
            static constexpr uint64_t THREAD_SENTINEL_ = std::numeric_limits<uint64_t>::max();

    };
}