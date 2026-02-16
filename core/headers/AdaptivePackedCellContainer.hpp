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
        void* handle = nullptr;
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
        std::function<void(RelEntry_*)> FinalizerPtr;
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
    };
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
    std::function<void(const char*)> APCLogger_;

    uint64_t ComputeMinThreadEpoch() const noexcept
    {
        uint64_t min_epoch = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < ThreadEpochs_.size(); i++)
        {
            uint64_t val = ThreadEpochs_[i].load(MoLoad_);
            if (val == std::numeric_limits<uint64_t>::max())
            {
                continue;
            }
            if (val < min_epoch)
            {
                min_epoch = val;
            }
        }
        return min_epoch;
    }

    size_t RegisterThreadForQSBRImplementation_() noexcept
    {
        if (QSBRThreadIdx_ != SIZE_MAX)
        {
            return QSBRThreadIdx_;
        }
        uint64_t sentinal = std::numeric_limits<uint64_t>::max();
        uint64_t cur_epoch = GlobalEpoch_.load(MoLoad_);
        for (size_t i = 0; i < ThreadEpochs_[i]; i++)
        {
            uint64_t val = ThreadEpochs_[i].load(std::memory_order_relaxed);
            if (val == sentinal)
            {
                if (ThreadEpochs_[i].compare_exchange_strong(val, cur_epoch, EXsuccess_, EXfailure_))
                {
                    QSBRThreadIdx_ = i;
                    return i;
                }
                else
                {
                    TotalCasFailure_.fetch_add(1, MoStoreUnSeq_);
                }
            }
        }
        return SIZE_MAX;
    }

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