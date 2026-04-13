#include "AdaptivePackedCellContainer.hpp"
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

    size_t AdaptivePackedCellContainer::SuggestedChildCapacity_() const noexcept
    {
        const size_t payload_capacity = GetPayloadCapacity();
        const size_t child_payload_size = std::max<size_t>(MINIMUM_BRANCH_CAPACITY, payload_capacity / 2);
        return child_payload_size + PayloadBegin();
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
                const size_t payload_end = GetPayloadEnd();
                const size_t payload_capacity = (payload_end > PayloadBegin()) ? (payload_end - PayloadBegin()) : NO_VAL;
                if (payload_capacity == 0)
                {
                    if (did_changed_easy_return)
                    {
                        *did_changed_easy_return = false;
                    }
                    return PackedCellBranchPlugin::BRANCH_SENTINAL;
                }
                
                size_t current_placement = static_cast<size_t>(PayloadBegin());
                if (current_cursor_placement >= PayloadBegin() || current_cursor_placement < payload_end)
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

    std::optional<APCPagedNodeRegionBounds> AdaptivePackedCellContainer::ReadRegionBounds_(APCPagedNodeRelMaskClasses region_kind) noexcept
    {
        if (!IfAPCBranchValid())
        {
            return std::nullopt;
        }

        std::optional<PackedCellBranchPlugin::LayoutBoundsUint32> bounds;

        switch (region_kind)
        {
            case APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::MESSAGE_FEEDFORWARD_BEGAIN);
                break;

            case APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::MESSAGE_FEEDBACKWARD_BEGAIN);
                break;

            case APCPagedNodeRelMaskClasses::STATE_SLOT:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::STATE_BEGAINING);
                break;

            case APCPagedNodeRelMaskClasses::ERROR_SLOT:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::ERROR_BEGAIN);
                break;

            case APCPagedNodeRelMaskClasses::EDGE_DESCRIPTOR:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::EDGE_DESCRIPTIOR_BEGAIN);
                break;

            case APCPagedNodeRelMaskClasses::WEIGHT_SLOT:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::WEIGHT_BEGIN);
                break;

            case APCPagedNodeRelMaskClasses::AUX_SLOT:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::AUX_BEGAIN);
                break;

            case APCPagedNodeRelMaskClasses::FREE_SLOT:
                bounds = BranchPluginOfAPC_->ReadLayoutBounds(
                    PackedCellBranchPlugin::MetaIndexOfAPCNode::FREE_BEGAIN);
                break;

            default:
                return std::nullopt;
        }

        if (!bounds.has_value())
        {
            return std::nullopt;
        }

        APCPagedNodeRegionBounds out_bounds;
        out_bounds.BeginIdx = static_cast<size_t>(bounds->BeginIndex);
        out_bounds.EndIdx = static_cast<size_t>(bounds->EndIndex);

        if (!out_bounds.IsValidRegion() || out_bounds.BeginIdx < PayloadBegin() || out_bounds.EndIdx > GetPayloadEnd())
        {
            return std::nullopt;
        }
        return out_bounds;
    }
}