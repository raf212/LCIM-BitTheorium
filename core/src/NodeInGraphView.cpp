#include "APCSegmentsCausalCordinator.hpp"
#include "PackedCellContainerManager.hpp"
#include "NodeInGraphView.h"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    void NodeInGraphView::AutoSetBranchPlugin_() noexcept
    {
        BranchPluginPtr_ = APCPtr_->GetSegmentIOPtr();        
    }

    GraphPortView NodeInGraphView::GetGraphPortView() noexcept
    {
        GraphPortView out_graph_view{};
        out_graph_view.SelfPtr = APCPtr_;
        if (!BranchPluginPtr_|| !APCManagerPtr_)
        {
            return out_graph_view;
        }
        out_graph_view.FeedForwardInPtr = APCManagerPtr_->GetAPCPtrFromBranchId(
            BranchPluginPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::FEEDFORWARD_IN_TARGET_ID)
        );
        out_graph_view.FeedForwardOutPtr = APCManagerPtr_->GetAPCPtrFromBranchId(
            BranchPluginPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::FEEDFORWARD_OUT_TARGET_ID)
        );
        out_graph_view.FeedBackwardIntPtr = APCManagerPtr_->GetAPCPtrFromBranchId(
            BranchPluginPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::FEEDBACKWARD_IN_TARGET_ID)
        );
        out_graph_view.FeedBackwardOutPtr = APCManagerPtr_->GetAPCPtrFromBranchId(
            BranchPluginPtr_->ReadMetaCellValue32(MetaIndexOfAPCNode::FEEDBACKWARD_OUT_TARGET_ID)
        );
        return out_graph_view;
    }

    bool NodeInGraphView::BindFeedForwardOut(AdaptivePackedCellContainer* target_apc) noexcept
    {
        if (!BranchPluginPtr_ || !target_apc)
        {
            return false;
        }
        
        BranchPluginPtr_->SetGraphNodeFlag();
        target_apc->GetSegmentIOPtr()->SetGraphNodeFlag();
        const uint32_t target_id = target_apc->GetBranchId();
        const uint32_t self_id = APCPtr_->GetBranchId();
        bool bind_ok = BranchPluginPtr_->TryBindPortTarget(MetaIndexOfAPCNode::FEEDFORWARD_OUT_TARGET_ID, target_id);
        bool bind_ok_2 = target_apc->GetSegmentIOPtr()->TryBindPortTarget(
            MetaIndexOfAPCNode::FEEDFORWARD_IN_TARGET_ID, self_id
        );

        return bind_ok && bind_ok_2;

    }

    bool NodeInGraphView::BindFeedBackwardOut(AdaptivePackedCellContainer* target_apc) noexcept
    {
        if (!BranchPluginPtr_ || !target_apc)
        {
            return false;
        }
        
        BranchPluginPtr_->SetGraphNodeFlag();
        target_apc->GetSegmentIOPtr()->SetGraphNodeFlag();
        const uint32_t target_id = target_apc->GetBranchId();
        const uint32_t self_id = APCPtr_->GetBranchId();
        bool bind_ok = BranchPluginPtr_->TryBindPortTarget(MetaIndexOfAPCNode::FEEDBACKWARD_OUT_TARGET_ID, target_id);
        bool bind_ok_2 = target_apc->GetSegmentIOPtr()->TryBindPortTarget(
            MetaIndexOfAPCNode::FEEDBACKWARD_IN_TARGET_ID, self_id
        );

        return bind_ok && bind_ok_2;
    }

    // bool NodeInGraphView::AcceptCausalCellForPort(PageNodeHelper::APCPortKind port_kind, packed64_t packed_cell) noexcept
    // {
    //     if (!BranchPluginPtr_)
    //     {
    //         return false;
    //     }
    //     if (PageNodeHelper::IsControlStopCell(packed_cell))
    //     {
    //         return true;
    //     }
        
    //     const uint16_t incoming_clock16 = PackedCell64_t::ExtractClk16(packed_cell);

    //     MetaIndexOfAPCNode last_accepted_feed_slot_idx = MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_FORWARD_CLOCK16;
    //     if (port_kind == PageNodeHelper::APCPortKind::FEED_BACKWARD_IN)
    //     {
    //         last_accepted_feed_slot_idx = MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_BACKWARD_CLOCK16;
    //     }
    //     while (true)
    //     {
    //         uint16_t last_accepted_clock16 = static_cast<uint16_t>(BranchPluginPtr_->ReadMetaCellValue32(last_accepted_feed_slot_idx));
    //         if (incoming_clock16 < last_accepted_clock16)
    //         {
    //             return false;
    //         }
    //         if (incoming_clock16 > last_accepted_clock16)
    //         {
    //             return true;
    //         }
    //         if (BranchPluginPtr_->JustUpdateValueOfMeta32(last_accepted_feed_slot_idx, last_accepted_clock16, incoming_clock16))
    //         {
    //             return true;
    //         }
    //     }
    // }

    

}