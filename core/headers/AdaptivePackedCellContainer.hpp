#pragma once 
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <iostream>

#include "AdaptivePackedCellContainer/AtomicAdaptiveBackoff.hpp"
#include "AdaptivePackedCellContainer/MasterClockConf.hpp"
#include "AdaptivePackedCellContainer/PackedCellBranchPlugin.hpp"
#include "PackedCellContainerManager.hpp"
#include "NodeInGraphView.h"
#include "AdaptivePackedCellContainer/APCHElpers.hpp"

namespace PredictedAdaptedEncoding
{
static_assert(__cpp_lib_atomic_wait, "C++ must suppoet atomic wait/notify");
#define CURRENT_BRANCHING_CLIENT  3

class PackedCellContainerManager;

class AdaptivePackedCellContainer
{
public:
    std::atomic<packed64_t>* BackingPtr{nullptr};

    struct QSBRGuard;

    enum class APCPortSlot : uint32_t
    {
        NONE = 0,
        FEED_FORWARD_IN = 1,
        FEED_FORWARD_OUT = 2,
        FEED_BACKWARD_IN = 3,
        FEED_BACKWARD_OUT = 4,
        LATERAL_0 = 5,
        LATERAL_1 = 6
    };
    
private:
    AtomicAdaptiveBackoff* AdaptiveBackoffOfAPCPtr_{nullptr};
    MasterClockConf* MasterClockConfPtr_{nullptr};
    PackedCellContainerManager* APCManagerPtr_{nullptr};
    std::unique_ptr<PackedCellBranchPlugin> BranchPluginOfAPC_;
    static inline std::atomic<uint32_t> GlobalBranchIdAlloc_{1};
    static inline thread_local PackedCellContainerManager::ThreadHandlePCCM  ThreadHandleAPCTL_ = {};
    
    //logging hook
    std::function<void(const char*, const char*)> APCLogger_;
    //region/index
    std::unique_ptr<std::atomic<uint8_t>[]> RegionRelArray_{nullptr};
    std::vector<std::vector<uint64_t>> RelBitmaps_;
    std::unique_ptr<std::atomic<uint64_t>[]> RegionEpochArray_{nullptr};
    static inline thread_local std::vector<std::pair<size_t, packed64_t>> TLSCandidates_;
    //--??
    
    size_t GetHashedRendomizedStep_(size_t sequense_number) noexcept;

    void InitZeroState_() noexcept;

    void RefreshAPCMeta_() noexcept;

    size_t SuggestedChildCapacity_() const noexcept;

    std::optional<packed64_t> TryConsumeAndIdleFromRegionLocal_(APCPagedNodeRelMaskClasses region_kind, size_t& scan_cursor) noexcept;

    PublishResult TryPublishToRegionLocal_(APCPagedNodeRelMaskClasses region_kind, packed64_t packed_cell, bool force_rel_mask = true, uint16_t max_tries = MAX_TRIES) noexcept;

    void UpdateRegionRelMaskForIdx_(tag8_t rel_mask) noexcept;

