#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    class PackedCellContainerManager;
    
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
        const size_t payload_capacity = GetPayloadCapacity();
        if (payload_capacity == 0)
        {
            return SIZE_MAX;
        }
        if (number_of_slots > payload_capacity)
        {
            number_of_slots = payload_capacity;
        }
        while (true)
        {
            uint32_t current_producer_cursor = GetProducerCursorPlacement();
            if (current_producer_cursor == PackedCellBranchPlugin::BRANCH_SENTINAL || current_producer_cursor < PayloadBegin() || current_producer_cursor >= GetPayloadEnd())
            {
                current_producer_cursor = PayloadBegin();
            }
            const size_t current_offset = static_cast<size_t>(current_producer_cursor - PayloadBegin()) % payload_capacity;
            const size_t next_offset = (current_offset + number_of_slots) % payload_capacity;
            const uint32_t desired_cursor = static_cast<uint32_t>(PayloadBegin() + next_offset);
            bool changed = false;
            ProducerORConsumerCursorSetAndGet_(
                static_cast<uint32_t>(desired_cursor),
                0,
                &changed,
                PackedCellBranchPlugin::MetaIndexOfAPCNode::PRODUCER_CURSOR_PLACEMENT
            );
            if (changed)
            {
                return static_cast<size_t>(current_producer_cursor);
            }
        }
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


    
    void AdaptivePackedCellContainer::InitOwned(size_t container_capacity,
        ContainerConf container_cfg
    )
    {
        
        FreeAll();
        if (container_capacity == 0)
        {
            throw std::invalid_argument("Capacity == 0");
        }
        if (container_capacity <= MINIMUM_BRANCH_CAPACITY)
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


    void AdaptivePackedCellContainer::InitRegionIdx(size_t region_size) noexcept
    {
        if (!IfAPCBranchValid() || region_size == 0)
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



    void AdaptivePackedCellContainer::TryCreateBranchIfNeeded(APCPagedNodeRelMaskClasses rel_mask_hint) noexcept
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

        AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(rel_mask_hint);
        if (grown_apc)
        {
            APCManagerPtr_->RequestForReclaimationOfTheAdaptivePackedCellContainer(grown_apc);
        }
        
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
                APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(target_apc.BackingPtr[observed_idx].load(MoLoad_));
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
            AdaptivePackedCellContainer* grown_this_apc = GrowSharedNodeByRegionKind(APCPagedNodeRelMaskClasses::FREE_SLOT);
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

    AdaptivePackedCellContainer* AdaptivePackedCellContainer::FindSharedRootOrThis() noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return this;
        }
        AdaptivePackedCellContainer* current_apc_ptr = this;
        while (current_apc_ptr)
        {
            const uint32_t previous_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_PREVIOUS_ID);
            if (previous_id == NO_VAL || previous_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                break;
            }
            AdaptivePackedCellContainer* previous_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(previous_id);
            if (!previous_apc_ptr || previous_apc_ptr == current_apc_ptr)
            {
                break;
            }
            current_apc_ptr = previous_apc_ptr;
        }
        return current_apc_ptr ? current_apc_ptr : this;
        
    }

    AdaptivePackedCellContainer* AdaptivePackedCellContainer::GetNextSharedSegment() noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return nullptr;
        }

        const uint32_t next_apc_id = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);

        if (next_apc_id == NO_VAL || next_apc_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
        {
            return nullptr;
        }
        return APCManagerPtr_->GetAPCPtrFromBranchId(next_apc_id);
    }

    bool AdaptivePackedCellContainer::IsAPCSharedChainEmpty() noexcept
    {
        if (!IfAPCBranchValid() || !APCManagerPtr_)
        {
            return true;
        }
        AdaptivePackedCellContainer* current_apc_ptr = FindSharedRootOrThis();
        while (current_apc_ptr)
        {
            if (current_apc_ptr->OccupancyAddOrSubAndGetAfterChange() > NO_VAL)
            {
                return false;
            }
            if (!current_apc_ptr->IfAPCBranchValid())
            {
                break;
            }
            PackedCellBranchPlugin* current_branch_plugin = current_apc_ptr->GetBranchPlugin();
            uint32_t next_apc_id = current_branch_plugin->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);
            if (next_apc_id == NO_VAL || next_apc_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                break;
            }
            AdaptivePackedCellContainer* next_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(next_apc_id);
            if (!next_apc_ptr || next_apc_ptr == current_apc_ptr)
            {
                break;
            }
            current_apc_ptr = next_apc_ptr;
        }
        return true;
    }

    bool AdaptivePackedCellContainer::TryConsumeFromSharedChain(packed64_t& out_cell_easy_return, size_t& root_scan_cursor) noexcept
    {
        if (!APCManagerPtr_ || !IfAPCBranchValid())
        {
            return false;
        }

        AdaptivePackedCellContainer* current_apc_ptr = FindSharedRootOrThis();
        while (current_apc_ptr)
        {
            if (current_apc_ptr->ConsumeAndIdleGenericValueCell(root_scan_cursor, out_cell_easy_return))
            {
                return true;
            }
            PackedCellBranchPlugin* current_branch_plugin = current_apc_ptr->GetBranchPlugin();
            if (!current_branch_plugin)
            {
                break;
            }
            const uint32_t next_apc_id = current_branch_plugin->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::SHARED_NEXT_ID);
            if (next_apc_id == NO_VAL || next_apc_id == PackedCellBranchPlugin::BRANCH_SENTINAL)
            {
                break;
            }
            AdaptivePackedCellContainer* next_apc_ptr = APCManagerPtr_->GetAPCPtrFromBranchId(next_apc_id);
            if (!next_apc_ptr || next_apc_ptr == current_apc_ptr)
            {
                break;
            }
            current_apc_ptr = next_apc_ptr;
        }
        return false;
    }


    bool AdaptivePackedCellContainer::TryPublishRegionalSharedGrowthOnce(APCPagedNodeRelMaskClasses region_kind, packed64_t packed_cell, std::atomic<uint64_t>* growth_counter) noexcept
    {
        const PublishResult local_result = PublishCellByRegionMAskTraverseStartsFromThisAPC(region_kind, packed_cell);
        if (local_result.ResultStatus == PublishStatus::OK)
        {
            return true;
        }
        
        AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(region_kind);
        if (grown_apc)
        {
            if (growth_counter)
            {
                growth_counter->fetch_add(1, std::memory_order_relaxed);
            }
            return grown_apc->PublishCellByRegionMAskTraverseStartsFromThisAPC(region_kind, packed_cell).ResultStatus == PublishStatus::OK;
        }
        return false;
        
    }


    std::optional<packed64_t> AdaptivePackedCellContainer::ConsumeCellByRegionMaskTraverseStartFromThisAPC(APCPagedNodeRelMaskClasses region_kind, size_t& scan_cursor) noexcept
    {
        auto maybe_packed_cell = TryConsumeAndIdleFromRegionLocal_(region_kind, scan_cursor);
        if (maybe_packed_cell)
        {
            return *maybe_packed_cell;
        }

        AdaptivePackedCellContainer* current_apc = GetNextSharedSegment();
        while (current_apc)
        {
            size_t sibling_cursor = PayloadBegin();
            auto maybe_shared_packed_cell = current_apc->TryConsumeAndIdleFromRegionLocal_(region_kind, sibling_cursor);
            if (maybe_shared_packed_cell)
            {
                return *maybe_shared_packed_cell;
            }
            current_apc = current_apc->GetNextSharedSegment();
        }
        return std::nullopt;
    }

    PublishResult AdaptivePackedCellContainer::PublishCellByRegionMAskTraverseStartsFromThisAPC(APCPagedNodeRelMaskClasses region_kind, packed64_t cell_to_publish, uint16_t max_tries) noexcept
    {
        if (!IfAPCBranchValid())
        {
            const PublishResult invalid{};
            return invalid;
        }
        
        const PublishResult local_result = TryPublishToRegionLocal_(region_kind, cell_to_publish, true, max_tries);
        if (local_result.ResultStatus == PublishStatus::OK)
        {
            return local_result;
        }

        AdaptivePackedCellContainer* curren_or_next_container_ptr = GetNextSharedSegment();
        while (curren_or_next_container_ptr)
        {
            const PublishResult sibling_result_publish = curren_or_next_container_ptr->TryPublishToRegionLocal_(region_kind, cell_to_publish, true, max_tries);
            if (sibling_result_publish.ResultStatus == PublishStatus::OK)
            {
                return sibling_result_publish;
            }
            curren_or_next_container_ptr = curren_or_next_container_ptr->GetNextSharedSegment();
        }
        if (BranchPluginOfAPC_->ShouldSplitNow())
        {
            AdaptivePackedCellContainer* grown_apc = GrowSharedNodeByRegionKind(region_kind, true);
            if (grown_apc)
            {
                return grown_apc->TryPublishToRegionLocal_(region_kind, cell_to_publish, true, max_tries);
            }
        }
        return local_result;
    }

    AdaptivePackedCellContainer* AdaptivePackedCellContainer::GrowSharedNodeByRegionKind(APCPagedNodeRelMaskClasses desired_region_kind, bool enable_recursive_branching) noexcept
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
            clear_flags();
            return nullptr;
        }

        if (!new_shared_container)
        {
            clear_flags();
            return nullptr;
        }
        
        PackedCellBranchPlugin* new_branch_plugin = new_shared_container->GetBranchPlugin();
        if (!new_branch_plugin)
        {
            new_shared_container->FreeAll();
            delete new_shared_container;
            clear_flags();
            return nullptr;
        }

        const uint32_t this_branch_id = GetBranchId();
        const uint32_t this_logical_id = GetLogicalId();
        const uint32_t this_shared_id = (GetSharedId() == NO_VAL) ? this_branch_id : GetSharedId();

        new_branch_plugin->InitLogicalNodeIdentity(this_logical_id, this_shared_id, false);

        new_branch_plugin->InitNodeSemantics(
            BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_ROLE_FLAGS),
            static_cast<PackedCellBranchPlugin::APCNodeComputeKind>(BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_COMPUTE_KIND)),
            BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_AUX_PARAM_U32)
        );

        new_branch_plugin->SetSegmentRegionKind(desired_region_kind);

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

        AdaptivePackedCellContainer* tail_apc = FindSharedRootOrThis();
        AdaptivePackedCellContainer* prev_apc = nullptr;
        while (tail_apc)
        {
            prev_apc = tail_apc;
            tail_apc = tail_apc->GetNextSharedSegment();
        }

        if (!prev_apc)
        {
            new_shared_container->FreeAll();
            delete new_shared_container;
            clear_flags();
            return nullptr;
        }
        
        if (
            !prev_apc->GetBranchPlugin()->TryBindShareNext(new_shared_container->GetBranchId()) ||
            !new_branch_plugin->TryBindSharedPrevious(prev_apc->GetBranchId())
        )
        {
            new_shared_container->FreeAll();
            delete new_shared_container;
            clear_flags();
            return nullptr;
        }

        const uint32_t current_group_size = BranchPluginOfAPC_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        uint32_t next_group_size = ((current_group_size == NO_VAL) ? 1 : current_group_size) + 1; 

        BranchPluginOfAPC_->JustUpdateValueOfMeta32(
            PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE,
            current_group_size,
            next_group_size
        );
        const uint32_t new_group_size_expected = new_branch_plugin->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        new_branch_plugin->JustUpdateValueOfMeta32(
            PackedCellBranchPlugin::MetaIndexOfAPCNode::NODE_GROUP_SIZE,
            new_group_size_expected,
            next_group_size
        );
        
        RefreshAPCMeta_();
        new_shared_container->RefreshAPCMeta_();
        clear_flags();
        return new_shared_container;
    }


}
