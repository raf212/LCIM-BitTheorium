#pragma once 
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <iostream>

#include "AtomicAdaptiveBackoff.hpp"
#include "MasterClockConf.hpp"
#include "PackedCellContainerManager.hpp"

namespace PredictedAdaptedEncoding
{
static_assert(__cpp_lib_atomic_wait, "C++ must suppoet atomic wait/notify");
struct ContainerConf
{
    size_t ScanLimit = 256;
    size_t MaxGather = 1024;
    unsigned TimerDownShift = 10u;
    bool UseTimeStamp = true;
    bool AllowPublishedClickFixUp = true;
    size_t MaxTlsCandidates = 4096;
    size_t ProducerBlockSize = 64;
    size_t RegionSize = 0;
    size_t ReloffsetCapacity = 0;
    PackedMode InitialMode = PackedMode::MODE_VALUE32;
    unsigned RetireBatchThreshold = 16;
    unsigned BackgroundEpochAdvanceMS = 50;
    static constexpr size_t MAXTLS = 8192;
};

struct AcquirePairedPointerStruct
{
    uint64_t AssembeledPtr = 0;
    size_t HeadIdx = SIZE_MAX;
    size_t TailIdx = SIZE_MAX;
    packed64_t HeadScreenshot = 0;
    packed64_t TailScreenshot = 0;
    RelOffsetMode Position = RelOffsetMode::RELOFFSET_GENERIC_VALUE;
    bool Ownership = false;
};

enum class PublishStatus : uint8_t
{
    OK = 0,
    FULL = 1,
    INVALID = 2
};

struct PublishResult
{
    PublishStatus ResultStatus;
    size_t Index;
};

class PackedCellContainerManager;

class AdaptivePackedCellContainer
{
public:
    std::atomic<packed64_t>* BackingPtr{nullptr};

    struct QSBRGuard;
    
    struct RelEntryGuard;

private:
    size_t ContainerCapacity_{0};
    int UsedNode_ = 0;
    bool IsContainerOwned_{false};
    ContainerConf APCContainerCfg_;
    std::atomic<size_t> Occupancy_{0};
    std::atomic<size_t> ProducerCursor_{0};
    std::atomic<size_t> ConsumerCursor_{0};

    AtomicAdaptiveBackoff* AdaptiveBackoffOfAPCPtr_{nullptr};

    enum class FinalizerKind_ : uint8_t 
    {
        NONE = 0,
        HOST = 1,
        PINNED = 2,
        GPU = 3
    };
    struct DeviceFence_
    {
        void* HandleDeviceFencePtr = nullptr;
        std::function<bool(void*)> IsSignaled;
    };
    struct RelEntry_
    {
        enum class APCKind : uint8_t
        {
            CHILD_CONTAINER = 0,
            PACKED_NODE = 1,
            HEAP_NODE = 2
        };

        APCKind Kind;
        AdaptivePackedCellContainer* ChildContainerPtr;
        size_t ChildBaseIdx;
        packed64_t RelEntryPacked;

        void* HeapPtr;
        size_t HeapSize;

        PackedCellDataType RECellDType;

        FinalizerKind_ KindFinalizer;
        std::function<void(void*)> FinalizerPtr;
        DeviceFence_ APCDeviceFence;
        std::atomic<uint64_t> RetireEpoch;
        std::atomic<RelEntry_*> NextPtr;

        //child container constructor
        RelEntry_(AdaptivePackedCellContainer* apc_container = nullptr, size_t base = 0) noexcept :
            Kind(APCKind::CHILD_CONTAINER), ChildContainerPtr(apc_container), ChildBaseIdx(base), RelEntryPacked(0),
            HeapPtr(nullptr), HeapSize(0), RECellDType(PackedCellDataType::UnsignedPCellDataType),
            KindFinalizer(FinalizerKind_::NONE), FinalizerPtr(nullptr), APCDeviceFence{}, RetireEpoch(0), NextPtr(nullptr)
        {}
        //packed cell constructor
        RelEntry_(packed64_t p) noexcept :
            Kind(APCKind::PACKED_NODE), ChildContainerPtr(nullptr), ChildBaseIdx(0), RelEntryPacked(p),
            HeapPtr(0), RECellDType(PackedCellDataType::UnsignedPCellDataType),
            KindFinalizer(FinalizerKind_::NONE), FinalizerPtr(nullptr), APCDeviceFence{}, RetireEpoch(0), NextPtr(nullptr)
        {}
        //heap constructor
        RelEntry_(void* heap_ptr, size_t heap_size, PackedCellDataType pc_dtype) noexcept :
            Kind(APCKind::HEAP_NODE), ChildContainerPtr(nullptr), ChildBaseIdx(0), RelEntryPacked(0),
            HeapPtr(heap_ptr), HeapSize(heap_size), RECellDType(pc_dtype),
            KindFinalizer(FinalizerKind_::HOST), FinalizerPtr(nullptr), APCDeviceFence{}, RetireEpoch(0), NextPtr(nullptr)
        {}
    };    //reloffset

