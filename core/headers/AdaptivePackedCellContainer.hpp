#pragma once 
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstdio>

#include "AtomicAdaptiveBackoff.hpp"
#include "MasterClockConf.hpp"

namespace AtomicCScompact
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

    AtomicAdaptiveBackoff APCAdaptiveBackoff_;
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
    struct RelEntry_;
    //reloffset

    //global epoch
    std::atomic<uint64_t>GlobalEpoch_{1};
    //epoch-table
    std::unique_ptr<std::atomic<uint64_t>[]> ThreadEpochArray_;
    size_t ThreadEpochCapacity_{0};
    std::atomic<size_t> RetireCount_{0};
    static inline thread_local size_t QSBRThreadIdx_ = SIZE_MAX;
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


    uint64_t ComputeMinThreadEpoch() const noexcept;

    size_t RegisterThreadForQSBRImplementation_() noexcept;

    inline void QSBRCurThreadRegisterIfNeed_() noexcept
    {
        if (QSBRThreadIdx_ == SIZE_MAX)
        {
            (void) RegisterThreadForQSBRImplementation_();
        }
    }

    inline void QSBREnterCritical_() noexcept
    {
        QSBRCurThreadRegisterIfNeed_();
        if (QSBRThreadIdx_ == SIZE_MAX)
        {
            return;
        }
        uint64_t epoch = GlobalEpoch_.load(MoLoad_);
        ThreadEpochArray_[QSBRThreadIdx_].store(epoch, MoStoreSeq_);
    }

    inline void QSBRExitCritical_() noexcept
    {
        if (QSBRThreadIdx_ == SIZE_MAX)
        {
            return;
        }
        ThreadEpochArray_[QSBRThreadIdx_].store(std::numeric_limits<uint64_t>::max(), MoStoreSeq_);
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
        APCAdaptiveBackoff_(AtomicAdaptiveBackoff::PCBCfg{}, PackedMode::MODE_VALUE32),
        RegionSize_(0), NumRegion_(0), MasterClockConfPtr_(nullptr)
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

    size_t NextProducerSequence() noexcept;

    void StartBackgroundReclaimerIfNeed();

    void StopBackgroundReclaimer() noexcept;

    void InitOwned(size_t acpacity, int node = REL_NODE0, ContainerConf container_cfg = {}, size_t allignment = MAX_VAL);

    void FreeAll() noexcept;

    void InitRegionIdx(size_t region_size);
    
    void ManualAdvanceEpoch(uint64_t increment) noexcept
    {
        if (increment == 0)
        {
            return;
        }
        GlobalEpoch_.fetch_add(increment, std::memory_order_acq_rel);
        TryReclaimRetired_();
        
    }
    
    PublishResult PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask = 0, int max_probs = -1) noexcept;
    std::optional<uint64_t> TryAssemblePairedPtr_(size_t probable_idx, RelOffsetMode& ptr_position) const noexcept;
    void RetirePairedPtrAtIdx_(
        size_t probable_idx, FinalizerKind_ fk = FinalizerKind_::HOST,
        std::function<void(void*)> finalizer_fn = nullptr,
        DeviceFence_ fence = {}
    ) noexcept;

    void TryReclaimRetired_() noexcept;

    bool PollDeviceFencesOnce_() noexcept;
};



}