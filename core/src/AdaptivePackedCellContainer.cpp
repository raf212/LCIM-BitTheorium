#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    class PackedCellContainerManager;

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
    

    uint32_t AdaptivePackedCellContainer::GetBranchId() const noexcept
    {
        if (BranchPluginOfAPC_)
        {
            return BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::BRANCH_ID);
        }
        return NO_VAL;
    }

    uint32_t AdaptivePackedCellContainer::GetLogicalId() const noexcept
    {
        if (BranchPluginOfAPC_)
        {
            return BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::LOGICAL_NODE_ID);
        }
        return NO_VAL;
    }

    uint32_t AdaptivePackedCellContainer::GetSharedId() const noexcept
    {
        if (BranchPluginOfAPC_)
        {
            return BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_ID);
        }
        return NO_VAL;
    }

    size_t AdaptivePackedCellContainer::ReserveProducerSlots(size_t number_of_slots) noexcept
    {
        if (!IfAPCBranchValid() || number_of_slots == 0)
        {
            return SIZE_MAX;
        }

        if (GetPayloadEnd() <= PayloadBegin())
        {
            return SIZE_MAX;
        }
        while (true)
        {
            const uint32_t current_producer_cursor = GetProducerCursorPlacement();
            if (current_producer_cursor == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                return SIZE_MAX;
            }

            uint64_t base = (current_producer_cursor < PayloadBegin()) ? PayloadBegin() : current_producer_cursor;
            uint64_t desired_cursor_placement = static_cast<uint64_t>(base) + static_cast<uint64_t>(number_of_slots);
            if (desired_cursor_placement > static_cast<uint64_t>(GetPayloadEnd()))
            {
                return SIZE_MAX;
            }
            bool changed = false;
            ProducerORConsumerCursorSetAndGet_(
                static_cast<uint32_t>(desired_cursor_placement),
                0,
                &changed,
                PackedCellBranchPlugin::MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT
            );
            if (changed)
            {
                return static_cast<size_t>(base);
            }
        }
    }

    size_t AdaptivePackedCellContainer::GetHashedRendomizedStep_(size_t sequense_number) noexcept
    {
        uint64_t mix_hash = (
            (static_cast<uint64_t>(sequense_number) * ID_HASH_GOLDEN_CONST) ^ (static_cast<uint64_t>(sequense_number >> (VALBITS + 1)))
        );
        size_t step = 1;
        if (GetPayloadCapacity() > 1)
        {
            step = static_cast<size_t>((mix_hash % (GetPayloadCapacity() - 1)) + 1);
        }
        return step;
    }

    void AdaptivePackedCellContainer::SetManagerForGlobalAPC(PackedCellContainerManager* pointer_of_global_apc_manager) noexcept
    {
        if (pointer_of_global_apc_manager)
        {
            try
            {
                pointer_of_global_apc_manager->StartAPCManager();
                APCManagerPtr_ = pointer_of_global_apc_manager;
            }
            catch(...)
            {
                pointer_of_global_apc_manager = nullptr;
            }
        }
    }

    void AdaptivePackedCellContainer::UpdateRegionRelForIdx_(tag8_t rel_mask) noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        return BranchPluginOfAPC_->OrReadyRelMask(rel_mask);
    }

    
    void AdaptivePackedCellContainer::InitOwned(size_t container_capacity,
        ContainerConf container_cfg
    )
    {
        
        FreeAll();
        if (container_capacity == 0)
        {
            throw std::invalid_argument("Capacity == 0");
        }
        if (container_capacity <= PayloadBegin() + 2)
        {
            throw std::invalid_argument("Capacity is too small for APC.");
        }
        
        BackingPtr = new std::atomic<packed64_t>[container_capacity];
        packed64_t idle_cell = PackedCell64_t::MakeInitialPacked(container_cfg.InitialMode);
        for (size_t i = 0; i < container_capacity; i++)
        {
            BackingPtr[i].store(idle_cell, MoStoreUnSeq_);
        }
        
        // attach manager-provided master clock and adaptive backoff only after allocations succeed
        try {
            MasterClockConfPtr_ = &PackedCellContainerManager::Instance().GetMasterClockAdaptivePackedCellContainerManager();
            AdaptiveBackoffOfAPCPtr_ = &PackedCellContainerManager::Instance().GetManagersAdaptiveBackoff();
            if (AdaptiveBackoffOfAPCPtr_ && MasterClockConfPtr_) {
                AdaptiveBackoffOfAPCPtr_->AttachMasterClockToAadaptiveBackOff(MasterClockConfPtr_);
            }
        } catch (...) {
            // best-effort; do not throw for integration issues
            MasterClockConfPtr_ = nullptr;
            AdaptiveBackoffOfAPCPtr_ = nullptr;
            if (APCLogger_) APCLogger_("InitOwned", "Attach masterclock/backoff failed (non-fatal)");
        }
        BranchPluginOfAPC_ = std::make_unique<PackedCellBranchPlugin>();
        BranchPluginOfAPC_->Bind(BackingPtr, container_capacity, MasterClockConfPtr_);
        const uint32_t new_branch_id = GlobalBranchIdAlloc_.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t logical_node_id = new_branch_id;
        const uint32_t shared_id = NO_VAL;
        BranchPluginOfAPC_->InitRootOrChildBranch(
            new_branch_id,
            logical_node_id,
            shared_id,
            container_capacity,
            container_cfg
        );
        BranchPluginOfAPC_->InitLogicalNodeIdentity(
            logical_node_id,
            shared_id,
            true
        );
        InitZeroState_();
        if (container_cfg.RegionSize > 0)
        {
            InitRegionIdx(container_cfg.RegionSize);
        }
        if (APCManagerPtr_)
        {
            APCManagerPtr_->RegisterAdaptivePackedCellContainer(this);
        }
        RefreshAPCMeta_();
    }

    void AdaptivePackedCellContainer::InitAPCAsNode(
        size_t capacity,
        const ContainerConf& container_configuration,
        uint32_t node_role_flags,
        PackedCellBranchPlugin::APCNodeComputeKind compute_kind,
        uint32_t aux_param_u32
    )
    {
        InitOwned(capacity, container_configuration);
        if (BranchPluginOfAPC_)
        {
            BranchPluginOfAPC_->InitNodeSemantics(node_role_flags, compute_kind, aux_param_u32);
            BranchPluginOfAPC_->SetGraphNodeFlag();
        }
    }

    void AdaptivePackedCellContainer::InitZeroState_() noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        BranchPluginOfAPC_->ForceOccupancyUpdateAndReturn(0);
        BranchPluginOfAPC_->MakeAPCBranchOwned();
        BranchPluginOfAPC_->ResetTotalCASFailureForThisBranch();
        UpdateProducerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
        UpdateConsumerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
    }

    void AdaptivePackedCellContainer::FreeAll() noexcept
    {
        if (!IfAPCBranchValid())
        {
            return;
        }
        
        try 
        {
            PackedCellContainerManager::Instance().UnRegisterAdaptivePackedCellContainer(this);
        }
        catch (...)
        {

        }
        AdaptiveBackoffOfAPCPtr_ = nullptr;
        MasterClockConfPtr_ = nullptr;        
        if (BackingPtr)
        {
            if (BranchPluginOfAPC_->IsBranchOwnedByFlag())
            {
                BranchPluginOfAPC_->ReleseOwneshipFlag();
                delete[] BackingPtr;
            }
            BackingPtr = nullptr;
        }

        RegionRelArray_.reset();
        RegionEpochArray_.reset();
        RelBitmaps_.clear();
        BranchPluginOfAPC_->ReleseOwneshipFlag();
        BranchPluginOfAPC_.reset();
    }

    void AdaptivePackedCellContainer::InitRegionIdx(size_t region_size) noexcept
    {
        if (!IfAPCBranchValid())
        {
            return;
        }
        if (region_size == 0)
        {
            return;
        }
        if (!BranchPluginOfAPC_)
        {
            return;
        }
        uint32_t current_region_size = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::REGION_SIZE);
        bool ok = BranchPluginOfAPC_->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCNode::REGION_SIZE, current_region_size, static_cast<uint32_t>(region_size));
        if (!ok)
        {
            return;
        }
        size_t number_of_region = ((GetPayloadCapacity() + region_size - 1) / region_size);
        uint32_t current_number_of_region = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::REGION_COUNT);
        ok = BranchPluginOfAPC_->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCNode::REGION_COUNT, current_number_of_region, static_cast<uint32_t>(number_of_region));
        if (!ok)
        {
            return;
        }
        RegionRelArray_.reset(
            new std::atomic<uint8_t>[number_of_region]
        );
        RegionEpochArray_.reset(
            new std::atomic<uint64_t>[number_of_region]
        );
        for (size_t region = 0; region < number_of_region; region++)
        {
            RegionRelArray_[region].store(0, MoStoreSeq_);
            RegionEpochArray_[region].store(0, MoStoreSeq_);
        }
        size_t words = (number_of_region + MAX_VAL - 1) / MAX_VAL;
        RelBitmaps_.assign(LN_OF_BYTE_IN_BITS, std::vector<uint64_t>(words, 0ull));
        for (size_t region = 0; region < number_of_region; region++)
        {
            size_t base = region * region_size;
            size_t end = std::min(GetPayloadCapacity(), base + region_size);
            tag8_t accum = 0;
            for (size_t i = base; i < end; i++)
            {
                const size_t absolute_idx = PayloadBegin() + i;
                accum |= PackedCell64_t::ExtractFullRelFromPacked(BackingPtr[absolute_idx].load(MoLoad_));
            }
            RegionRelArray_[region].store(accum, MoStoreSeq_);
            if (accum)
            {
                size_t w = region / MAX_VAL;
                size_t b = region % MAX_VAL;
                uint64_t mask = (1ull << b);
                for (unsigned bit = 0; bit < LN_OF_BYTE_IN_BITS; bit++)
                {
                    if (accum & (1u << bit))
                    {
                        std::atomic_ref<uint64_t>aref(RelBitmaps_[bit][w]);
                        aref.fetch_or(mask, std::memory_order_acq_rel);
                    }
                }
            }
        }
    }

    size_t AdaptivePackedCellContainer::NextProducerSequence() noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return SIZE_MAX;
        }
        size_t current_block_size = static_cast<size_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::PRODUCER_BLOCK_SIZE));
        thread_local size_t block_base = 0;
        thread_local size_t block_left = 0;
        if (block_left == 0)
        {
            size_t block = std::min<size_t>(current_block_size, GetPayloadCapacity());
            size_t base = ReserveProducerSlots(block);
            if (base == SIZE_MAX)
            {
                return SIZE_MAX;
            }
            block_base = base;
            block_left = block;
        }
        size_t seq = block_base++;
        --block_left;
        return seq;
    }

    void PackedCellContainerManager::ProcessRemainingWorkOfAPC_(NodeOfAdaptivePackedCellContainer_* batch_head_apc_ptr, uint64_t min_epoch) noexcept
    {
        (void)min_epoch;
        while (batch_head_apc_ptr)
        {
            NodeOfAdaptivePackedCellContainer_* next_apc = batch_head_apc_ptr->StackNextPtr.load(MoLoad_);
            NodeOfAdaptivePackedCellContainer_* node_ptr = batch_head_apc_ptr;
            if (node_ptr->DeadAPC.load(MoLoad_) || node_ptr->APCContainerPtr == nullptr)
            {
                PushANodeAtHeadInStackOfAdaptivePackedCellContainer_(CleanUpStackHead_, node_ptr);
                batch_head_apc_ptr = next_apc;
                continue;
            }
            AdaptivePackedCellContainer* current_apc_ptr = node_ptr->APCContainerPtr;
            if (current_apc_ptr)
            {
                if (node_ptr->RequestedBranchedAPC.exchange(NO_VAL, std::memory_order_acq_rel))
                {
                    try 
                    {
                        current_apc_ptr->TryCreateBranchIfNeeded();
                    }
                    catch (...)
                    {
                        if (Logger_)
                        {
                            Logger_("BM", "TryCreateBranchIfNeeded threw");
                        }
                    }
                }
            }
            batch_head_apc_ptr = next_apc;   
        }
    }


    void AdaptivePackedCellContainer::RefreshAPCMeta_() noexcept
    {
        if (!IfAPCBranchValid())
        {
            return;
        }
        const uint32_t occupancy_now = static_cast<uint32_t>(OccupancyAddOrSubAndGetAfterChange());
        BranchPluginOfAPC_->ForceOccupancyUpdateAndReturn(occupancy_now);//why to update if same occupancy???
        if (BranchPluginOfAPC_->ShouldSplitNow())
        {
            BranchPluginOfAPC_->TurnOnFlags(static_cast<uint32_t>(PackedCellBranchPlugin::APCFlags::SATURATED));
        }
        else
        {
            BranchPluginOfAPC_->ClearFlags(static_cast<uint32_t>(PackedCellBranchPlugin::APCFlags::SATURATED));
        }
        if (MasterClockConfPtr_)
        {
            BranchPluginOfAPC_->TouchLocalMetaClock48();
        }

        uint32_t current_group_size = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        if (current_group_size == NO_VAL)
        {
            BranchPluginOfAPC_->JustUpdateValueOfMeta32(
                PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE,
                current_group_size,
                1u
            );
        }
        
    }

    AdaptivePackedCellContainer* AdaptivePackedCellContainer::GrowSharedNodeCheaply(bool enable_recursive_branching) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return nullptr;
        }

        if (!BranchPluginOfAPC_->HasThisFlag(PackedCellBranchPlugin::APCFlags::ENABLE_BRANCHING))
        {
            return nullptr;
        }

        if (!BranchPluginOfAPC_->TryMarkSplitInFlight())
        {
            return nullptr;
        }

        auto clear_flags = [&]() noexcept
        {
            BranchPluginOfAPC_->ClearFlags(
                static_cast<uint32_t>(PackedCellBranchPlugin::APCFlags::SPLIT_INFLIGHT)
            );
        };
        AdaptivePackedCellContainer* new_shared_container = nullptr;
        ContainerConf child_configuration{};
        child_configuration.InitialMode = static_cast<PackedMode>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::DEFINED_MODE_OF_CURRENT_APC));
        child_configuration.ProducerBlockSize = static_cast<size_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::PRODUCER_BLOCK_SIZE));
        child_configuration.RegionSize = static_cast<size_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::REGION_SIZE));
        child_configuration.RetireBatchThreshold = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::RETIRE_BRANCH_THRASHOLD);
        child_configuration.BackgroundEpochAdvanceMS = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::BACKGROUND_EPOCH_ADVANCE_MS);
        child_configuration.EnableBranching = enable_recursive_branching;
        child_configuration.BranchSplitThresholdPercentage = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SPLIT_THRESHOLD_PERCENTAGE);
        child_configuration.BranchMaxDepth = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::MAX_DEPTH);
        child_configuration.BranchMinChildCapacity = SuggestedChildCapacity_();


        try
        {
            new_shared_container = new AdaptivePackedCellContainer();
            new_shared_container->SetManagerForGlobalAPC(APCManagerPtr_);
            new_shared_container->InitOwned(child_configuration.BranchMinChildCapacity, child_configuration);
        }
        catch(...)
        {
        }
        const uint32_t this_branch_id = GetBranchId();
        const uint32_t this_logical_id = GetLogicalId();
        const uint32_t this_shared_id = (GetSharedId() == NO_VAL) ? this_branch_id : GetSharedId();

        const uint32_t new_branch_id = new_shared_container->GetBranchId();
        PackedCellBranchPlugin* new_branch_plugin = new_shared_container->GetBranchPlugin();
        if (!new_branch_plugin)
        {
            new_shared_container->FreeAll();
            delete new_shared_container;
            clear_flags();
            return nullptr;
        }

        new_branch_plugin->InitLogicalNodeIdentity(this_logical_id, this_shared_id, false);

        new_branch_plugin->InitNodeSemantics(
            BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_ROLE_FLAGS),
            static_cast<PackedCellBranchPlugin::APCNodeComputeKind>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_COMPUTE_KIND)),
            BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_AUX_PARAM_U32)
        );

        const auto copy_meta = [&](PackedCellBranchPlugin::MetaIndexOfAPCNode idx) noexcept
        {
            const uint32_t original_value = BranchPluginOfAPC_->ReadMetaCellValue32(idx);
            const uint32_t current_value = new_branch_plugin->ReadMetaCellValue32(idx);
            new_branch_plugin->JustUpdateValueOfMeta32(idx, current_value, original_value);
        };
        copy_meta(PackedCellBranchPlugin::MetaIndexOfAPCNode::FEEDFORWARD_IN_TARGET_ID);
        copy_meta(PackedCellBranchPlugin::MetaIndexOfAPCNode::FEEDFORWARD_OUT_TARGET_ID);
        copy_meta(PackedCellBranchPlugin::MetaIndexOfAPCNode::FEEDBACKWARD_IN_TARGET_ID);
        copy_meta(PackedCellBranchPlugin::MetaIndexOfAPCNode::FEEDBACKWARD_OUT_TARGET_ID);
        copy_meta(PackedCellBranchPlugin::MetaIndexOfAPCNode::LATERAL_0_TARGET_ID);
        copy_meta(PackedCellBranchPlugin::MetaIndexOfAPCNode::LATERAL_1_TARGET_ID);

        AdaptivePackedCellContainer* tail = this;
        while (tail)
        {
            PackedCellBranchPlugin* tail_plugin = tail->GetBranchPlugin();
            if (!tail_plugin)
            {
                break;
            }
            const uint32_t next_id = tail_plugin->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);
            if (next_id == PackedCellBranchPlugin::BRANCH_SENTINAL || next_id == NO_VAL)
            {
                const uint32_t current_next = tail_plugin->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);
                if (!tail_plugin->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID, current_next, new_branch_id))
                {
                    new_shared_container->FreeAll();
                    delete new_shared_container;
                    clear_flags();
                    return nullptr;
                }
                const uint32_t new_previous_current = new_branch_plugin->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_PREVIOUS_ID);
                new_branch_plugin->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_PREVIOUS_ID, new_previous_current, tail->GetBranchId());
                break;
            }
            tail = APCManagerPtr_->GetAPCPtrFromBranchId(next_id);
        }
        uint32_t group_size = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        if (group_size == NO_VAL)
        {
            group_size = 1u;
        }
        group_size = group_size + 1;

        BranchPluginOfAPC_->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE, new_branch_plugin->ReadMetaCellValue32(
            PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE
        ),
        group_size
        );
        RefreshAPCMeta_();
        new_shared_container->RefreshAPCMeta_();
        clear_flags();
        return new_shared_container;
    }

    void AdaptivePackedCellContainer::TryCreateBranchIfNeeded() noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return;
        }
        
        if (!BranchPluginOfAPC_->HasThisFlag(
            PackedCellBranchPlugin::APCFlags::ENABLE_BRANCHING
        ))
        {
            return;
        }

        if(!BranchPluginOfAPC_->ShouldSplitNow())
        {
            return;
        }

        AdaptivePackedCellContainer* grown_apc = GrowSharedNodeCheaply();
        if (grown_apc)
        {
            APCManagerPtr_->RequestForReclaimationOfTheAdaptivePackedCellContainer(grown_apc);
        }
        
    }

    size_t AdaptivePackedCellContainer::SuggestedChildCapacity_() const noexcept
    {
        const size_t payload_capacity = GetPayloadCapacity();
        const size_t child_payload_size = std::max<size_t>(MINIMUM_BRANCH_CAPACITY, payload_capacity / 2);
        return child_payload_size + PayloadBegin();
    }

    AdaptivePackedCellContainer* PackedCellContainerManager::GetAPCPtrFromBranchId(uint32_t branch_id) noexcept
    {
        if (branch_id == NO_VAL || branch_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
        {
            return nullptr;
        }
        NodeOfAdaptivePackedCellContainer_* cur_node_of_apc_ptr = RegistryHeadOfAPCNodesPtr_.load(MoLoad_);
        while (cur_node_of_apc_ptr)
        {
            AdaptivePackedCellContainer* apc_ptr = cur_node_of_apc_ptr->APCContainerPtr;
            if (apc_ptr && !cur_node_of_apc_ptr->DeadAPC.load(MoLoad_))
            {
                if (apc_ptr->GetBranchId() == branch_id)
                {
                    return apc_ptr;
                }
            }
            cur_node_of_apc_ptr = cur_node_of_apc_ptr->RegistryNextPtr;
        }
        return nullptr;
    }

    uint32_t AdaptivePackedCellContainer::ProducerORConsumerCursorSetAndGet_(std::optional<uint32_t> cursor_placement, int32_t increment_or_decrement_of_cursor, 
        bool* did_changed_easy_return, const PackedCellBranchPlugin::MetaIndexOfAPCNode cursors_meta_idx
    ) noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            if (did_changed_easy_return)
            {
                *did_changed_easy_return = false;
            }
            return PackedCellBranchPlugin::BRANCH_SENTINAL;
        }
        if (GetPayloadEnd() <= PayloadBegin())
        {
            if (did_changed_easy_return)
            {
                *did_changed_easy_return = false;
            }
            return PackedCellBranchPlugin::BRANCH_SENTINAL;
        }
        
        while (true)
        {
            const uint32_t current_cursor_placement = BranchPluginOfAPC_->ReadMetaCellValue32(cursors_meta_idx);
            uint32_t desired_cursor_place = current_cursor_placement;
            if (cursor_placement.has_value())
            {
                desired_cursor_place = cursor_placement.value();
            }
            else if (increment_or_decrement_of_cursor != 0)
            {
                if (current_cursor_placement == PackedCellBranchPlugin::BRANCH_SENTINAL)
                {
                    if (did_changed_easy_return)
                    {
                        *did_changed_easy_return = false;
                    }
                    return current_cursor_placement;
                }
                const int64_t winded_cursor = static_cast<int64_t>(current_cursor_placement) + static_cast<int64_t>(increment_or_decrement_of_cursor);
                if (winded_cursor < static_cast<int64_t>(PayloadBegin()))
                {
                    desired_cursor_place = PayloadBegin();
                }
                else if (winded_cursor >= static_cast<int64_t>(GetPayloadEnd()))
                {
                    desired_cursor_place = static_cast<uint32_t>(GetPayloadEnd() - 1u);
                }
                else
                {
                    desired_cursor_place = static_cast<uint32_t>(winded_cursor);
                }
            }
            else
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = false;
                }
                return current_cursor_placement;
            }
            
            if (desired_cursor_place < PayloadBegin())
            {
                desired_cursor_place = PayloadBegin();
            }
            else if (desired_cursor_place >= GetPayloadEnd())
            {
                desired_cursor_place = static_cast<uint32_t>(GetPayloadEnd() - 1u);
            }

            if (desired_cursor_place == current_cursor_placement)
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = false;
                }
                return current_cursor_placement;
            }
            if (BranchPluginOfAPC_->JustUpdateValueOfMeta32(cursors_meta_idx, current_cursor_placement, desired_cursor_place))
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = true;
                }
                return desired_cursor_place;
            }
        }
    }
    
    size_t AdaptivePackedCellContainer::OccupancyAddOrSubAndGetAfterChange(int delta) noexcept
    {
        if (!BranchPluginOfAPC_)
        {
            return SIZE_MAX;
        }

        if (delta == 0)
        {
            return static_cast<size_t>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT));
        }
        while (true)
        {
            packed64_t current_occupancy_cell = BranchPluginOfAPC_->ReadFullMetaCell(PackedCellBranchPlugin::MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT);
            val32_t current_occupancy = PackedCell64_t::ExtractValue32(current_occupancy_cell);
            if (current_occupancy == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                return PackedCellBranchPlugin::BRANCH_SENTINAL;
            }
            
            int64_t next_occupancy_winded = static_cast<int64_t>(current_occupancy) + static_cast<int64_t>(delta);
            if (next_occupancy_winded < 0)
            {
                next_occupancy_winded = 0;
            }
            constexpr int64_t high_val = static_cast<int64_t>(PackedCellBranchPlugin::BRANCH_SENTINAL - 1u);
            if (next_occupancy_winded > high_val)
            {
                next_occupancy_winded = high_val;
            }
            
            uint32_t next_occupancy = static_cast<uint32_t>(next_occupancy_winded);
            if (BranchPluginOfAPC_->JustUpdateValueOfMeta32(PackedCellBranchPlugin::MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT, current_occupancy, next_occupancy))
            {
                return static_cast<size_t>(next_occupancy);
            }
            if (APCManagerPtr_)
            {
                auto& backoff = APCManagerPtr_->GetManagersAdaptiveBackoff();
                backoff.AdaptiveBackOffPacked(current_occupancy_cell);
            }
            
        }
    }

    bool AdaptivePackedCellContainer::WriteGenericValueCellWithCASClaimedManager(packed64_t packed_cell, uint16_t max_tries) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return false;
        }

        auto try_write_into_one = [&](AdaptivePackedCellContainer& target_apc) noexcept->bool
        {
            const size_t payload_capacity = target_apc.GetPayloadCapacity();
            if (payload_capacity == 0)
            {
                return false;
            }
            uint16_t tries = 0;
            while (tries++ < max_tries)
            {
                const size_t next_sequense = target_apc.NextProducerSequence();
                if (next_sequense == SIZE_MAX)
                {
                    return false;
                }

                size_t idx = PayloadBegin() + ((next_sequense - PayloadBegin()) % payload_capacity);
                size_t step = 1u + ((next_sequense * ID_HASH_GOLDEN_CONST) % ((payload_capacity > 1) ? (payload_capacity - 1) : 1));
                for (size_t prob = 0; prob < payload_capacity; prob++)
                {
                    packed64_t current_cell = target_apc.BackingPtr[idx].load(MoLoad_);
                    if (PackedCell64_t::ExtractLocalityFromPacked(current_cell) == PackedCellLocalityTypes::ST_IDLE)
                    {
                        packed64_t local_claimed = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_CLAIMED);
                        packed64_t expected_cell = current_cell;
                        if (target_apc.BackingPtr[idx].compare_exchange_strong(expected_cell, local_claimed, OnExchangeSuccess, OnExchangeFailure))
                        { 
                            target_apc.BackingPtr[idx].store(packed_cell, MoStoreSeq_);
                            target_apc.BackingPtr[idx].notify_all();
                            target_apc.OccupancyAddOrSubAndGetAfterChange(+1);
                            target_apc.BranchPluginOfAPC_->TouchLocalMetaClock48();
                            target_apc.RefreshAPCMeta_();
                            return true;
                        }
                        else
                        {
                            target_apc.GetBranchPlugin()->TotalCASFailForThisBranchIncreaseAndGet(1u);
                        }
                    }
                    idx = PayloadBegin() + ((idx - PayloadBegin() + step) % payload_capacity);
                }
                const size_t observed_idx = PayloadBegin() + (next_sequense % payload_capacity);
                APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(BackingPtr[observed_idx].load(MoLoad_));
            }
            return false;
        };

        if (try_write_into_one(*this))
        {
            return true;
        }

        if (BranchPluginOfAPC_->ShouldSplitNow())
        {
            APCManagerPtr_->RequestBranchCreationForTheAdaptivePackedCellContainer(this);
            AdaptivePackedCellContainer* grown_this_apc = GrowSharedNodeCheaply();
            if (grown_this_apc && try_write_into_one(*grown_this_apc))
            {
                return true;
            }
        }
        uint32_t next_branch_shared_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);

        while (next_branch_shared_id != NO_VAL && next_branch_shared_id != PackedCellBranchPlugin::BRANCH_SENTINAL)
        {
            AdaptivePackedCellContainer* sibling_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(next_branch_shared_id);
            if (!sibling_apc_ptr)
            {
                break;
            }
            if (try_write_into_one(*sibling_apc_ptr))
            {
                return true;
            }
            next_branch_shared_id = sibling_apc_ptr->GetBranchPlugin()->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);
        }
        return false;
    }

    bool AdaptivePackedCellContainer::ConsumeAndIdleGenericValueCell(size_t& scan_cursor, packed64_t& out_cell) noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return false;
        }

        auto try_consume_one_apc = [&](AdaptivePackedCellContainer& target_apc, size_t& scan_cursor) noexcept->bool
        {
            const size_t payload_capacity = target_apc.GetPayloadCapacity();
            if (payload_capacity == 0)
            {
                return false;
            }
            for (size_t prob = 0; prob < payload_capacity; prob++)
            {
                const size_t idx = PayloadBegin() + ((scan_cursor - PayloadBegin() + prob) % payload_capacity);
                packed64_t current_cell = target_apc.BackingPtr[idx].load(MoLoad_);
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
                if (!target_apc.BackingPtr[idx].compare_exchange_strong(expected_cell, local_claimed, OnExchangeSuccess, OnExchangeFailure))
                {
                    APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(expected_cell);
                    target_apc.GetBranchPlugin()->TotalCASFailForThisBranchIncreaseAndGet(1u);
                    continue;
                }
                out_cell = current_cell;

                const PackedMode old_mode = PackedCell64_t::ExtractModeOfPackedCellFromPacked(current_cell);
                const PackedCellDataType old_dtype = PackedCell64_t::ExtractPCellDataTypeFromPacked(current_cell);

                target_apc.BackingPtr[idx].store(PackedCell64_t::MakeInitialPacked(old_mode, old_dtype), MoStoreSeq_);
                target_apc.BackingPtr[idx].notify_all();
                target_apc.OccupancyAddOrSubAndGetAfterChange(-1);
                target_apc.RefreshAPCMeta_();
                scan_cursor = idx + 1;
                if (scan_cursor >= (PayloadBegin() + payload_capacity))
                {
                    scan_cursor = PayloadBegin();
                }
                return true;
            }
            return false;
        };
        if (try_consume_one_apc(*this, scan_cursor))
        {
            return true;
        }

        uint32_t next_apc_shared_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);

        while (next_apc_shared_id != NO_VAL && next_apc_shared_id != PackedCellBranchPlugin::BRANCH_SENTINAL)
        {
            AdaptivePackedCellContainer* sibling_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(next_apc_shared_id);
            if (!sibling_apc_ptr)
            {
                break;
            }
            size_t sibling_cursor = PayloadBegin();
            if (try_consume_one_apc(*sibling_apc_ptr, sibling_cursor))
            {
                return true;
            }
            next_apc_shared_id = sibling_apc_ptr->GetBranchPlugin()->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);
        }
        return false;
    }

}
