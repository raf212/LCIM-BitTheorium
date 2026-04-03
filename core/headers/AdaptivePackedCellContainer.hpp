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
#define CURRENT_BRANCHING_CLIENT  3

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

    void UpdateRegionRelForIdx_(tag8_t rel_mask) noexcept;

    void InitZeroState_() noexcept;

    void RefreshAPCMeta_() noexcept;

    size_t SuggestedChildCapacity_() const noexcept;

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

    uint32_t GetBranchId() const noexcept;

    size_t ReserveProducerSlots(size_t number_of_slots) noexcept;

    size_t NextProducerSequence() noexcept;

    void InitOwned(size_t cpacity, ContainerConf container_cfg = {});

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

    inline void DirectStoreCellToAPCIdx(size_t idx, packed64_t cell) noexcept
    {
        if (IfValidPayloadIndex_(idx))
        {
            BackingPtr[idx].store(cell, MoStoreSeq_);
        }
    }

    uint32_t ProducerORConsumerCursorSetAndGet_(std::optional<uint32_t> cursor_placement = std::nullopt, int32_t increment_or_decrement_of_cursor = 0, 
        bool* did_changed_easy_return = nullptr, const PackedCellBranchPlugin::MetaIndexOfAPCBranch cursors_meta_idx = PackedCellBranchPlugin::MetaIndexOfAPCBranch::PRODUCER_CURSOR_PLACEMENT
    ) noexcept;

    uint32_t GetProducerCursorPlacement() noexcept
    {
        return BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::PRODUCER_CURSOR_PLACEMENT);
    }

    bool UpdateProducerCursorPlacement(uint32_t new_cursor_placement_idx) noexcept
    {
        bool will_return = false;
        ProducerORConsumerCursorSetAndGet_(new_cursor_placement_idx, 0, &will_return, PackedCellBranchPlugin::MetaIndexOfAPCBranch::PRODUCER_CURSOR_PLACEMENT);
        return will_return;
    }

    bool ProducerCursorIncrementOrdecrement(int32_t increment_decrement_value)  noexcept
    {
        bool will_retuen = false;
        ProducerORConsumerCursorSetAndGet_(std::nullopt, increment_decrement_value, &will_retuen, PackedCellBranchPlugin::MetaIndexOfAPCBranch::PRODUCER_CURSOR_PLACEMENT);
        return will_retuen;
    }

    uint32_t GetConsumerCursorPlacement() noexcept
    {
        return BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::CONSUMER_CURSORE_PLACEMENT);
    }

    bool UpdateConsumerCursorPlacement(uint32_t new_cursor_value) noexcept
    {
        bool will_return = false;
        ProducerORConsumerCursorSetAndGet_(new_cursor_value, 0, &will_return, PackedCellBranchPlugin::MetaIndexOfAPCBranch::CONSUMER_CURSORE_PLACEMENT);
        return will_return;
    }

    bool ConsumerCursorIncrementOrDecrement(int32_t increment_decrement_value) noexcept
    {
        bool will_return = false;
        ProducerORConsumerCursorSetAndGet_(std::nullopt, increment_decrement_value, &will_return, PackedCellBranchPlugin::MetaIndexOfAPCBranch::CONSUMER_CURSORE_PLACEMENT);
        return will_return;
    }

    bool WriteGenericValueCellWithCASClaimedManager(packed64_t packed_cell, uint16_t max_tries = MAX_TRIES) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return false;
        }
        const size_t payload_capacity = GetPayloadCapacity();
        if (payload_capacity == 0)
        {
            return false;
        }
        
        uint16_t tries = 0;
        while (tries++ < max_tries)
        {
            const size_t next_sequense = NextProducerSequence();
            if (next_sequense == SIZE_MAX)
            {
                return false;
            }

            size_t idx = PayloadBegin() + ((next_sequense - PayloadBegin()) % payload_capacity);
            size_t step = 1u + ((next_sequense * ID_HASH_GOLDEN_CONST) % ((payload_capacity > 1) ? (payload_capacity - 1) : 1));
            for (size_t prob = 0; prob < payload_capacity; prob++)
            {
                packed64_t current_cell = BackingPtr[idx].load(MoLoad_);
                if (PackedCell64_t::ExtractLocalityFromPacked(current_cell) == PackedCellLocalityTypes::ST_IDLE)
                {
                    packed64_t local_claimed = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_CLAIMED);
                    packed64_t expected_cell = current_cell;
                    if (BackingPtr[idx].compare_exchange_strong(expected_cell, local_claimed, OnExchangeSuccess, OnExchangeFailure))
                    {
                        BackingPtr[idx].store(packed_cell, MoStoreSeq_);
                        BackingPtr[idx].notify_all();
                        OccupancyAddOrSubAndGetAfterChange(+1);
                        BranchPluginOfAPC_->TouchLocalMetaClock48();
                        if (BranchPluginOfAPC_->ShouldSplitNow())
                        {
                            APCManagerPtr_->RequestBranchCreationForTheAdaptivePackedCellContainer(this);
                        }
                        return true;
                    }
                }
                idx = PayloadBegin() + ((idx - PayloadBegin() + step) % payload_capacity);
            }
            const size_t observed_idx = PayloadBegin() + (next_sequense % payload_capacity);
            APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(BackingPtr[observed_idx].load(MoLoad_));
        }
        return false;
    }

    bool ConsumeAndIdleGenericValueCell(size_t& scan_cursor, packed64_t& out_cell) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return false;
        }
        const size_t payload_capacity = GetPayloadCapacity();
        if (payload_capacity == 0)
        {
            return false;
        }
        for (size_t prob = 0; prob < payload_capacity; prob++)
        {
            const size_t idx = PayloadBegin() + ((scan_cursor - PayloadBegin() + prob) % payload_capacity);
            packed64_t current_cell = BackingPtr[idx].load(MoLoad_);
            if (PackedCell64_t::ExtractLocalityFromPacked(current_cell) != PackedCellLocalityTypes::ST_PUBLISHED)
            {
                continue;
            }
            if (static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(current_cell)) != RelOffsetMode32::RELOFFSET_GENERIC_VALUE)
            {
                continue;
            }
            
            packed64_t local_claimed = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_CLAIMED);
            packed64_t expected_cell = current_cell;
            if (!BackingPtr[idx].compare_exchange_strong(expected_cell, local_claimed, OnExchangeSuccess, OnExchangeFailure))
            {
                APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(expected_cell);
                continue;
            }
            out_cell = current_cell;

            const PackedMode old_mode = PackedCell64_t::ExtractModeOfPackedCellFromPacked(current_cell);
            const PackedCellDataType old_dtype = PackedCell64_t::ExtractPCellDataTypeFromPacked(current_cell);

            BackingPtr[idx].store(PackedCell64_t::MakeInitialPacked(old_mode, old_dtype), MoStoreSeq_);
            BackingPtr[idx].notify_all();
            OccupancyAddOrSubAndGetAfterChange(-1);
            scan_cursor = idx + 1;
            if (scan_cursor >= (PayloadBegin() + payload_capacity))
            {
                scan_cursor = PayloadBegin();
            }
            return true;
        }
        return false;
    }

    //has to replaced with node logic
    struct BinaryFanOutView
    {
        AdaptivePackedCellContainer* SelfPtr = nullptr;
        AdaptivePackedCellContainer* LeftChildPtr = nullptr;
        AdaptivePackedCellContainer* RightCgildPtr = nullptr;
    };

    std::optional<BinaryFanOutView> GetAFanOut(
    ) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return std::nullopt;
        }

        BinaryFanOutView out_binary_fanout{};
        out_binary_fanout.SelfPtr = this;

        const uint32_t left_branch_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::LEFT_CHILD_ID);
        const uint32_t right_branch_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::RIGHT_CHILD_ID);

        if (left_branch_id != PackedCellBranchPlugin::BRANCH_SENTINAL)
        {
            out_binary_fanout.LeftChildPtr = APCManagerPtr_->GetAPCPtrFromBranchId(left_branch_id);
        }
        
        if (right_branch_id != PackedCellBranchPlugin::BRANCH_SENTINAL)
        {
            out_binary_fanout.RightCgildPtr = APCManagerPtr_->GetAPCPtrFromBranchId(right_branch_id);
        }
        return out_binary_fanout;
    }
    

};


}  