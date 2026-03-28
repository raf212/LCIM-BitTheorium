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
    
private:
    size_t ContainerCapacity_{0};
    int UsedNode_ = 0;
    bool IsContainerOwned_{false};
    std::atomic<size_t> ProducerCursor_{0};
    std::atomic<size_t> ConsumerCursor_{0};

    AtomicAdaptiveBackoff* AdaptiveBackoffOfAPCPtr_{nullptr};
    MasterClockConf* MasterClockConfPtr_{nullptr};
    PackedCellContainerManager* APCManagerPtr_{nullptr};
    std::unique_ptr<PackedCellBranchPlugin> BranchPluginOfAPC_;
    static inline std::atomic<uint32_t> GlobalBranchIdAlloc_{1};
    static inline thread_local PackedCellContainerManager::ThreadHandlePCCM  ThreadHandleAPCTL_ = {};
    std::atomic<uint64_t> TotalCasFailure_{0};
    //logging hook
    std::function<void(const char*, const char*)> APCLogger_;
    //region/index
    std::unique_ptr<std::atomic<uint8_t>[]> RegionRelArray_{nullptr};
    std::vector<std::vector<uint64_t>> RelBitmaps_;
    std::unique_ptr<std::atomic<uint64_t>[]> RegionEpochArray_{nullptr};
    static inline thread_local std::vector<std::pair<size_t, packed64_t>> TLSCandidates_;
    //--??
    
    size_t GetHashedRendomizedStep_(size_t sequense_number) noexcept;

    void UpdateRegionRelForIdx_(tag8_t rel_mask) noexcept;

    void InitZeroState_() noexcept;

    void RefreshAPCMeta_() noexcept;

    size_t SuggestedChildCapacity_() const noexcept;

    inline bool IfValidPayloadIndex_(size_t idx) const noexcept
    {
        return (BackingPtr && idx < ContainerCapacity_ && idx >= PayloadBegin());
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


public:
    AdaptivePackedCellContainer(/* args */) noexcept  = default;

    ~AdaptivePackedCellContainer()
    {
        FreeAll();
    }

    AdaptivePackedCellContainer(const AdaptivePackedCellContainer&) = delete;
    AdaptivePackedCellContainer& operator = (const AdaptivePackedCellContainer&) = delete;

    uint32_t GetBranchId() const noexcept;

    size_t ReserveProducerSlots(size_t number_of_slots) noexcept;

    size_t NextProducerSequence() noexcept;

    void InitOwned(size_t cpacity, int node = REL_NODE0, ContainerConf container_cfg = {}, size_t allignment = MAX_VAL);

    void FreeAll() noexcept;

    void InitRegionIdx(size_t region_size) noexcept;
    

    void TryCreateBranchIfNeeded() noexcept;
    
    void SetManagerForGlobalAPC(PackedCellContainerManager* pointer_of_global_apc_manager) noexcept;
    //Paired Pointer functions
    PublishResult PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask = 0, int max_probs = -1) noexcept;
    bool PublishHeapPtrWithAdaptiveBackoff(void* target_publishable_ptr, uint16_t max_retries = 100);
    std::optional<AcquirePairedPointerStruct> AcquirePairedAtomicPtr(size_t probable_idx, bool claim_ownership = true, int max_claim_attempts = 256) noexcept;
    bool ReleaseAcquiredPairedPtr(const AcquirePairedPointerStruct& acquired_pair_struct, PackedCellLocalityTypes desired_locality = PackedCellLocalityTypes::ST_IDLE) noexcept;
    void RetireAcquiredPointerPair(const AcquirePairedPointerStruct& acquired_pair_struct) noexcept;
    template<typename TypePtr>
    std::optional<TypePtr> ViewPointerMemoryIfAssembeled(size_t probable_idx) noexcept;
    //

    size_t GetOrSetTotalContainerCapacity(std::optional<size_t>container_capacity_of_apc = std::nullopt) noexcept
    {
        if (container_capacity_of_apc)
        {
            ContainerCapacity_ = *container_capacity_of_apc;
        }
        if (ContainerCapacity_ < MINIMUM_BRANCH_CAPACITY)
        {
            ContainerCapacity_ = MINIMUM_BRANCH_CAPACITY;
        }
        
        return ContainerCapacity_;
    }

    size_t OccupancyAddOrSubAndGetAfterChange(int delta = 0)
    {
        if (!BranchPluginOfAPC_)
        {
            return SIZE_MAX;
        }
        uint32_t current_occupancy = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::OCCUPANCY_SNAPSHOT);
        return static_cast<size_t>(BranchPluginOfAPC_->UpdateOccupancySnapshotAndReturn(current_occupancy + delta));
    }

    PackedCellBranchPlugin* GetBranchPlugin() noexcept
    {
        return BranchPluginOfAPC_.get();
    }
    const PackedCellBranchPlugin* GetBranchPlugin() const noexcept
    {
        return BranchPluginOfAPC_.get();
    }

    inline size_t GetPayloadCapacity() const noexcept
    {
        return BranchPluginOfAPC_ ? BranchPluginOfAPC_->PayloadCapacityFromHeader() : NO_VAL;
    }

    inline size_t GetPayloadEnd() const noexcept
    {
        return BranchPluginOfAPC_ ? BranchPluginOfAPC_->PayloadEndRead() : SIZE_MAX;
    }

    const uint32_t PayloadBegin() const noexcept
    {
        return PackedCellBranchPlugin::METACELL_COUNT;
    }
    
    inline bool IfAPCBranchValid() const noexcept
    {
        return (BackingPtr && GetPayloadCapacity() >= MINIMUM_BRANCH_CAPACITY);
    }

    inline void DirectStoreCellToAPCIdx(size_t idx, packed64_t cell) noexcept
    {
        if (idx < PayloadBegin())   return;
        BackingPtr[idx].store(cell, MoStoreSeq_);
    }
};


}  