    //global epoch
    std::atomic<uint64_t>GlobalEpoch_{1};
    //epoch-table
    std::unique_ptr<std::atomic<uint64_t>[]> ThreadEpochArray_;
    size_t ThreadEpochCapacity_{0};
    std::atomic<size_t> RetireCount_{0};

    static inline thread_local size_t QSBRThreadIdx_ = SIZE_MAX;
    static inline thread_local PackedCellContainerManager::ThreadHandlePCCM  ThreadHandleAPCTL_ = {};

    //retire
    std::atomic<RelEntry_*> RetireHead_{nullptr};
    unsigned RetireBatchThreshold_{16};
    //reclaimation
    std::thread BackgroundThread_;
    std::mutex BackgroundMutex_;
    std::condition_variable BackgroundCondVar_;
    bool BackgroundThreadStop_{false};
    //Tools
    std::atomic<uint64_t> TotalRetired_{0};
    std::atomic<uint64_t> TotalReclaimed_{0};
    std::atomic<uint64_t> RetireQueDepthMax_{0};
    std::atomic<uint64_t> TotalReclaimedBytes_{0};
    std::atomic<uint64_t> TotalCasFailure_{0};
    //logging hook
    std::function<void(const char*, const char*)> APCLogger_;
    //mc
    MasterClockConf* MasterClockConfPtr_{nullptr};

    PackedCellContainerManager* APCManagerPtr_{nullptr};

    uint64_t ComputeMinThreadEpoch() const noexcept;

    size_t RegisterThreadForQSBRImplementation_() noexcept;

    inline void QSBRCurThreadRegisterIfNeed_() noexcept
    {
        if (ThreadHandleAPCTL_.QSBRIdx != SIZE_MAX && ThreadHandleAPCTL_.WaitSlotPtr != nullptr)
        {
            return;
        }
        ThreadHandleAPCTL_ = PackedCellContainerManager::Instance().RegisterAPCThread();
    }

    inline void QSBREnterCritical_() noexcept
    {
        QSBRCurThreadRegisterIfNeed_();
        if (ThreadHandleAPCTL_.QSBRIdx == SIZE_MAX)
        {
            return;
        }
        PackedCellContainerManager::Instance().EnterCriticalContainer(ThreadHandleAPCTL_);
    }

    inline void QSBRExitCritical_() noexcept
    {
        if (ThreadHandleAPCTL_.QSBRIdx == SIZE_MAX)
        {
            return;
        }
        PackedCellContainerManager::Instance().ExtitCriticalContainer(ThreadHandleAPCTL_);
    }

    void RetirePushLocked_(RelEntry_* rel_entry_ptr) noexcept;

    static bool DeviceFenceSatisfied_(const RelEntry_& rel_entry_address) noexcept;
    
    void BackgroundReclaimerMainThread_() noexcept;

    //3 function bellow are demos 
    inline constexpr bool IsPointerValue32_(val32_t value) noexcept
    {
        (void)value;
        return true;
    }
    //demo
    inline constexpr val32_t MakePointerValue32_(uint32_t idx) noexcept
    {
        return idx;
    }
    //demo
    inline constexpr uint32_t DecodePointerIdx_(val32_t value) noexcept
    {
        return value;
    }
    //region/index
    size_t RegionSize_{0};
    size_t NumRegion_{0};
    std::unique_ptr<std::atomic<uint8_t>[]> RegionRelArray_{nullptr};
    std::vector<std::vector<uint64_t>> RelBitmaps_;
    std::unique_ptr<std::atomic<uint64_t>[]> RegionEpochArray_{nullptr};

    static inline thread_local std::vector<std::pair<size_t, packed64_t>> TLSCandidates_;

    inline bool IfAnyValid_() const noexcept
    {
        return (BackingPtr && ContainerCapacity_ > 0);
    }

    inline bool IfIdxValid_(size_t idx) const noexcept
    {
        return (BackingPtr && idx < ContainerCapacity_);
    }

    size_t GetHashedRendomizedStep_(size_t sequense_number) noexcept
    {
        uint64_t mix_hash = (
            (static_cast<uint64_t>(sequense_number) * ID_HASH_GOLDEN_CONST) ^ (static_cast<uint64_t>(sequense_number >> (VALBITS + 1)))
        );
        size_t step = 1;
        if (ContainerCapacity_ > 1)
        {
            step = static_cast<size_t>((mix_hash % (ContainerCapacity_ - 1)) + 1);
        }
        return step;
    }

