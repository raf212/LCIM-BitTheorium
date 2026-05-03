#include "APCSegmentsCausalCordinator.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{

    void SegmentIODefinition::InitDefaultAPCSegmentedNodeLayout_() noexcept
    {
        const uint32_t payload_begain = METACELL_COUNT;
        const uint32_t payload_end = GetTotalCapacityForThisAPC();
        if (payload_end <= payload_begain)
        {
            return;
        }
        CompleteAPCNodeRegionsLayout full_paged_node_layout{};
        BuidDefaultLayoutPlan_(full_paged_node_layout);
        WriteBoundsPairToHeader_(full_paged_node_layout.FeedForwardLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.FeedBackwardLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.StateLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.ErrorLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.EdgeDescriptorLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.WeightLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.AUXLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.FreeLayout);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::REGION_DIR_COUNT, TOTAL_LAYOUT_SECTION_IN_APC_CONTAINER_NODE);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::EDGE_TABLE_COUNT, NO_VAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::WEIGHT_TABLE_COUNT, NO_VAL);

        TurnOnMultipleSegmentFlagsAtOnce_(static_cast<uint32_t>(ControlEnumOfAPCSegment::HAS_LAYOUT_DIR));

        
    }

    void SegmentIODefinition::BuidDefaultLayoutPlan_(CompleteAPCNodeRegionsLayout& full_layout) noexcept
    {
        const uint32_t payload_begain = METACELL_COUNT;
        const uint32_t payload_end = GetTotalCapacityForThisAPC();
        if (payload_end <= payload_begain)
        {
            return;
        }
        full_layout.NormalizePercentagesIfNeeded();

        const uint32_t total_span = payload_end - payload_begain;
        uint32_t initial_cursor = payload_begain;
        
        auto AssignOne = [&](LayoutBoundsOfSingleRelNodeClass& one, bool keep_tail = false) noexcept
        {
            if (one.LAYOUT_CLASS == APCPagedNodeRelMaskClasses::NANNULL)
            {
                one.BeginIndex = initial_cursor;
                one.EndIndex = initial_cursor;
                return;
            }
            one.BeginIndex = initial_cursor;
            uint32_t wanted_span = one.ComputeWantedSpanFromTotal(total_span);
            if (one.LAYOUT_CLASS != APCPagedNodeRelMaskClasses::FREE_SLOT)
            {
                wanted_span = std::max<uint32_t>(wanted_span, 2u);
            }
            if (keep_tail)
            {
                one.EndIndex = payload_end;
                initial_cursor = payload_end;
                return;
            }
            const uint32_t remaining_span = (payload_end > initial_cursor) ? (payload_end - initial_cursor) : NO_VAL;
            wanted_span = std::min<uint32_t>(wanted_span, remaining_span);
            one.EndIndex = initial_cursor + wanted_span;
            initial_cursor = one.EndIndex;
        };
        AssignOne(full_layout.FeedForwardLayout);
        AssignOne(full_layout.FeedBackwardLayout);
        AssignOne(full_layout.StateLayout);
        AssignOne(full_layout.ErrorLayout);
        AssignOne(full_layout.EdgeDescriptorLayout);
        AssignOne(full_layout.WeightLayout);
        AssignOne(full_layout.AUXLayout);

        full_layout.FreeLayout.BeginIndex = initial_cursor;
        full_layout.FreeLayout.EndIndex = payload_end;
    }

    bool SegmentIODefinition::WriteBoundsPairToHeader_(const LayoutBoundsOfSingleRelNodeClass layout_bound) noexcept
    {
        auto maybe_region_bounds_pair = GetMetaBoundsLegalPairForPageClasses(layout_bound.LAYOUT_CLASS);
        if (!maybe_region_bounds_pair || layout_bound.IsEmpty() == true)
        {
            return false;
        }
        const auto [begin_meta, end_meta] = *maybe_region_bounds_pair;
        const uint32_t current_begin = ReadMetaCellValue32(begin_meta);
        const uint32_t current_end = ReadMetaCellValue32(end_meta);
        return JustUpdateValueOfMeta32(begin_meta, current_begin, layout_bound.BeginIndex) &&
                JustUpdateValueOfMeta32(end_meta, current_end, layout_bound.EndIndex);
    }

    std::optional<std::pair<MetaIndexOfAPCNode, MetaIndexOfAPCNode>>SegmentIODefinition::GetMetaBoundsLegalPairForPageClasses(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept
    {
        MetaIndexOfAPCNode begin_idx;
        MetaIndexOfAPCNode end_idx;
        switch (desired_rel_mask)
        {
            // ---- Feedforward message ----
            case APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE:
            {
                begin_idx = MetaIndexOfAPCNode::MESSAGE_FEEDFORWARD_BEGAIN;
                end_idx   = MetaIndexOfAPCNode::MESSAGE_FEEDFORWARD_END;
                break;
            }

            // ---- Feedback message ----
            case APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE:
            {
                begin_idx = MetaIndexOfAPCNode::MESSAGE_FEEDBACKWARD_BEGAIN;
                end_idx   = MetaIndexOfAPCNode::MESSAGE_FEEDBACKWARD_END;
                break;
            }

            // ---- State ----
            case APCPagedNodeRelMaskClasses::STATE_SLOT:
            {
                begin_idx = MetaIndexOfAPCNode::STATE_BEGAINING;
                end_idx   = MetaIndexOfAPCNode::STATE_END;
                break;
            }

            //-- ERROR--
            case APCPagedNodeRelMaskClasses::ERROR_SLOT:
            {
                begin_idx = MetaIndexOfAPCNode::ERROR_BEGAIN;
                end_idx   = MetaIndexOfAPCNode::ERROR_END;
                break;
            }

            // ---- Edge descriptor ----
            case APCPagedNodeRelMaskClasses::EDGE_DESCRIPTOR:
            {
                begin_idx = MetaIndexOfAPCNode::EDGE_DESCRIPTIOR_BEGAIN;
                end_idx   = MetaIndexOfAPCNode::EDGE_DESCRIPTIOR_END;
                break;
            }

            // ---- Weight ----
            case APCPagedNodeRelMaskClasses::WEIGHT_SLOT:
            {
                begin_idx = MetaIndexOfAPCNode::WEIGHT_BEGIN;
                end_idx   = MetaIndexOfAPCNode::WEIGHT_END;
                break;
            }

            // ---- Aux ----
            case APCPagedNodeRelMaskClasses::AUX_SLOT:
            {
                begin_idx = MetaIndexOfAPCNode::AUX_BEGAIN;
                end_idx   = MetaIndexOfAPCNode::AUX_END;
                break;
            }

            // ---- Free ----
            case APCPagedNodeRelMaskClasses::FREE_SLOT:
            {
                begin_idx = MetaIndexOfAPCNode::FREE_BEGAIN;
                end_idx   = MetaIndexOfAPCNode::FREE_END;
                break;
            }

            default:
            {
                return std::nullopt;
            }
        }

        return std::pair {begin_idx, end_idx};
    }

    bool SegmentIODefinition::UpdateAPCModeFlagsInHeader_(uint32_t flags_to_turn_on, uint32_t flags_to_turn_off, MetaIndexOfAPCNode desired_flag_idx) noexcept
    {
        if (desired_flag_idx != MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS && desired_flag_idx != MetaIndexOfAPCNode::MANAGER_CONTROL_FLAGS)
        {
            return false;
        }
        
        while (true)
        {
            const uint32_t current_flags = ReadMetaCellValue32(desired_flag_idx);
            if (current_flags == BRANCH_SENTINAL)
            {
                return false;
            }
            
            uint32_t next_flags = current_flags;
            next_flags |= flags_to_turn_on;
            next_flags &= ~flags_to_turn_off;
            if (next_flags == current_flags)
            {
                return true;
            }
            if (JustUpdateValueOfMeta32(desired_flag_idx, current_flags, next_flags))
            {
                return true;
            }
        }
    }

    std::optional<CompleteAPCNodeRegionsLayout> SegmentIODefinition::ReadAndGetFullRegionLayout_() noexcept
    {
        auto LoadOne = [&](APCPagedNodeRelMaskClasses desired_rel_mask, LayoutBoundsOfSingleRelNodeClass& out_one) noexcept->bool
        {
            auto maybe_one = ReadLayoutBounds(desired_rel_mask);
            if (!maybe_one)
            {
                return false;
            }
            out_one = *maybe_one;
            return true;
        };

        CompleteAPCNodeRegionsLayout out_layout{};
        LoadOne(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, out_layout.FeedForwardLayout);
        LoadOne(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE, out_layout.FeedBackwardLayout);
        LoadOne(APCPagedNodeRelMaskClasses::STATE_SLOT, out_layout.StateLayout);
        LoadOne(APCPagedNodeRelMaskClasses::ERROR_SLOT, out_layout.ErrorLayout);
        LoadOne(APCPagedNodeRelMaskClasses::EDGE_DESCRIPTOR, out_layout.EdgeDescriptorLayout);
        LoadOne(APCPagedNodeRelMaskClasses::WEIGHT_SLOT, out_layout.WeightLayout);
        LoadOne(APCPagedNodeRelMaskClasses::AUX_SLOT, out_layout.AUXLayout);
        LoadOne(APCPagedNodeRelMaskClasses::FREE_SLOT, out_layout.FreeLayout);
        return out_layout;
    }

    bool SegmentIODefinition::WriteAllRegionsLayoutToHeader_(const CompleteAPCNodeRegionsLayout& full_layout) noexcept
    {
        return 
            WriteBoundsPairToHeader_(full_layout.FeedForwardLayout) &&
            WriteBoundsPairToHeader_(full_layout.FeedBackwardLayout) &&
            WriteBoundsPairToHeader_(full_layout.StateLayout) &&
            WriteBoundsPairToHeader_(full_layout.ErrorLayout) && 
            WriteBoundsPairToHeader_(full_layout.EdgeDescriptorLayout) &&
            WriteBoundsPairToHeader_(full_layout.WeightLayout) && 
            WriteBoundsPairToHeader_(full_layout.AUXLayout) &&
            WriteBoundsPairToHeader_(full_layout.FreeLayout);
    }

    bool SegmentIODefinition::TurnOnReadyBitForDesiredPagedNode_(APCPagedNodeRelMaskClasses desired_region_class) noexcept
    {
        const uint32_t anew_readybit = APCAndPagedNodeHelpers::MakeOneAPCNodeClassReadyBit(desired_region_class);
        if (anew_readybit == 0)
        {
            return false;
        }
        while (true)
        {
            const uint32_t compleate_current_paged_node_ready_bit = ReadMetaCellValue32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT);
            const uint32_t updated_current_ready_bit = compleate_current_paged_node_ready_bit | anew_readybit;
            if (updated_current_ready_bit == compleate_current_paged_node_ready_bit)
            {
                return true;
            }
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT, compleate_current_paged_node_ready_bit, updated_current_ready_bit))
            {
                return true;
            }
        }
        return false;
    }

    bool SegmentIODefinition::ClearTheDesiredPagedNodeReadyBit_(APCPagedNodeRelMaskClasses desired_region_class) noexcept
    {
        const uint32_t anew_readybit = APCAndPagedNodeHelpers::MakeOneAPCNodeClassReadyBit(desired_region_class);
        if (anew_readybit == 0)
        {
            return false;
        }
        while (true)
        {
            const uint32_t compleate_current_paged_node_ready_bit = ReadMetaCellValue32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT);
            const uint32_t updated_current_ready_bit = compleate_current_paged_node_ready_bit & ~anew_readybit;
            if (updated_current_ready_bit == compleate_current_paged_node_ready_bit)
            {
                return true;
            }
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::PAGED_NODE_READY_BIT, compleate_current_paged_node_ready_bit, updated_current_ready_bit))
            {
                return true;
            }
        }
        return false;
    }

    bool SegmentIODefinition::ForceZeroOccupancy_() noexcept
    {
        if (!IsBound())
        {
            return false;
        }

        const uint32_t payload_capacity = static_cast<uint32_t>(PayloadCapacityFromHeader());
        WriteExactMetaCellJustNewValue(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_PUBLISHED_CELLS, NO_VAL);
        WriteExactMetaCellJustNewValue(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_CLAIMED_CELLS, NO_VAL);
        WriteExactMetaCellJustNewValue(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_IDLE_CELLS, payload_capacity);
        WriteExactMetaCellJustNewValue(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_FAULTY_CELLS, NO_VAL);

        for (uint8_t i = 0; i < APCAndPagedNodeHelpers::SIZE_OF_APCPagedNodeRelMaskClasses; i++)
        {
            WriteExactMetaCellJustNewValue(
                APCAndPagedNodeHelpers::GetOccupancyMetIndexByRegionClass(static_cast<APCPagedNodeRelMaskClasses>(i)),
                NO_VAL
            );
        }
        
        WriteExactMetaCellJustNewValue(MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_PUBLISHED_CELLS, NO_VAL);
        return true;      
    }


}