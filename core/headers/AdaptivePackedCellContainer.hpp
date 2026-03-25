#pragma once 
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <iostream>

#include "AtomicAdaptiveBackoff.hpp"
#include "MasterClockConf.hpp"
#include "PackedCellBranchPlugin.hpp"
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

    bool EnableBranching = true;
    uint32_t BranchSplitThresholdPercentage = 70;
    uint32_t BranchMaxDepth = 8;
    size_t BranchMinChildCapacity = 256;
};

struct AcquirePairedPointerStruct
{
    uint64_t AssembeledPtr = 0;
    size_t HeadIdx = SIZE_MAX;
    size_t TailIdx = SIZE_MAX;
    packed64_t HeadScreenshot = 0;
    packed64_t TailScreenshot = 0;
    RelOffsetMode32 Position = RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
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
    size_t ContainerPayloadCapacity_{0};
    int UsedNode_ = 0;
    bool IsContainerOwned_{false};
    ContainerConf APCContainerCfg_;
    std::atomic<size_t> Occupancy_{0};
    std::atomic<size_t> ProducerCursor_{0};
    std::atomic<size_t> ConsumerCursor_{0};

    AtomicAdaptiveBackoff* AdaptiveBackoffOfAPCPtr_{nullptr};
    MasterClockConf* MasterClockConfPtr_{nullptr};
    PackedCellContainerManager* APCManagerPtr_{nullptr};
    //branch
    std::unique_ptr<PackedCellBranchPlugin> BranchPluginOfAPC_;
    static inline std::atomic<uint32_t> GlobalBranchIdAlloc_{1};
    std::atomic<bool>BranchCreateInFlight_{false};
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

        APCKind Kind = APCKind::PACKED_NODE;
        AdaptivePackedCellContainer* ChildContainerPtr = nullptr;
        size_t ChildBaseIdx = NO_VAL;
        packed64_t RelEntryPacked = NO_VAL;

        void* HeapPtr = nullptr;
        size_t HeapSize = 0;

        PackedCellDataType RECellDType = PackedCellDataType::UnsignedPCellDataType;

        FinalizerKind_ KindFinalizer = FinalizerKind_::NONE;
        std::function<void(void*)> FinalizerPtr = nullptr;
        DeviceFence_ APCDeviceFence{};
        std::atomic<uint64_t> RetireEpoch{NO_VAL};
        std::atomic<RelEntry_*> NextPtr{nullptr};

        uint32_t ChildBranchId = NO_VAL;
        uint32_t ParentBranchId = NO_VAL;

        //child container constructor
        RelEntry_(
            AdaptivePackedCellContainer* apc_container = nullptr, 
            uint32_t child_branch_id = 0,
            uint32_t parent_branch_id = 0,
            size_t base = 0
        ) noexcept :
            Kind(APCKind::CHILD_CONTAINER), ChildContainerPtr(apc_container), ChildBaseIdx(base), 
            ChildBranchId(child_branch_id), ParentBranchId(parent_branch_id)
        {}
        //packed cell constructor
        RelEntry_(packed64_t p) noexcept :
            Kind(APCKind::PACKED_NODE), RelEntryPacked(p)
        {}
        //heap constructor
        RelEntry_(void* heap_ptr, size_t heap_size, PackedCellDataType pc_dtype) noexcept :
            Kind(APCKind::HEAP_NODE),HeapPtr(heap_ptr), 
            HeapSize(heap_size), RECellDType(pc_dtype),
            KindFinalizer(FinalizerKind_::HOST)
        {}
    };    //reloffset
    //epoch-table
    std::atomic<uint64_t>GlobalEpoch_{1};
    std::unique_ptr<std::atomic<uint64_t>[]> ThreadEpochArray_;
    size_t ThreadEpochCapacity_{0};
    std::atomic<size_t> RetireCount_{0};

    static inline thread_local size_t QSBRThreadIdx_ = SIZE_MAX;
    static inline thread_local PackedCellContainerManager::ThreadHandlePCCM  ThreadHandleAPCTL_ = {};

    //retire
    std::atomic<RelEntry_*> RetireHead_{nullptr};
    unsigned RetireBatchThreshold_{16}; //why 16??
    //reclaimation
    std::thread BackgroundThread_;
    std::mutex BackgroundMutex_;
    std::condition_variable BackgroundCondVar_;
    bool BackgroundThreadStop_{false};
    //Tools -- these should be encoded in header ??
    std::atomic<uint64_t> TotalRetired_{0};
    std::atomic<uint64_t> TotalReclaimed_{0};
    std::atomic<uint64_t> RetireQueDepthMax_{0};
    std::atomic<uint64_t> TotalReclaimedBytes_{0};
    std::atomic<uint64_t> TotalCasFailure_{0};
    //logging hook
    std::function<void(const char*, const char*)> APCLogger_;
    //region/index
    size_t RegionSize_{0};
    size_t NumRegion_{0};
    std::unique_ptr<std::atomic<uint8_t>[]> RegionRelArray_{nullptr};
    std::vector<std::vector<uint64_t>> RelBitmaps_;
    std::unique_ptr<std::atomic<uint64_t>[]> RegionEpochArray_{nullptr};
    static inline thread_local std::vector<std::pair<size_t, packed64_t>> TLSCandidates_;
    //--??

    uint64_t ComputeMinThreadEpoch() const noexcept;

    size_t RegisterThreadForQSBRImplementation_() noexcept;

    void RetirePushLocked_(RelEntry_* rel_entry_ptr) noexcept;

    static bool DeviceFenceSatisfied_(const RelEntry_& rel_entry_address) noexcept;
    
    void BackgroundReclaimerMainThread_() noexcept;

    size_t GetHashedRendomizedStep_(size_t sequense_number) noexcept;

    void UpdateRegionRelForIdx_(size_t idx, tag8_t rel_mask) noexcept;

    void InitZeroState_() noexcept;

    void RefreshAPCMeta_() noexcept;

    inline bool IfAnyValid_() const noexcept
    {
        return (BackingPtr && ContainerCapacity_ > 0);
    }

    inline bool IfValidPayloadIndex_(size_t idx) const noexcept
    {
        return (BackingPtr && idx < ContainerCapacity_ && idx >= PackedCellBranchPlugin::METACELL_COUNT);
    }

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

    inline size_t SuggestedChildCapacity_() const noexcept
    {
        const size_t child_payload = std::max<size_t>(APCContainerCfg_.BranchMinChildCapacity, ContainerCapacity_);
        return child_payload;
    }

