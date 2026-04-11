#pragma once
#include "MasterClockConf.hpp"
#include "APCSideHelper.hpp"

namespace PredictedAdaptedEncoding
{
class AdaptivePackedCellContainer;
class PackedCellContainerManager;
class PackedCellBranchPlugin;

struct GraphPortView
{
    AdaptivePackedCellContainer* SelfPtr = nullptr;
    AdaptivePackedCellContainer* FeedForwardInPtr = nullptr;
    AdaptivePackedCellContainer* FeedBackwardIntPtr = nullptr;
    AdaptivePackedCellContainer* FeedForwardOutPtr = nullptr;
    AdaptivePackedCellContainer* FeedBackwardOutPtr = nullptr;
};


class NodeInGraphView
{
private:
    AdaptivePackedCellContainer* APCPtr_;
    PackedCellContainerManager* APCManagerPtr_;
    PackedCellBranchPlugin* BranchPluginPtr_{nullptr};
    void AutoSetBranchPlugin_() noexcept;
    
public:
    NodeInGraphView(AdaptivePackedCellContainer* adaptive_packed_cell_container_ptr, PackedCellContainerManager* apc_manager_ptr) noexcept:
        APCPtr_(adaptive_packed_cell_container_ptr), APCManagerPtr_(apc_manager_ptr)
    {
        AutoSetBranchPlugin_();
    }
    GraphPortView GetGraphPortView() noexcept;
    bool BindFeedForwardOut(AdaptivePackedCellContainer* target_apc) noexcept;
    bool BindFeedBackwardOut(AdaptivePackedCellContainer* target_apc) noexcept;
    // bool AcceptCausalCellForPort(APCSideHelper::APCPortKind port_kind, packed64_t packed_cell) noexcept;
    
};
    
}

