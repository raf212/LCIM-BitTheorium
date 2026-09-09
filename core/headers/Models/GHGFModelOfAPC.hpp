#pragma once 
#include "../NeuromorphicTimeSpace/VagueTemoraryPremativeFabric.hpp"

namespace BidirectionalInMemGraph
{

    class GHGFModelConstructor : private VagueTemoraryPremativeFabric
    {
    public:
        using GM = GHGFLayerModel;
        using FCSpan = std::span<const float>;
        struct GHGFModelConstructionValues
        {
            std::span<AdaptivePackedCellContainer> APCParticipentSpan{};
            std::span<const GM::GHGFNodeRole> RoleSpan{};
            std::span<const GM::GHGFConnection> ConnectionSpan{};            
        };
    private:
        GM::GHGFCache Cache_{};
        GM::GHGFStorageProfile Profile_{};

        GM::GHGFNodeRole GHGFRole_(uint32_t slot) const noexcept;
        bool IsExternalGHGFBuffer_(const float* data, size_t count) const noexcept;
        bool IsLiveGHGFSlot_(uint32_t slot) noexcept;
        bool IsGHGFPlanCurrent_() noexcept;
        bool PredictGHGFNode_(uint32_t slot, uint32_t batch) noexcept;
        bool UpdateGHGFNode_(uint32_t slot, uint32_t batch) noexcept;
        bool PropogateGHGFError_(uint32_t child, uint32_t batch) noexcept;
        bool PredictGHGFBatch(uint32_t batch) noexcept;
        bool UpdateGHGFBatch(FCSpan observation, uint32_t batch) noexcept;
        bool CopyGHGFPrediction_(FCSpan prediction, uint32_t batch) noexcept;

        float* GHGFRegion_(uint32_t slot, uint32_t cell_offset) noexcept;
        float* GHGFStateRow_(uint32_t slot, GM::GHGFStateRow row) noexcept;
        float* GHGFErrorRow_(uint32_t slot, GM::GHGFErrorRow row) noexcept;

        void InvalidateGHGFModel_() noexcept;
        void ResetAPCGHGFStateRegion_(uint32_t slot) noexcept;

        uint64_t GHGFParentMask_(uint32_t slot, FabricSegments axis) noexcept;
    public :

        bool ConnectGHGFParent(const GM::GHGFConnection& connection) noexcept;

        bool ResetGHGFState() noexcept;

        bool CompileGHGFModel() noexcept;

        bool InitializeGHGFFabric(
            uint32_t slot_count,
            const GHGFLayerModel::GHGFStorageProfile& profile
        ) noexcept;

        bool InitializeGHGFNode(
            AdaptivePackedCellContainer& apc,
            GM::GHGFNodeRole role
        ) noexcept;

        bool CreateNodeOfGHGF(
            AdaptivePackedCellContainer& desired_apc,
            GM::GHGFNodeRole role
        ) noexcept
        {
            return 
                HasDefaultRegionTable_ && 
                CreateAPC(desired_apc, DefaultRegionTable_) &&
                InitializeGHGFNode(desired_apc, role);
        }
        
        bool ConstructGHGFModel(
            GHGFModelConstructionValues& model_values,
            const GHGFLayerModel::GHGFStorageProfile& profile
        ) noexcept;
            
    };
}