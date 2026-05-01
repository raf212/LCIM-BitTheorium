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
        if (PayloadCapacityFromHeader() > 1)
        {
            step = static_cast<size_t>((mix_hash % (PayloadCapacityFromHeader() - 1)) + 1);
        }
        return step;
    }

    void AdaptivePackedCellContainer::InitZeroState_() noexcept
    {
        ForceZeroOccupancy_();
        MakeAPCBranchOwned();
        ResetTotalCASFailureForThisBranch();
        UpdateProducerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
        UpdateConsumerCursorPlacement(static_cast<uint32_t>(PayloadBegin()));
    }

    void AdaptivePackedCellContainer::RefreshAPCMeta_() noexcept
    {
        if (!IfAPCBranchValid())
        {
            return;
        }
        if (ShouldSplitNow())
        {
            TurnOnASegmentFlag(SegmentIODefinition::ControlEnumOfAPCSegment::SATURATED);
        }
        else
        {
            ClearOneControlEnumFlagOfAPC(SegmentIODefinition::ControlEnumOfAPCSegment::SATURATED);
        }
        if (OwnedMasterClockConfPtr_)
        {
            TouchLocalMetaClock48();
        }

        uint32_t current_group_size = ReadMetaCellValue32(MetaIndexOfAPCNode::NODE_GROUP_SIZE);
        if (current_group_size == NO_VAL)
        {
            JustUpdateValueOfMeta32(
                MetaIndexOfAPCNode::NODE_GROUP_SIZE,
                current_group_size,
                1u
            );
        }
        
    }

    size_t AdaptivePackedCellContainer::SuggestedChildCapacity_() noexcept
    {
        const size_t payload_capacity = PayloadCapacityFromHeader();
        const size_t child_payload_size = std::max<size_t>(MINIMUM_BRANCH_CAPACITY, payload_capacity / 2);
        return child_payload_size + PayloadBegin();
    }

    uint32_t AdaptivePackedCellContainer::ProducerORConsumerCursorSetAndGet_(std::optional<uint32_t> cursor_placement, int32_t increment_or_decrement_of_cursor, 
        bool* did_changed_easy_return, const MetaIndexOfAPCNode cursors_meta_idx
    ) noexcept
    {
        if (PayloadCapacityFromHeader() <= PayloadBegin())
        {
            if (did_changed_easy_return)
            {
                *did_changed_easy_return = false;
            }
            return SegmentIODefinition::BRANCH_SENTINAL;
        }
        
        while (true)
        {
            const uint32_t current_cursor_placement = ReadMetaCellValue32(cursors_meta_idx);
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
                const size_t payload_end = GetTotalCapacityForThisAPC();
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
            else if (desired_cursor_place >= GetTotalCapacityForThisAPC())
            {
                desired_cursor_place = static_cast<uint32_t>(GetTotalCapacityForThisAPC() - 1u);
            }

            if (desired_cursor_place == current_cursor_placement)
            {
                if (did_changed_easy_return)
                {
                    *did_changed_easy_return = false;
                }
                return current_cursor_placement;
            }
            if (JustUpdateValueOfMeta32(cursors_meta_idx, current_cursor_placement, desired_cursor_place))
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
        const auto maybe_current_region_bounds = ReadLayoutBounds(region_kind);
        if (!maybe_current_region_bounds.has_value() || maybe_current_region_bounds->GetPayloadSpan() == 0)
        {
            return std::nullopt;
        }

        LayoutBoundsOfSingleRelNodeClass current_region_bounds = *maybe_current_region_bounds;
        const size_t region_capacity = current_region_bounds.GetPayloadSpan();

        if (scan_cursor < current_region_bounds.BeginIndex || scan_cursor >= current_region_bounds.EndIndex)
        {
            scan_cursor = current_region_bounds.BeginIndex;
        }

        for (size_t prob = 0; prob < region_capacity; prob++)
        {
            const size_t idx = current_region_bounds.BeginIndex + ((scan_cursor - current_region_bounds.BeginIndex + prob) % region_capacity);
            packed64_t current_cell = BackingPtr[idx].load(MoLoad_);
            if (!current_region_bounds.CanCellBEConsumedForThisPhysicalRegion(current_cell, region_kind, idx))
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
                TotalCASFailForThisBranchIncreaseAndGet(1);
                continue;
            }
            const PackedMode old_mode = PackedCell64_t::ExtractModeOfPackedCellFromPacked(current_cell);
            const PackedCellDataType old_dtype = PackedCell64_t::ExtractPCellDataTypeFromPacked(current_cell);
            const APCPagedNodeRelMaskClasses old_page_class = PackedCell64_t::ExtractRelMaskFromPacked(current_cell);

            BackingPtr[idx].store(PackedCell64_t::MakeInitialPacked(old_mode, PriorityPhysics::HANDLE_NOW, PackedCellLocalityTypes::ST_IDLE, old_page_class, old_dtype), MoStoreSeq_);
            BackingPtr[idx].notify_all();
            // RebuildExectReadyMask();
            ReconcileOccupancySnapshotFromPayload();
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
        const uint32_t ready_bit = APCAndPagedNodeHelpers::MakeOneAPCNodeClassReadyBit(rel_mask);
        if (ready_bit == 0)
        {
            return;
        }
        while (true)
        {
            const val32_t current_branch_rel_mask = ReadMetaCellValue32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT);
            const uint32_t next_mask = current_branch_rel_mask | ready_bit;
            if (next_mask == current_branch_rel_mask)
            {
                return;
            }
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT, current_branch_rel_mask, next_mask))
            {
                return;
            }
        }
    }


    PublishResult AdaptivePackedCellContainer::TryPublishToRegionLocal_(APCPagedNodeRelMaskClasses region_kind, packed64_t packed_cell, bool force_rel_mask, uint16_t max_tries) noexcept
    {
        PublishResult failed_result{PublishStatus::INVALID, SIZE_MAX};
        if (!IfAPCBranchValid())
        {
            return failed_result;
        }
        if (force_rel_mask)
        {
            packed_cell = PackedCell64_t::SetPageClassInPacked(packed_cell, region_kind);
        }
        
        const auto maybe_current_region_bounds = ReadLayoutBounds(region_kind);
        if (!maybe_current_region_bounds|| maybe_current_region_bounds->IsEmpty())
        {
            return failed_result;
        }
        auto TryPublishOnceInBounds = [&](const LayoutBoundsOfSingleRelNodeClass& current_region_bounds)->PublishResult
        {
            const uint32_t begin_idx = current_region_bounds.BeginIndex;
            const uint32_t end_idx = current_region_bounds.EndIndex;
            const size_t span = (end_idx > begin_idx) ? (end_idx - begin_idx) : 0;

            const size_t next_producer_sequense = NextProducerSequence();
            if (next_producer_sequense == SIZE_MAX)
            {
                return failed_result;   
            }

            if (span == 0)
            {
                failed_result.ResultStatus = PublishStatus::FULL;
                return failed_result;
            }
            const size_t seed_idx = (next_producer_sequense > PayloadBegin()) ? (next_producer_sequense - PayloadBegin()) : 0;
            const size_t base = begin_idx + (seed_idx % span);
            const size_t step = MakeProbeStepCoPrime_(seed_idx + 1u, span);
            for (size_t tries = 0; tries < max_tries; tries++)
            {
                const size_t current_index = begin_idx + ((base - begin_idx + tries * step) % span);
                packed64_t current_cell = BackingPtr[current_index].load(MoLoad_);
                if (PackedCell64_t::ExtractLocalityFromPacked(current_cell) != PackedCellLocalityTypes::ST_IDLE)
                {
                    continue;
                }
                const packed64_t claimd_local_inplace_cell = PackedCell64_t::SetLocalityInPacked(current_cell, PackedCellLocalityTypes::ST_CLAIMED);
                packed64_t expected_cell = current_cell;
                if (!BackingPtr[current_index].compare_exchange_strong(expected_cell, claimd_local_inplace_cell, OnExchangeSuccess, OnExchangeFailure))
                {
                    if (AdaptiveBackoffOfAPCPtr_)
                    {
                        AdaptiveBackoffOfAPCPtr_->AdaptiveBackOffPacked(expected_cell);
                    }
                    TotalCASFailForThisBranchIncreaseAndGet(1);
                    continue;
                }
                BackingPtr[current_index].store(packed_cell, MoStoreSeq_);
                BackingPtr[current_index].notify_all();
                UpdateRegionRelMaskForIdx_(region_kind);
                ReconcileOccupancySnapshotFromPayload();
                TouchLocalMetaClock48();
                RefreshAPCMeta_();
                return {PublishStatus::OK, current_index};
            }
            failed_result.ResultStatus = PublishStatus::FULL;
            return failed_result;
        };

        PublishResult first = TryPublishOnceInBounds(*maybe_current_region_bounds);
        if (first.ResultStatus == PublishStatus::OK)
        {
            return first;
        }
        auto maybe_complete_layout = ReadAndGetFullRegionLayout_();
        if (!maybe_complete_layout.has_value())
        {
            failed_result.ResultStatus = PublishStatus::FULL;
            return failed_result;
        }
        CompleteAPCNodeRegionsLayout complete_layout = *maybe_complete_layout;
        const uint32_t suggested_expension = SuggestedInternalAPCExpension_(&complete_layout);
        if (suggested_expension != 0 && TryExtendASegmentInOwnAPC(
            region_kind,
            suggested_expension,
            ContainerConf::APCSegmentExtendOrder::PRIORITY
        ))
        {
            auto maybe_extended_bounds = ReadLayoutBounds(region_kind);
            if (maybe_extended_bounds)
            {
                PublishResult second = TryPublishOnceInBounds(*maybe_extended_bounds);
                if (second.ResultStatus == PublishStatus::OK)
                {
                    return second;
                }
            }
        }
        return failed_result;
    }

    uint32_t AdaptivePackedCellContainer::SuggestedInternalAPCExpension_(CompleteAPCNodeRegionsLayout* complete_layout, uint8_t prefared_percentage_of_free) noexcept
    {
        complete_layout->NormalizePercentagesIfNeeded();
        LayoutBoundsOfSingleRelNodeClass* free_layout = complete_layout->GetALayoutByRelMask(APCPagedNodeRelMaskClasses::FREE_SLOT);
        const uint32_t suggested_expension = static_cast<uint32_t>(((PayloadCapacityFromHeader() * (free_layout->InitialOrCurrentPercentage)) / 100) / (100 / prefared_percentage_of_free));
        return suggested_expension;

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
        const size_t region_size = static_cast<size_t>(ReadMetaCellValue32(MetaIndexOfAPCNode::REGION_SIZE));
        if (region_size == 0)
        {
            RegionRelArray_.reset();
            RegionEpochArray_.reset();
            RelBitmaps_.clear();
            return true;
        }

        const size_t number_of_regions = (PayloadCapacityFromHeader() + region_size - 1u) / region_size;
        RegionRelArray_.reset(new std::atomic<uint8_t>[number_of_regions]);
        RegionEpochArray_.reset(new std::atomic<uint64_t>[number_of_regions]);
        const size_t words = (number_of_regions + MAX_VAL - 1) / MAX_VAL;
        RelBitmaps_.assign(APCAndPagedNodeHelpers::SIZE_OF_APCPagedNodeRelMaskClasses, std::vector<uint64_t>(words, 0ull));
        uint32_t global_ready_mask = NO_VAL;
        for (size_t region = 0; region < number_of_regions; region++)
        {
            const size_t base = region * region_size;
            const size_t capacity_end = std::min(PayloadCapacityFromHeader(), base + region_size);
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
                region_ready_mask |= APCAndPagedNodeHelpers::MakeOneAPCNodeClassReadyBit(absolute_cell_relation_mask);
                region_epoch = std::max<uint64_t>(region_epoch, PackedCell64_t::ExtractClk16(absolute_packed_cell));
                RegionRelArray_[region].store(static_cast<uint8_t>(region_ready_mask & APCAndPagedNodeHelpers::HIGH_ALL_EIGHT_NIBBLE), MoStoreSeq_);
                RegionEpochArray_[region].store(region_epoch, MoStoreSeq_);
                global_ready_mask |= region_ready_mask;
                if (region_ready_mask != NO_VAL)
                {
                    const size_t word = region / MAX_VAL;
                    const size_t bit = region % MAX_VAL;
                    const uint64_t region_mask = (1ull << bit);
                    for (unsigned rel_class = 0; rel_class < APCAndPagedNodeHelpers::SIZE_OF_APCPagedNodeRelMaskClasses; rel_class++)
                    {
                        if (region_ready_mask & (1u << rel_class))
                        {
                            RelBitmaps_[rel_class][word] |= region_mask;
                        }
                    }
                }
            }
        }
        const uint32_t expected_mask = ReadMetaCellValue32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT);

        JustUpdateValueOfMeta32(
            MetaIndexOfAPCNode::PAGED_NODE_READY_BIT,
            expected_mask,
            global_ready_mask
        );
        return true;
    }




    bool AdaptivePackedCellContainer::ApplyOccupancyTransition_(
        PackedCellLocalityTypes from,
        PackedCellLocalityTypes to,
        APCPagedNodeRelMaskClasses desired_region_class
    ) noexcept
    {
        if (desired_region_class == APCPagedNodeRelMaskClasses::NONE || desired_region_class == APCPagedNodeRelMaskClasses::NANNULL)
        {
            return false;
        }
        if (from == PackedCellLocalityTypes::ST_IDLE && to == PackedCellLocalityTypes::ST_CLAIMED)
        {
            AllClaimedCellsOccupancySnapshotAddOrSubAndGetAfterChange(+1);
            return true;
        }
        if (from == PackedCellLocalityTypes::ST_CLAIMED && to == PackedCellLocalityTypes::ST_PUBLISHED)
        {
            AllPublishedCellsOccupancySnapshotAddOrSubAndGetAfterChange(+1);
            AllClaimedCellsOccupancySnapshotAddOrSubAndGetAfterChange(-1);
            const uint32_t region_occ_after_update = RegionOccupancyAddOrSubAndGet(desired_region_class, +1);
            TurnOnReadyBitForDesiredPagedNode_(desired_region_class);
            return region_occ_after_update > NO_VAL;
        }
        if (from == PackedCellLocalityTypes::ST_PUBLISHED && to == PackedCellLocalityTypes::ST_CLAIMED)
        {
            AllPublishedCellsOccupancySnapshotAddOrSubAndGetAfterChange(-1);
            AllClaimedCellsOccupancySnapshotAddOrSubAndGetAfterChange(+1);
            RegionOccupancyAddOrSubAndGet(desired_region_class, -1);
            return ClearTheDesiredPagedNodeReadyBit_(desired_region_class);
        }
        if (from == PackedCellLocalityTypes::ST_CLAIMED && to == PackedCellLocalityTypes::ST_IDLE)
        {
            AllClaimedCellsOccupancySnapshotAddOrSubAndGetAfterChange(-1);
            return true;
        }

        //continue from here after fixing rel.h
        
        
        
        return false;
    }


}