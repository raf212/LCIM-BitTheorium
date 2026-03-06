
#include "AtomicAdaptiveBackoff.hpp"
# include <functional>

namespace PredictedAdaptedEncoding
{
    class AdaptivePackedCellContainer;

    class PackedCellContainerMannager
    {
        public :
            struct ThreadHandlePCCM
            {
                size_t QSBRIdx = SIZE_MAX;
                std::atomic<uint64_t>* WaitSlotPtr = nullptr;
                uint64_t APCNodeToken = 0;
            };

            static PackedCellContainerMannager& Instance()
            {
                static PackedCellContainerMannager pcc_mannager;
                return pcc_mannager;
            }

            void StartPCCMannager();
            void StopPCCMannager();
            ThreadHandlePCCM RegisterAPCThread();
            void UnRegisterAPCThread(const ThreadHandlePCCM& thread_handle) noexcept;
            inline void EnterCriticlContainer(const ThreadHandlePCCM& thread_handle) noexcept;
            inline void ExtitCriticalContainer(const ThreadHandlePCCM& thread_handle) noexcept;

            void NotifyAPCThread(const ThreadHandlePCCM& thread_handle, uint64_t token) noexcept;
            void NotifySlotIdxOfAPC(size_t idx, uint64_t token) noexcept;
            void NotifyAllActiveAPCThreads(uint64_t token) noexcept;

            void RegisterPackedCellContainer(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void RequestBanchCreationForPackedCellContainer(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void RequestForAPCReclaimation(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;
            void RequestBranchCreationForContainer(AdaptivePackedCellContainer* adaptive_p_c_ptr) noexcept;

            uint64_t ComputeMinThreadEpoch() const noexcept;
            MasterClockConf& MasterClockPackedCelAndMannager() noexcept;




        private :
            PackedCellContainerMannager();
            ~PackedCellContainerMannager();
            PackedCellContainerMannager(const PackedCellContainerMannager&) = delete;
            PackedCellContainerMannager& operator=(const PackedCellContainerMannager&) = delete;
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

            std::atomic<bool> RunningMannager_{false};
            std::thread ManagerThread_;
            unsigned ManagersIntervalMilliSecond_{25};
            std::atomic<uint64_t>GlobalEpoch_{1};

            Timer48 Timer48APCMannager_;
            MasterClockConf MasterClockConfAPCMannager_;
            AtomicAdaptiveBackoff AdaptiveBackOffAPCMannager_;
            size_t MaxThreads_ = 4096;
            std::atomic<size_t> UnregistersSinceCompact_{0};
            size_t CompactionTriggerThreshold_ = 1024;
            std::atomic<uint64_t> MannagerWakeCounter_{0};
            std::function<void(const char*, const char*)> Logger_;
            size_t AllocateThreadSlots_();
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
                    new_node_of_adaptive_packed_cell_container_ptr->StackNextPtr.store(head_node_of_adaptive_packed_cell_container, MoStoreUnSeq_);
                } while (!head_node_of_adaptive_packed_cell_container.compare_exchange_weak(head_node_ptr, new_node_of_adaptive_packed_cell_container_ptr, std::memory_order_release, std::memory_order_relaxed));
            }

    };
}