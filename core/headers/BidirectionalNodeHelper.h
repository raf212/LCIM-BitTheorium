#include "AdaptivePackedCellContainer.hpp"

namespace PredictedAdaptedEncoding
{
    enum class APCPortKind : uint8_t
    {
        SELF_APC = 0,
        FEEFORWARD = 1,
        FEEDBACKWARD = 2,
        LATERAL = 3
    };

    struct APCOutputPortView
    {
        AdaptivePackedCellContainer* TargetPtr = nullptr;
        APCPortKind PortKind = APCPortKind::SELF_APC;
        uint8_t PortPriority = ZERO_PRIORITY;
        tag8_t AcceptedRelMask = REL_ALL_LOW_4;
        bool EnabledPortView = false;
    };

    struct APCNodeOutputFebricView
    {
        AdaptivePackedCellContainer* SelfPter = nullptr;
        std::array<APCOutputPortView, 5> Ports{};
        size_t PortCount = 0;
    };

    
}