    void UpdateRegionRelForIdx_(size_t idx, tag8_t rel_mask) noexcept;

    void InitZeroState_()noexcept;

public:
    AdaptivePackedCellContainer(/* args */) noexcept :
        BackingPtr(nullptr), ContainerCapacity_(0), IsContainerOwned_(false), UsedNode_(0), APCContainerCfg_(),
        RegionSize_(0), NumRegion_(0), MasterClockConfPtr_(nullptr), APCManagerPtr_(nullptr)
    {}
    ~AdaptivePackedCellContainer()
    {
        {
            std::lock_guard<std::mutex>lk(BackgroundMutex_);
            BackgroundThreadStop_ = true;
            BackgroundCondVar_.notify_all();
        }
        if (BackgroundThread_.joinable())
        {
            BackgroundThread_.join();
        }
        FreeAll();
    }
    AdaptivePackedCellContainer(const AdaptivePackedCellContainer&) = delete;
    AdaptivePackedCellContainer& operator = (const AdaptivePackedCellContainer&) = delete;

    size_t ReserveProducerSlots(size_t number_of_slots) noexcept
    {
        if (!IfAnyValid_() || number_of_slots == 0)
        {
            return SIZE_MAX;
        }
        return ProducerCursor_.fetch_add(number_of_slots, std::memory_order_relaxed);
    }

    size_t GetOrSetContainerCapacity(std::optional<size_t>container_capacity_of_apc = std::nullopt) noexcept
    {
        if (container_capacity_of_apc)
        {
            ContainerCapacity_ = *container_capacity_of_apc;
        }
        return ContainerCapacity_;
    }

    size_t OccupancyAddSubOrGetAfterChange(int amount_should_be_changed_Use_negetive_for_decrese_positive_to_increase = 0)
    {
        return Occupancy_.fetch_add(amount_should_be_changed_Use_negetive_for_decrese_positive_to_increase) + amount_should_be_changed_Use_negetive_for_decrese_positive_to_increase;
    }

    size_t NextProducerSequence() noexcept;

    void StartBackgroundReclaimerIfNeed();

    void StopBackgroundReclaimer() noexcept;

    void InitOwned(size_t cpacity, int node = REL_NODE0, ContainerConf container_cfg = {}, size_t allignment = MAX_VAL);

    void FreeAll() noexcept;

    void InitRegionIdx(size_t region_size);
    
    void ManualAdvanceEpoch(uint64_t increment) noexcept
    {
        if (increment == 0)
        {
            return;
        }
        GlobalEpoch_.fetch_add(increment, std::memory_order_acq_rel);
        TryReclaimRetirePairedPtr_();
        
    }
    


    void TryReclaimRetirePairedPtr_() noexcept;

    bool PollDeviceFencesOnce_() noexcept;

    void TryCreateBranchIfNeeded() noexcept
    {

    }
    void TryReclaimRetiredWithMinEpoch(uint64_t min_epoch) noexcept;

    void SetManagerForGlobalAPC(PackedCellContainerManager* pointer_of_global_apc_manager)
    {
        if (pointer_of_global_apc_manager)
        {
            try
            {
                pointer_of_global_apc_manager->StartPCCManager();
                APCManagerPtr_ = pointer_of_global_apc_manager;
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
            }
        }
    }
    //Paired Pointer functions
    //old
    PublishResult PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask = 0, int max_probs = -1) noexcept;
    std::optional<uint64_t> TryAssemblePairedPtr_(size_t probable_idx, RelOffsetMode& ptr_position_for_easy_return) const noexcept;

    std::optional<uint64_t> GetAssembledPtrWithTriCASReset(size_t probable_idx, RelOffsetMode& ptr_position_for_easy_return, 
                                            std::optional<bool>ownership_cas = std::nullopt, std::optional<tag8_t>third_cas = std::nullopt) noexcept;

    void RetirePairedPtrAtIdx_(
        size_t probable_idx,
        DeviceFence_ fence = {}
    ) noexcept;
    bool PublishHeapPtrWithAdaptiveBackoff(void* target_publishable_ptr, uint16_t max_retries = 100);
    //new
    std::optional<AcquirePairedPointerStruct> AcquirePairedAtomicPtr(size_t probable_idx, bool claim_ownership = true, int max_claim_attempts = 256) noexcept;
    bool ReleaseAcquiredPairedPtr(const AcquirePairedPointerStruct& acquired_pair_struct, tag8_t desired_locality = ST_IDLE) noexcept;
    void RetireAcquiredPointerPair(const AcquirePairedPointerStruct& acquired_pair_struct, DeviceFence_ fence = {}) noexcept;
    template<typename TypePtr>
    std::optional<TypePtr> ViewPointerMemoryIfAssembeled(size_t probable_idx) noexcept;
    

};


}  