public:
    AdaptivePackedCellContainer(/* args */) noexcept  = default;

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

    uint32_t GetBranchId() const noexcept;

    size_t ReserveProducerSlots(size_t number_of_slots) noexcept;

    size_t NextProducerSequence() noexcept;

    void StartBackgroundReclaimerIfNeed();

    void StopBackgroundReclaimer() noexcept;

    void InitOwned(size_t cpacity, int node = REL_NODE0, ContainerConf container_cfg = {}, size_t allignment = MAX_VAL);

    void FreeAll() noexcept;

    void InitRegionIdx(size_t region_size);
    
    void TryReclaimRetirePairedPtr_() noexcept;

    bool PollDeviceFencesOnce_() noexcept;

    void TryCreateBranchIfNeeded() noexcept
    {

    }
    void TryReclaimRetiredWithMinEpoch(uint64_t min_epoch) noexcept;

    void SetManagerForGlobalAPC(PackedCellContainerManager* pointer_of_global_apc_manager);
    //Paired Pointer functions
    PublishResult PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask = 0, int max_probs = -1) noexcept;
    bool PublishHeapPtrWithAdaptiveBackoff(void* target_publishable_ptr, uint16_t max_retries = 100);
    std::optional<AcquirePairedPointerStruct> AcquirePairedAtomicPtr(size_t probable_idx, bool claim_ownership = true, int max_claim_attempts = 256) noexcept;
    bool ReleaseAcquiredPairedPtr(const AcquirePairedPointerStruct& acquired_pair_struct, PackedCellLocalityTypes desired_locality = PackedCellLocalityTypes::ST_IDLE) noexcept;
    void RetireAcquiredPointerPair(const AcquirePairedPointerStruct& acquired_pair_struct, DeviceFence_ fence = {}) noexcept;
    template<typename TypePtr>
    std::optional<TypePtr> ViewPointerMemoryIfAssembeled(size_t probable_idx) noexcept;
    //
    void ManualAdvanceEpoch(uint64_t increment) noexcept
    {
        if (increment == 0)
        {
            return;
        }
        GlobalEpoch_.fetch_add(increment, std::memory_order_acq_rel);
        TryReclaimRetirePairedPtr_();
        
    }

    size_t GetOrSetTotalContainerCapacity(std::optional<size_t>container_capacity_of_apc = std::nullopt) noexcept
    {
        if (container_capacity_of_apc)
        {
            ContainerCapacity_ = *container_capacity_of_apc;
            ContainerPayloadCapacity_ = ContainerCapacity_ - PackedCellBranchPlugin::METACELL_COUNT;
        }
        return ContainerCapacity_;
    }

    size_t OccupancyAddSubOrGetAfterChange(int delta = 0)
    {
        return Occupancy_.fetch_add(delta) + delta;
    }

    PackedCellBranchPlugin* GetBranchPlugin() noexcept
    {
        return BranchPluginOfAPC_.get();
    }
    const PackedCellBranchPlugin* GetBranchPlugin() const noexcept
    {
        return BranchPluginOfAPC_.get();
    }
};


}  