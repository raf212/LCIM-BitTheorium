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
    size_t Capacity_{0};
    bool Owned_{false};
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
    std::vector<std::atomic<std::uintptr_t>> RelOffset_;
    std::atomic<size_t> RelOffsetAlloc_{0};
    size_t RelOffsetCapacity_{0};
    //global epoch
    std::atomic<uint64_t>GlobalEpoch_{1};
    //epoch-table
    std::vector<std::atomic<uint64_t>> ThreadEpochs_;
    static inline thread_local size_t QSBRThreadIdx_ = SIZE_MAX;
    //retire
    std::atomic<RelEntry_*> RetireHead_{nullptr};
    std::atomic<size_t> RetireCount_{0};
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
        ThreadEpochs_[QSBRThreadIdx_].store(epoch, MoStoreSeq_);
    }

    inline void QSBRExitCritical_() noexcept
    {
        if (QSBRThreadIdx_ == SIZE_MAX)
        {
            return;
        }
        ThreadEpochs_[QSBRThreadIdx_].store(std::numeric_limits<uint64_t>::max(), MoStoreSeq_);
    }

    void RetirePushLocked_(RelEntry_* rel_entry_ptr) noexcept;

    static bool DeviceFenceSatisfied_(const RelEntry_& rel_entry_address) noexcept;

    void TryReclaimRetired_() noexcept;

    bool PollDeviceFencesOnce_() noexcept;
    
    void BackgroundReclaimerMainThread_() noexcept;
    //reloffset pointer helper
    static constexpr val32_t PTR_FLAG32_ = (1u << 31);
    static constexpr val32_t PTR_HALF_HEAD_ = (1u << 30);
    static constexpr val32_t PTR_HALF_TAIL_ = (1u << 29);
    static constexpr val32_t PTR_INDEX_MASK_ = 0x7FFFFFFFu;

    inline constexpr bool IsPointerValue32_(val32_t value) noexcept
    {
        return ((value & PTR_FLAG32_) != 0);
    }

    inline constexpr val32_t MakePointerValue32_(uint32_t idx) noexcept
    {
        return static_cast<val32_t>((idx & PTR_INDEX_MASK_) | PTR_FLAG32_);
    }

    inline constexpr uint32_t DecodePointerIdx_(val32_t value) noexcept
    {
        return static_cast<uint32_t>(value & PTR_INDEX_MASK_);
    }
    //region/index
    size_t RegionSize_{0};
    size_t NumRegion_{0};
    std::vector<std::atomic<uint8_t>> RegionRel_;
    std::vector<std::vector<uint64_t>> RelBitmaps_;
    std::vector<std::atomic<uint64_t>> RegionEpoch_;


public:
    AdaptivePackedCellContainer(/* args */);
    ~AdaptivePackedCellContainer();
};

AdaptivePackedCellContainer::AdaptivePackedCellContainer(/* args */)
{
}

AdaptivePackedCellContainer::~AdaptivePackedCellContainer()
{
}


}