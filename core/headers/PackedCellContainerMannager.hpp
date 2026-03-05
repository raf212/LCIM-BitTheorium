
#include "AtomicAdaptiveBackoff.hpp"

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

    };
}