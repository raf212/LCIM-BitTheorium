#include "APCSegmentsCausalCordinator.hpp"

namespace PredictedAdaptedEncoding
{

    explicit MasterClockConf::MasterClockConf(AdaptivePackedCellContainer* apc_ptr, Timer48& master_timer) noexcept :
        MasterTimer48_(master_timer), APCPtr_(apc_ptr)
    {
        if (APCPtr_->IfAPCBranchValid())
        {
            SegmentIODefinitionPtr_ = APCPtr_->GetBranchPlugin();
        }
        
    }

    packed64_t MasterClockConf::RefreshPackedCellClockOnly(
        packed64_t provided_packed_cell,
        APCPagedNodeRelMaskClasses force_rel_mask,
        std::optional<PackedCellLocalityTypes> override_locality
    ) noexcept
    {
        const uint64_t now_ticks48 = NowTicks48();
        const clk16_t now_clk16 = GetImmidiateDownShiftedClock16(now_ticks48);

        const PriorityPhysics priority_of_provided_cell = PackedCell64_t::ExtractPriorityFromPacked(provided_packed_cell);
        PackedCellLocalityTypes locality_of_provided_cell = PackedCell64_t::ExtractLocalityFromPacked(provided_packed_cell);
        if (override_locality.has_value())
        {
            locality_of_provided_cell = *override_locality;
        }
        const tag8_t rel_mask = (force_rel_mask == APCPagedNodeRelMaskClasses::NANNULL) ? 
                        PackedCell64_t::ExtractRelMaskFromPacked(provided_packed_cell) : static_cast<tag8_t>(force_rel_mask);
        const PackedCellDataType dtype_of_provided_cell = PackedCell64_t::ExtractPCellDataTypeFromPacked(provided_packed_cell);
        const PackedMode mode_of_provided_cell = PackedCell64_t::ExtractModeOfPackedCellFromPacked(provided_packed_cell);
        if (mode_of_provided_cell == PackedMode::MODE_VALUE32)
        {
            const val32_t value32_of_provided_cell = PackedCell64_t::ExtractValue32(provided_packed_cell);
            const RelOffsetMode32 reloffset32_of_provided_cell = static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(provided_packed_cell));
            return PackedCell64_t::ComposeValue32u_64(
                value32_of_provided_cell,
                now_clk16,
                MakeSTRLMode32_t(priority_of_provided_cell, locality_of_provided_cell, rel_mask, reloffset32_of_provided_cell, dtype_of_provided_cell)
            );
        }

        const RelOffsetMode48 reloffset48_of_provided_cell = static_cast<RelOffsetMode48>(PackedCell64_t::ExtractRelOffsetFromPacked(provided_packed_cell));
        
        if (reloffset48_of_provided_cell == RelOffsetMode48::RELOFFSET_PURE_TIMER)
        {
            return PackedCell64_t::ComposeCLK48u_64(
                now_ticks48,
                //rename Strl to STRL(future)
                MakeStrl4ForMode48_t(priority_of_provided_cell, locality_of_provided_cell, rel_mask, reloffset48_of_provided_cell, dtype_of_provided_cell)
            );
        }
        return provided_packed_cell;
    }

    inline void MasterClockConf::AttachCurrentThreadSegment() noexcept
    {
        if (APCPtr_)
        {
            SegmentIODefinitionPtr_ = APCPtr_->GetBranchPlugin();
        }
    }


    std::optional<packed64_t> MasterClockConf::TouchPackedCellClockAndGetCellWithNewClock(
        size_t index_of_packed_cell,
        APCPagedNodeRelMaskClasses force_rel_mask,
        std::optional<PackedCellLocalityTypes> override_locality
    ) noexcept
    {
        if (!APCPtr_)
        {
            return std::nullopt;
        }
        if (!APCPtr_->IfIndexValid(index_of_packed_cell))
        {
            return std::nullopt;
        }
        packed64_t current_packed_cell = APCPtr_->BackingPtr[index_of_packed_cell].load(MoLoad_);
        while (true)
        {
            const packed64_t refreshed_packed_cell = RefreshPackedCellClockOnly(current_packed_cell, force_rel_mask, override_locality);
            if (APCPtr_->BackingPtr[index_of_packed_cell].compare_exchange_strong(
                current_packed_cell,
                refreshed_packed_cell,
                OnExchangeSuccess,
                OnExchangeFailure
            ))
            {
                return refreshed_packed_cell;
            }
            if (APCPtr_->GetAtomicAdaptiveBackoffPtr())
            {
                APCPtr_->GetAtomicAdaptiveBackoffPtr()->AdaptiveBackOffPacked(current_packed_cell);
            }
        }
    }

    bool MasterClockConf::TouchSegmentLocalClock48HighPriority() noexcept
    {
        if (!APCPtr_ || !SegmentIODefinitionPtr_)
        {
            return false;
        }
        packed64_t wanted_pure_clock48 = ComposePureClockCell48(PriorityPhysics::TIME_DEPENDENCY);
        APCPtr_->BackingPtr[static_cast<size_t>(MetaIndexOfAPCNode::LOCAL_CLOCK48)].store(wanted_pure_clock48, MoStoreSeq_);
        APCPtr_->BackingPtr[static_cast<size_t>(MetaIndexOfAPCNode::LOCAL_CLOCK48)].notify_all();
        return true;
    }

}