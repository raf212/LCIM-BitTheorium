#pragma once
#include "Models/GHGFModelOfAPC.hpp"
#include <span>

namespace BidirectionalInMemGraph
{ 

    bool GHGFModelConstructor::IsLiveGHGFSlot_(uint32_t slot) noexcept
    {
        if (!IsFabricActive() || slot >= CountOfAPC_)
        {
            return false;
        }

        const uint64_t raw = std::atomic_ref<uint64_t>(*GetAPCGenerationPtr_(slot)).load(std::memory_order_acquire);
        const HandleOfAPCStatic::ControlValues control = HandleOfAPCStatic::ReadControlCell(raw);
        const uint64_t role = SlabBasePtr_[SlotBegin_(slot) + static_cast<uint8_t>(APCDataStructure::HeaderIdentifierOfAPC::GHGF_ROLE_CELL)];
        return !control.Closed  && role >= 1u &&
            role <= static_cast<uint64_t>(GM::GHGFNodeRole::VOLATILE) + 1u;
    }

    bool GHGFModelConstructor::IsGHGFPlanCurrent_() noexcept
    {
        return IsFabricActive() && Cache_.ModelPrepared_ &&
            Cache_.PreparedRevision_ == CompiledDagRevision_.load(std::memory_order_acquire);
    }

    float* GHGFModelConstructor::GHGFRegion_(uint32_t slot, uint32_t cell_offset) noexcept
    {
        return RegionT_<float>(slot, cell_offset);
    }

    float* GHGFModelConstructor::GHGFStateRow_(uint32_t slot, GM::GHGFStateRow row) noexcept
    {
        return GHGFRegion_(slot, Cache_.StateCellOffset_) +
            static_cast<size_t>(row) * Profile_.BatchCapacity;
    }

    float* GHGFModelConstructor::GHGFErrorRow_(uint32_t slot, GM::GHGFErrorRow row) noexcept
    {
        return GHGFRegion_(slot, Cache_.ErrorCellOffset_) +
            static_cast<size_t>(row) * Profile_.BatchCapacity;
    }

    uint64_t GHGFModelConstructor::GHGFParentMask_(uint32_t slot, FabricSegments axis) noexcept
    {
        CompiledDAGRecord* record = CompiledDAGRow_(slot);
        return axis == FabricSegments::VALUE_PARENT_EDGE_TABLE_H ?
            record->ValueParentMask : record->VolatileParentMask;
    }

    void GHGFModelConstructor::InvalidateGHGFModel_() noexcept
    {
        Cache_.ModelPrepared_ = false;
        Cache_.Phase_ = GM::GHGFPhase::NEEDS_RESET;
        Cache_.ActiveBatch_ = UNSIGNED_ZERO;
    }



    bool GHGFModelConstructor::InitializeGHGFFabric(
        uint32_t slot_count,
        const GHGFLayerModel::GHGFStorageProfile& profile
    ) noexcept
    {
        if (
            IsFabricActive() || 
            slot_count == UNSIGNED_ZERO || 
            !GM::IsValidStoregeProfile(profile)
        )
        {
            return false;
        }
        
        InvalidateGHGFModel_();
        Cache_.NodeCount_ = UNSIGNED_ZERO;
        Cache_.ObservationCount_ = UNSIGNED_ZERO;
        DefaultRegionTable_ = profile.DefaultSchemaTable;
        HasDefaultRegionTable_ = true;

        if (
            !InitializeFabricWithPtrTable(
                slot_count,
                profile.RequiredAPCCells,
                profile.FabricConfig,
                profile.MaxDirectParentPerAxis
            )
        )
        {
            return false;
        }

        for (const SD::RegionSchemaRecord& record : MetrixViewRow_(0u))
        {
            switch (record.Region)
            {
            case MacroColumnOfAPC::STATE_SLOT: Cache_.StateCellOffset_ = record.CellOffset; break;
            case MacroColumnOfAPC::ERROR_SLOT: Cache_.ErrorCellOffset_ = record.CellOffset; break;
            case MacroColumnOfAPC::WEIGHT_SLOT: Cache_.WeightCellOffset_ = record.CellOffset; break;
            default: 
                break;
            }
        }
        return true;
    }

    void GHGFModelConstructor::ResetAPCGHGFStateRegion_(uint32_t slot) noexcept
    {
        std::fill_n(
            GHGFRegion_(slot, Cache_.StateCellOffset_),
            static_cast<size_t>(GM::STATE_ROW_COUNT_HEIGHT) * Profile_.BatchCapacity, GM::StorageConst::ZERO
        );
        std::fill_n(
            GHGFRegion_(slot, Cache_.ErrorCellOffset_),
            static_cast<size_t>(GM::ERROR_ROW_COUNT_HEIGHT) * Profile_.BatchCapacity, GM::StorageConst::ZERO
        );
        const float initial_mean = GHGFRole_(slot) == GM::GHGFNodeRole::OBSERVATION ?
            GM::StorageConst::INITIAL_BINARY_PROBABILITY : GM::StorageConst::ZERO;

        for (uint32_t lane = 0; lane < Profile_.BatchCapacity; ++lane)
        {
            GHGFStateRow_(slot, GM::GHGFStateRow::EXPECTED_MEAN)[lane] = initial_mean;
            GHGFStateRow_(slot, GM::GHGFStateRow::PRECISION)[lane] = GM::StorageConst::INITIAL_PRECISION;
            GHGFStateRow_(slot, GM::GHGFStateRow::EXPECTED_PRECISION)[lane] = GM::StorageConst::INITIAL_PRECISION;
            GHGFStateRow_(slot, GM::GHGFStateRow::CONDITIONAL_EXPECTED_PRECISION)[lane] = GM::StorageConst::INITIAL_PRECISION;
            GHGFStateRow_(slot, GM::GHGFStateRow::OBSERVED)[lane] = GM::StorageConst::ONE;
            GHGFStateRow_(slot, GM::GHGFStateRow::CURRENT_VARIANCE)[lane] = GM::StorageConst::ONE;
        }
    }

    bool GHGFModelConstructor::ConstructGHGFModel(
        GHGFModelConstructionValues& model_values,
        const GHGFLayerModel::GHGFStorageProfile& profile
    ) noexcept
    {
        if (
            IsFabricActive() ||
            !GM::IsValidStoregeProfile(profile) ||
            model_values.APCParticipentSpan.empty() ||
            model_values.APCParticipentSpan.size() != model_values.RoleSpan.size() ||
            model_values.APCParticipentSpan.size() >= GM::StorageConst::INVALID_SLOT
        )
        {
            return false;
        }
        for (AdaptivePackedCellContainer& apc : model_values.APCParticipentSpan)
        {
            if (apc.IsActiveAPC())
            {
                return false;
            }
        }

        if (!InitializeGHGFFabric(static_cast<uint32_t>(model_values.APCParticipentSpan.size()), profile))
        {
            return false;
        }
        
        for (uint32_t i = 0; i < CountOfAPC_; i++)
        {
            if (!CreateNodeOfGHGF(model_values.APCParticipentSpan[i], model_values.RoleSpan[i]))
            {
                ShutDownFabricWithPtrTable();
                return false;
            }
        }
        
        for (const GM::GHGFConnection& connection : model_values.ConnectionSpan)
        {
            if (!ConnectGHGFParent(connection))
            {
                ShutDownFabricWithPtrTable();
                return false;
            }
        }
        
        if (!CompileGHGFModel() ||!ResetGHGFState())
        {
            ShutDownFabricWithPtrTable();
            return false;
        }
        return true;
    }
            
}