    inline bool IfValidPayloadIndex_(size_t idx) const noexcept
    {
        return (BackingPtr && idx >= PayloadBegin() && idx < GetPayloadEnd());
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

    void InitOwned(size_t cpacity, ContainerConf container_cfg = {});
    void InitAPCAsNode(
        size_t capacity,
        const ContainerConf& container_configuration,
        uint32_t node_role_flags,
        PackedCellBranchPlugin::APCNodeComputeKind compute_kind = PackedCellBranchPlugin::APCNodeComputeKind::NONE,
        uint32_t aux_param_u32 = NO_VAL

    );

    void FreeAll() noexcept;
    void InitRegionIdx(size_t region_size) noexcept;
    void TryCreateBranchIfNeeded(APCPagedNodeRelMaskClasses rel_mask_hint = APCPagedNodeRelMaskClasses::FREE_SLOT) noexcept;
    void SetManagerForGlobalAPC(PackedCellContainerManager* pointer_of_global_apc_manager) noexcept;
    bool TryPublishRegionalSharedGrowthOnce(APCPagedNodeRelMaskClasses region_kind, packed64_t packed_cell, std::atomic<uint64_t>* growth_counter = nullptr) noexcept;
    PublishResult PublishCellByRegionMAskTraverseStartsFromThisAPC(APCPagedNodeRelMaskClasses region_kind, packed64_t cell_to_publish, uint16_t max_tries = MAX_TRIES) noexcept;
    AdaptivePackedCellContainer* GrowSharedNodeByRegionKind(APCPagedNodeRelMaskClasses desired_region_kind, bool enable_branching = true) noexcept;
    std::optional<packed64_t> ConsumeCellByRegionMaskTraverseStartFromThisAPC(APCPagedNodeRelMaskClasses region_kind, size_t& scan_cursor) noexcept;
    AdaptivePackedCellContainer* FindSharedRootOrThis() noexcept;
    AdaptivePackedCellContainer* GetNextSharedSegment() noexcept;
    bool IsAPCSharedChainEmpty() noexcept;
    uint32_t GetBranchId() const noexcept;
    uint32_t GetLogicalId() const noexcept;
    uint32_t GetSharedId() const noexcept;
    size_t ReserveProducerSlots(size_t number_of_slots) noexcept;
    size_t NextProducerSequence() noexcept;

    bool PublishToPort(APCPortSlot port_slot, packed64_t packed_cell, uint16_t max_tries = MAX_TRIES) noexcept;

    size_t OccupancyAddOrSubAndGetAfterChange(int delta = 0) noexcept;

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

    static constexpr uint32_t PayloadBegin() noexcept
    {
        return PackedCellBranchPlugin::METACELL_COUNT;
    }
    
    inline bool IfAPCBranchValid() const noexcept
    {
        return (BackingPtr && GetPayloadCapacity() >= MINIMUM_BRANCH_CAPACITY - PayloadBegin());
    }

    uint32_t ProducerORConsumerCursorSetAndGet_(std::optional<uint32_t> cursor_placement = std::nullopt, int32_t increment_or_decrement_of_cursor = 0, 
        bool* did_changed_easy_return = nullptr, const MetaIndexOfAPCNode cursors_meta_idx = MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT
    ) noexcept;

    uint32_t GetProducerCursorPlacement() noexcept
    {
        return BranchPluginOfAPC_->ReadMetaCellValue32(MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT);
    }

    bool UpdateProducerCursorPlacement(uint32_t new_cursor_placement_idx) noexcept
    {
        bool will_return = false;
        ProducerORConsumerCursorSetAndGet_(new_cursor_placement_idx, 0, &will_return, MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT);
        return will_return;
    }

    bool ProducerCursorIncrementOrdecrement(int32_t increment_decrement_value)  noexcept
    {
        bool will_retuen = false;
        ProducerORConsumerCursorSetAndGet_(std::nullopt, increment_decrement_value, &will_retuen, MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT);
        return will_retuen;
    }

    uint32_t GetConsumerCursorPlacement() noexcept
    {
        return BranchPluginOfAPC_->ReadMetaCellValue32(MetaIndexOfAPCNode::CONSUMER_CURSORE_PLACEMENT);
    }

    bool UpdateConsumerCursorPlacement(uint32_t new_cursor_value) noexcept
    {
        bool will_return = false;
        ProducerORConsumerCursorSetAndGet_(new_cursor_value, 0, &will_return, MetaIndexOfAPCNode::CONSUMER_CURSORE_PLACEMENT);
        return will_return;
    }

    bool ConsumerCursorIncrementOrDecrement(int32_t increment_decrement_value) noexcept
    {
        bool will_return = false;
        ProducerORConsumerCursorSetAndGet_(std::nullopt, increment_decrement_value, &will_return, MetaIndexOfAPCNode::CONSUMER_CURSORE_PLACEMENT);
        return will_return;
    }

    PackedCellContainerManager* GetAPCManager() noexcept
    {
        if (!APCManagerPtr_)
        {
            return nullptr;
        }
        return APCManagerPtr_;
    }

    MasterClockConf* GetMasterClockPtrForThisAPC() noexcept
    {
        if (!APCManagerPtr_)
        {
            return nullptr;
        }
        return (APCManagerPtr_->GetMasterClockAdaptivePackedCellContainerManager().GetMasterClockPtr());
    }

    //Paired Pointer functions-- have to separate into a class or struct for better use and update 
    PublishResult PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask = 0, int max_probs = -1) noexcept;
    bool PublishHeapPtrWithAdaptiveBackoff(void* target_publishable_ptr, uint16_t max_retries = 100);
    std::optional<AcquirePairedPointerStruct> AcquirePairedAtomicPtr(size_t probable_idx, bool claim_ownership = true, int max_claim_attempts = 256) noexcept;
    bool ReleaseAcquiredPairedPtr(const AcquirePairedPointerStruct& acquired_pair_struct, PackedCellLocalityTypes desired_locality = PackedCellLocalityTypes::ST_IDLE) noexcept;
    void RetireAcquiredPointerPair(const AcquirePairedPointerStruct& acquired_pair_struct) noexcept;
    template<typename TypePtr>
    std::optional<TypePtr> ViewPointerMemoryIfAssembeled(size_t probable_idx) noexcept;
    //
};


struct AdaptivePackedCellContainer::QSBRGuard
{
    bool IsQSBRGuardActive{false};
    AdaptivePackedCellContainer* ParentContainer{nullptr};

    QSBRGuard(AdaptivePackedCellContainer* apc_ptr = nullptr) noexcept :
        ParentContainer(apc_ptr)
    {
        if (ParentContainer)
        {
            ParentContainer ->QSBREnterCritical_();
            IsQSBRGuardActive = true;
        }
        
    }

    ~QSBRGuard() noexcept 
    {
        if (IsQSBRGuardActive)
        {
            ParentContainer->QSBRExitCritical_();
        }
    }
    QSBRGuard(const QSBRGuard&) = delete;
    QSBRGuard& operator = (const QSBRGuard&) = delete;
    QSBRGuard(QSBRGuard&& oprtr) noexcept :
        ParentContainer(oprtr.ParentContainer), IsQSBRGuardActive(oprtr.IsQSBRGuardActive)
    {
        oprtr.IsQSBRGuardActive = false;//1
        oprtr.ParentContainer = nullptr;//2
    }
};

}  