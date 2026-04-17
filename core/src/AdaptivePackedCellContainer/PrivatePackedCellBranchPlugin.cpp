#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{

    void PackedCellBranchPlugin::InitDefaultAPCSegmentedNodeLayout_() noexcept
    {
        const uint32_t payload_begain = METACELL_COUNT;
        const uint32_t payload_end = PayloadEndRead();
        if (payload_end <= payload_begain)
        {
            return;
        }
        CompleteAPCNodeRegionsLayout full_paged_node_layout{};
        BuidDefaultLayoutPlan_(full_paged_node_layout);
        WriteBoundsPairToHeader_(full_paged_node_layout.FeedForwardLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.FeeDBackwardLAyout);
        WriteBoundsPairToHeader_(full_paged_node_layout.StateLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.ErrorLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.EdgeDescriptorLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.WeightLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.AUXLayout);
        WriteBoundsPairToHeader_(full_paged_node_layout.FreeLayout);

        WriteBrenchMeta32_(MetaIndexOfAPCNode::REGION_DIR_COUNT, TOTAL_LAYOUT_SECTION_IN_APC_CONTAINER_NODE);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::EDGE_TABLE_COUNT, NO_VAL);
        WriteBrenchMeta32_(MetaIndexOfAPCNode::WEIGHT_TABLE_COUNT, NO_VAL);

        TurnOnMultipleSegmentFlagsAtOnce_(static_cast<uint32_t>(APCFlags::HAS_LAYOUT_DIR));

        
    }

    void PackedCellBranchPlugin::BuidDefaultLayoutPlan_(CompleteAPCNodeRegionsLayout& full_layout) noexcept
    {
        const uint32_t payload_begain = METACELL_COUNT;
        const uint32_t payload_end = PayloadEndRead();
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
        AssignOne(full_layout.FeeDBackwardLAyout);
        AssignOne(full_layout.StateLayout);
        AssignOne(full_layout.ErrorLayout);
        AssignOne(full_layout.EdgeDescriptorLayout);
        AssignOne(full_layout.WeightLayout);
        AssignOne(full_layout.AUXLayout);

        full_layout.FreeLayout.BeginIndex = initial_cursor;
        full_layout.FreeLayout.EndIndex = payload_end;
    }

    bool PackedCellBranchPlugin::WriteBoundsPairToHeader_(const LayoutBoundsOfSingleRelNodeClass layout_bound) noexcept
    {
        auto maybe_region_bounds_pair = GetMetaBoundsPairForRegionMask_(layout_bound.LAYOUT_CLASS);
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

    std::optional<std::pair<MetaIndexOfAPCNode, MetaIndexOfAPCNode>>PackedCellBranchPlugin::GetMetaBoundsPairForRegionMask_(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept
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

    bool PackedCellBranchPlugin::UpdateAPCModeFlagsInHeader_(uint32_t flags_to_turn_on, uint32_t flags_to_turn_off) noexcept
    {
        while (true)
        {
            const uint32_t current_flags = ReadAPCModeFlags_();
            uint32_t next_flags = current_flags;
            next_flags |= flags_to_turn_on;
            next_flags &= ~flags_to_turn_off;
            if (next_flags == current_flags)
            {
                return true;
            }
            if (JustUpdateValueOfMeta32(MetaIndexOfAPCNode::SEGMENT_CONF_FLAGS, current_flags, next_flags))
            {
                return true;
            }
        }
    }
}