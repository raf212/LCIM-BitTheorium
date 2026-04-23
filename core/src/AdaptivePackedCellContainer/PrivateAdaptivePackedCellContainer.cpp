#include "APCSegmentsCausalCordinator.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
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

    void AdaptivePackedCellContainer::InitZeroState_() noexcept
    {
        if (!SegmentIODefinitionPtr_)
        {
            return;
        }
        SegmentIODefinitionPtr_->ForceOccupancyUpdateAndReturn(0);
        SegmentIODefinitionPtr_->MakeAPCBranchOwned();
        SegmentIODefinitionPtr_->ResetTotalCASFailureForThisBranch();
        UpdateProducerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
        UpdateConsumerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
    }

    void AdaptivePackedCellContainer::FreeAll() noexcept
    {
        if (!BackingPtr && !IfAPCBranchValid() && !OwnedMasterClockConfPtr_)
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
        OwnedMasterClockConfPtr_.reset();   
        if (BackingPtr)
        {
            if (SegmentIODefinitionPtr_ && SegmentIODefinitionPtr_->IsBranchOwnedByFlag())
            {
                SegmentIODefinitionPtr_->ReleseOwneshipFlag();
                delete[] BackingPtr;
            }
            BackingPtr = nullptr;
        }

        RegionRelArray_.reset();
        RegionEpochArray_.reset();
        RelBitmaps_.clear();
        if (SegmentIODefinitionPtr_)
        {
            SegmentIODefinitionPtr_.reset();
        }        
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
        if (SegmentIODefinitionPtr_->ShouldSplitNow())
        {
            SegmentIODefinitionPtr_->TurnOnASegmentFlag(SegmentIODefinition::ControlEnumOfAPCSegment::SATURATED);
        }
        else
        {
            SegmentIODefinitionPtr_->ClearOneControlEnumFlagOfAPC(SegmentIODefinition::ControlEnumOfAPCSegment::SATURATED);
        }
        if (OwnedMasterClockConfPtr_)
        {
            SegmentIODefinitionPtr_->TouchLocalMetaClock48();
        }

        uint32_t current_group_size = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        if (current_group_size == NO_VAL)
        {
            SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(
                MetaIndexOfAPCNode::NODE_GROUP_SIZE,
                current_group_size,
                1u
            );
        }
        
    }

    size_t AdaptivePackedCellContainer::SuggestedChildCapacity_() const noexcept
    {
        const size_t payload_capacity = GetPayloadCapacity();
        const size_t child_payload_size = std::max<size_t>(MINIMUM_BRANCH_CAPACITY, payload_capacity / 2);
        return child_payload_size + PayloadBegin();
    }

    uint32_t AdaptivePackedCellContainer::ProducerORConsumerCursorSetAndGet_(std::optional<uint32_t> cursor_placement, int32_t increment_or_decrement_of_cursor, 
        bool* did_changed_easy_return, const MetaIndexOfAPCNode cursors_meta_idx
    ) noexcept
    {
        if (!SegmentIODefinitionPtr_)
        {
            if (did_changed_easy_return)
            {
                *did_changed_easy_return = false;
            }
            return SegmentIODefinition::BRANCH_SENTINAL;
        }
        if (GetPayloadEnd() <= PayloadBegin())
        {
            if (did_changed_easy_return)
            {
                *did_changed_easy_return = false;
            }
            return SegmentIODefinition::BRANCH_SENTINAL;
        }
        
        while (true)
        {
            const uint32_t current_cursor_placement = SegmentIODefinitionPtr_->ReadMetaCellValue32(cursors_meta_idx);
            uint32_t desired_cursor_place = current_cursor_placement;
            if (cursor_placement.has_value())
            {
                desired_cursor_place = cursor_placement.value();
            }

            else if (increment_or_decrement_of_cursor != 0)
            {
                if (current_cursor_placement == SegmentIODefinition::BRANCH_SENTINAL)
                {
                    if (did_changed_easy_return)
                    {
                        *did_changed_easy_return = false;
                    }
                    return current_cursor_placement;
                }
                const size_t payload_end = GetPayloadEnd();
                const size_t payload_capacity = (payload_end > PayloadBegin()) ? (payload_end - PayloadBegin()) : NO_VAL;
                if (payload_capacity == 0)
                {
                    if (did_changed_easy_return)
                    {
                        *did_changed_easy_return = false;
                    }
                    return SegmentIODefinition::BRANCH_SENTINAL;
                }
                
                size_t current_placement = static_cast<size_t>(PayloadBegin());
                if (current_cursor_placement >= PayloadBegin() && current_cursor_placement < payload_end)
                {
                    current_placement = static_cast<size_t>(current_cursor_placement);
                }

                const int64_t current_offset = static_cast<int64_t>(current_placement - PayloadBegin());
                int64_t next_offset = current_offset + static_cast<int64_t>(increment_or_decrement_of_cursor);

                const int64_t modulo = static_cast<int64_t>(payload_capacity);
                next_offset  = next_offset % modulo;
                if (next_offset < 0)
                {
                    next_offset = next_offset + modulo;
                }
                desired_cursor_place = static_cast<uint32_t>(PayloadBegin() + static_cast<uint32_t>(next_offset));
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
            if (SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(cursors_meta_idx, current_cursor_placement, desired_cursor_place))
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = true;
                }
                return desired_cursor_place;
            }
        }
    }


    std::optional<packed64_t> AdaptivePackedCellContainer::TryConsumeAndIdleFromRegionLocal_(APCPagedNodeRelMaskClasses region_kind, size_t& scan_cursor) noexcept
    {
        if (!IfAPCBranchValid())
        {
            return std::nullopt;
        }
        const auto maybe_current_region_bounds = SegmentIODefinitionPtr_->ReadLayoutBounds(region_kind);
        if (!maybe_current_region_bounds.has_value() || maybe_current_region_bounds->GetPayloadSpan() == 0)
        {
            return std::nullopt;
        }

        const LayoutBoundsOfSingleRelNodeClass current_region_bounds = *maybe_current_region_bounds;
        const size_t region_capacity = current_region_bounds.GetPayloadSpan();

        if (scan_cursor < current_region_bounds.BeginIndex || scan_cursor >= current_region_bounds.EndIndex)
        {
            scan_cursor = current_region_bounds.BeginIndex;
        }

        for (size_t prob = 0; prob < region_capacity; prob++)
        {
            const size_t idx = current_region_bounds.BeginIndex + ((scan_cursor - current_region_bounds.BeginIndex + prob) % region_capacity);
            packed64_t current_cell = BackingPtr[idx].load(MoLoad_);
            if (!APCAndPagedNodeHelpers::CanCellBeConsumedForThisRegion(current_cell, region_kind))
            {
                continue;
            }

            const packed64_t current_cell_claimed_local = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_CLAIMED);
            packed64_t expected_cell = current_cell;
            if (!BackingPtr[idx].compare_exchange_strong(expected_cell, current_cell_claimed_local, OnExchangeSuccess, OnExchangeFailure))
            {
                if (APCManagerPtr_)
                {
                    APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(expected_cell);
                }
                SegmentIODefinitionPtr_->TotalCASFailForThisBranchIncreaseAndGet(1);
                continue;
            }
            const PackedMode old_mode = PackedCell64_t::ExtractModeOfPackedCellFromPacked(current_cell);
            const PackedCellDataType old_dtype = PackedCell64_t::ExtractPCellDataTypeFromPacked(current_cell);

            BackingPtr[idx].store(PackedCell64_t::MakeInitialPacked(old_mode, old_dtype, static_cast<tag8_t>(region_kind)));
            BackingPtr[idx].notify_all();
            OccupancyAddOrSubAndGetAfterChange(-1);
            RefreshAPCMeta_();
            scan_cursor = idx + 1;
            if (scan_cursor >= current_region_bounds.EndIndex)
            {
                scan_cursor = current_region_bounds.BeginIndex;
            }
            return current_cell;
        }
        return std::nullopt;
    }

    void AdaptivePackedCellContainer::UpdateRegionRelMaskForIdx_(APCPagedNodeRelMaskClasses rel_mask) noexcept
    {
        if (!IfAPCBranchValid())
        {
            return;
        }
        const uint32_t ready_bit = APCAndPagedNodeHelpers::ReadyBitForRelClass(rel_mask);
        if (ready_bit == 0)
        {
            return;
        }
        while (true)
        {
            const val32_t current_branch_rel_mask = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::READY_REL_MASK);
            const uint32_t next_mask = current_branch_rel_mask | ready_bit;
            if (next_mask == current_branch_rel_mask)
            {
                return;
            }
            if (SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(MetaIndexOfAPCNode::READY_REL_MASK, current_branch_rel_mask, next_mask))
            {
                return;
            }
        }
    }


    PublishResult AdaptivePackedCellContainer::TryPublishToRegionLocal_(APCPagedNodeRelMaskClasses region_kind, packed64_t packed_cell, bool force_rel_mask, uint16_t max_tries) noexcept
    {
        PublishResult result{PublishStatus::INVALID, SIZE_MAX};
        if (!IfAPCBranchValid())
        {
            return result;
        }

        const auto maybe_current_region_bounds = SegmentIODefinitionPtr_->ReadLayoutBounds(region_kind);
        if (!maybe_current_region_bounds.has_value() || maybe_current_region_bounds->GetPayloadSpan() == 0)
        {
            return result;
        }
        const LayoutBoundsOfSingleRelNodeClass current_region_bounds = * maybe_current_region_bounds;
        const size_t region_capacity = current_region_bounds.GetPayloadSpan();
        if (force_rel_mask)
        {
            packed_cell = APCAndPagedNodeHelpers::SetRelMaskForPagedNode(packed_cell, region_kind);
        }
        
        uint16_t tries = 0;
        while (tries++ < max_tries)
        {
            const size_t next_sequense = NextProducerSequence();
            if (next_sequense == SIZE_MAX)
            {
                result.ResultStatus = PublishStatus::FULL;
                return result;
            }

            size_t idx = current_region_bounds.BeginIndex + ((next_sequense - PayloadBegin()) % region_capacity);
            const size_t step = MakeProbeStepCoPrime_(next_sequense * ID_HASH_GOLDEN_CONST, region_capacity);
            for (size_t prob = 0; prob < region_capacity; prob++)
            {
                packed64_t current_cell = BackingPtr[idx].load(MoLoad_);
                if (PackedCell64_t::ExtractLocalityFromPacked(current_cell) == PackedCellLocalityTypes::ST_IDLE)
                {
                    const packed64_t current_cell_claimed = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_CLAIMED);
                    packed64_t expected_cell = current_cell;
                    if (BackingPtr[idx].compare_exchange_strong(expected_cell, current_cell_claimed, OnExchangeSuccess, OnExchangeFailure))
                    {
                        BackingPtr[idx].store(PackedCell64_t::SetLocalityInPacked(packed_cell, PackedCellLocalityTypes::ST_PUBLISHED));
                        BackingPtr[idx].notify_all();
                        OccupancyAddOrSubAndGetAfterChange(+1);
                        UpdateRegionRelMaskForIdx_(region_kind);
                        SegmentIODefinitionPtr_->TouchLocalMetaClock48();
                        RefreshAPCMeta_();
                        result.ResultStatus = PublishStatus::OK;
                        result.Index = idx;
                        return result;
                    }
                    SegmentIODefinitionPtr_->TotalCASFailForThisBranchIncreaseAndGet(1);
                }
                idx = current_region_bounds.BeginIndex + ((idx - current_region_bounds.BeginIndex + step) % region_capacity);
            }
            const size_t observed_idx = current_region_bounds.BeginIndex  + ((next_sequense - PayloadBegin()) % region_capacity);
            if (APCManagerPtr_)
            {
                APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(BackingPtr[observed_idx].load(MoLoad_));
            }
        }
        result.ResultStatus = PublishStatus::FULL;
        return result;
    }

    size_t AdaptivePackedCellContainer::FindGreatestCommonDivisor_(size_t a, size_t b) noexcept
    {
        while (b != 0)
        {
            const size_t modulo = a % b;
            a = b;
            b = modulo;
        }
        return a;
    }

    size_t AdaptivePackedCellContainer::MakeProbeStepCoPrime_(size_t seed, size_t region_capacity) const noexcept
    {
        if (region_capacity <= 1)
        {
            return 1;
        }
        size_t step = 1u + (seed % (region_capacity - 1u));
        while (FindGreatestCommonDivisor_(step, region_capacity) != 1u)
        {
            ++step;
            if (step >= region_capacity)
            {
                step = 1u;
            }
        }
        return step;
    }

    bool AdaptivePackedCellContainer::RebuildRegionIndexFromPayload_() noexcept
    {
        if (!IfAPCBranchValid())
        {
            return false;
        }
        const size_t region_size = static_cast<size_t>(SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_SIZE));
        if (region_size == 0)
        {
            RegionRelArray_.reset();
            RegionEpochArray_.reset();
            RelBitmaps_.clear();
            return true;
        }

        const size_t number_of_regions = (GetPayloadCapacity() + region_size - 1u) / region_size;
        RegionRelArray_.reset(new std::atomic<uint8_t>[number_of_regions]);
        RegionEpochArray_.reset(new std::atomic<uint64_t>[number_of_regions]);
        const size_t words = (number_of_regions + MAX_VAL - 1) / MAX_VAL;
        RelBitmaps_.assign(APCAndPagedNodeHelpers::SIZE_OF_APCPagedNodeRelMaskClasses, std::vector<uint64_t>(words, 0ull));
        uint32_t global_ready_mask = NO_VAL;
        for (size_t region = 0; region < number_of_regions; region++)
        {
            const size_t base = region * region_size;
            const size_t capacity_end = std::min(GetPayloadCapacity(), base + region_size);
            uint32_t region_ready_mask = NO_VAL;
            uint64_t region_epoch = NO_VAL;
            for (size_t i = base; i < capacity_end; i++)
            {
                const size_t absolute_idx = PayloadBegin() + i;
                const packed64_t absolute_packed_cell = BackingPtr[absolute_idx].load(MoLoad_);
                if (PackedCell64_t::ExtractLocalityFromPacked(absolute_packed_cell) != PackedCellLocalityTypes::ST_PUBLISHED)
                {
                    continue;
                }
                const APCPagedNodeRelMaskClasses absolute_cell_relation_mask = APCAndPagedNodeHelpers::ExtractPagedRelMaskFromPacked(absolute_packed_cell);
                region_ready_mask |= APCAndPagedNodeHelpers::ReadyBitForRelClass(absolute_cell_relation_mask);
                region_epoch = std::max<uint64_t>(region_epoch, PackedCell64_t::ExtractClk16(absolute_packed_cell));
                RegionRelArray_[region].store(static_cast<uint8_t>(region_ready_mask & APCAndPagedNodeHelpers::HIGH_ALL_EIGHT_NIBBLE), MoStoreSeq_);
                RegionEpochArray_[region].store(region_epoch, MoStoreSeq_);
                global_ready_mask |= region_ready_mask;
                if (region_ready_mask != NO_VAL)
                {
                    const size_t word = region / MAX_VAL;
                    const size_t bit = region % MAX_VAL;
                    const uint64_t region_mask = (1ull << bit);
                    for (unsigned rel_class = 0; rel_class < APCAndPagedNodeHelpers::SIZE_OF_APCPagedNodeRelMaskClasses; i++)
                    {
                        if (region_ready_mask & (1u << rel_class))
                        {
                            RelBitmaps_[rel_class][word] |= region_mask;
                        }
                    }
                }
            }
        }
        const uint32_t expected_mask = SegmentIODefinitionPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::READY_REL_MASK);

        SegmentIODefinitionPtr_->JustUpdateValueOfMeta32(
            MetaIndexOfAPCNode::READY_REL_MASK,
            expected_mask,
            global_ready_mask
        );
        return true;
    }






}