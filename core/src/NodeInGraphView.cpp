#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include "NodeInGraphView.h"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    void NodeInGraphView::AutoSetBranchPlugin_() noexcept
    {
        BranchPluginPtr_ = APCPtr_->GetBranchPlugin();        
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
            BranchPluginPtr_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDFORWARD_IN_TARGET_ID)
        );
        out_graph_view.FeedForwardOutPtr = APCManagerPtr_->GetAPCPtrFromBranchId(
            BranchPluginPtr_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDFORWARD_OUT_TARGET_ID)
        );
        out_graph_view.FeedBackwardIntPtr = APCManagerPtr_->GetAPCPtrFromBranchId(
            BranchPluginPtr_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDBACKWARD_IN_TARGET_ID)
        );
        out_graph_view.FeedBackwardOutPtr = APCManagerPtr_->GetAPCPtrFromBranchId(
            BranchPluginPtr_->ReadMetaCellValue32(PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDBACKWARD_OUT_TARGET_ID)
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
        target_apc->GetBranchPlugin()->SetGraphNodeFlag();
        const uint32_t target_id = target_apc->GetBranchId();
        const uint32_t self_id = APCPtr_->GetBranchId();
        bool bind_ok = BranchPluginPtr_->TryBindPortTarget(PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDFORWARD_OUT_TARGET_ID, target_id);
        bool bind_ok_2 = target_apc->GetBranchPlugin()->TryBindPortTarget(
            PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDFORWARD_IN_TARGET_ID, self_id
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
        target_apc->GetBranchPlugin()->SetGraphNodeFlag();
        const uint32_t target_id = target_apc->GetBranchId();
        const uint32_t self_id = APCPtr_->GetBranchId();
        bool bind_ok = BranchPluginPtr_->TryBindPortTarget(PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDBACKWARD_OUT_TARGET_ID, target_id);
        bool bind_ok_2 = target_apc->GetBranchPlugin()->TryBindPortTarget(
            PackedCellBranchPlugin::MetaIndexOfAPCBranch::FEEDBACKWARD_IN_TARGET_ID, self_id
        );

        return bind_ok && bind_ok_2;
    }

    

}