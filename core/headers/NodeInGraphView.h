#pragma once
#include "AdaptivePackedCellContainer/APCHelpers.hpp"
#include "AdaptivePackedCellContainer.hpp"

namespace PredictedAdaptedEncoding
{
struct PageNodeHelper
{
    enum class APCPortControl : uint8_t
    {
        NULL_SEQUENSE = 0,
        START_SEQUENSE = 1,
        STOP_SEQUENSE = 2
    };

    enum class NodeControlMask : uint8_t
    {
        UNDEFINED_ORDER = 0,
        SELF_ORDER = 1,
        FEEDFORWARD_EVIDANCE = 2,
        FEEDBACKWARD_EVIDANCE = 3,
        RESIDUAL_ERROR = 4,
        LATENT_STATE = 5,
        LATERAL_SIGNAL = 6
    };
    

    static inline std::optional<NodeControlMask> ExtractNodeControl(packed64_t packed_cell) noexcept
    {
        if (!PredictedAdaptedEncoding::APCHelpers::IsCellPublishedMode32Generic(packed_cell))
        {
            return std::nullopt;
        }

        if (PackedCell64_t::ExtractPCellDataTypeFromPacked(packed_cell) != PackedCellDataType::CharPCellDataType)
        {
            return std::nullopt;
        }

        return static_cast<NodeControlMask>(PackedCell64_t::ExtractRelMaskFromPacked(packed_cell));
        
    }

    static inline std::optional<APCPortControl> ExtractControlSequense(packed64_t packed_cell) noexcept
    {
        if (ExtractNodeControl(packed_cell) != NodeControlMask::SELF_ORDER)
        {
            return std::nullopt;
        }
        return static_cast<APCPortControl>(PackedCell64_t::ExtractValue32(packed_cell));
    }

    static inline bool IsControlStopCell(packed64_t packed_cell) noexcept
    {
        if (ExtractControlSequense(packed_cell) == APCPortControl::STOP_SEQUENSE)
        {
            return true;
        }
        return false;
    }
};

        

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
    // bool AcceptCausalCellForPort(PageNodeHelper::APCPortKind port_kind, packed64_t packed_cell) noexcept;
    
};

struct BidirectionalNodeStepResult
{
    bool ConsumedFeedForward = false;
    bool ConsumedFeedback = false;
    bool EmittedForward = false;
    bool EmittedBackward = false;
    bool EmittedLateral = false;
    uint32_t Evidence = 0;
    uint32_t Prediction = 0;
    uint32_t Residual = 0;
    uint32_t Latent = 0;
};

class BidirectionalAPCNode
{
private:
    AdaptivePackedCellContainer* APCPtr_;
public:
    BidirectionalAPCNode(AdaptivePackedCellContainer* adaptive_packed_cell_container = nullptr) noexcept :
        APCPtr_(adaptive_packed_cell_container)
    {}

    uint32_t ReadComputeKind() const noexcept;
    bool SetAUXUint32(uint32_t value) noexcept;
    uint32_t ReadAUXUint32() const noexcept;
    bool BindFeedForwardTo(BidirectionalAPCNode& next_apc_node) noexcept;
    bool BindFeedBackwardTo(BidirectionalAPCNode& previous_apc_node) noexcept;
};
    
}

