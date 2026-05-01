#pragma once 
#include "PackedCell/CoreCellDefination.hpp"
#include "APCHelpers.hpp"


namespace PredictedAdaptedEncoding
{
enum class APCPagedNodeRelMaskClasses : tag8_t;

class SegmentIODefinition;
class AdaptivePackedCellContainer;

    struct Timer48
    {
        uint64_t TicksPerSec_ = A_BILLION;

        inline uint64_t NowTicks() const noexcept
        {
            using  cns = std::chrono::nanoseconds;
            auto d = std::chrono::steady_clock::now().time_since_epoch();
            uint64_t ns_count = static_cast<uint64_t>(std::chrono::duration_cast<cns>(d).count());
            return ns_count & MaskBits(CLK_B48);
        }
    };

    class MasterClockConf final
    {
    private:
        Timer48& MasterTimer48_;
        unsigned TimerDownShift_ = 10u;
        AdaptivePackedCellContainer* APCPtr_ = nullptr;
    public:
        explicit MasterClockConf(AdaptivePackedCellContainer* apc_ptr, Timer48& master_timer) noexcept :
            APCPtr_(apc_ptr), MasterTimer48_(master_timer)
        {}
        MasterClockConf(const MasterClockConf&) = delete;
        MasterClockConf& operator = (const MasterClockConf&) = delete;
        MasterClockConf(MasterClockConf&&) = delete;
        MasterClockConf& operator = (MasterClockConf&&) = delete;
        ~MasterClockConf() noexcept = default;

        packed64_t RefreshPackedCellClockOnly(
            packed64_t provided_packed_cell,
            APCPagedNodeRelMaskClasses force_rel_mask = APCPagedNodeRelMaskClasses::NANNULL,
            std::optional<PackedCellLocalityTypes> override_locality = std::nullopt
        ) noexcept;

        std::optional<packed64_t> TouchPackedCellClockAndGetCellWithNewClock(
            size_t index_of_packed_cell,
            APCPagedNodeRelMaskClasses force_rel_mask = APCPagedNodeRelMaskClasses::NANNULL,
            std::optional<PackedCellLocalityTypes> override_locality = std::nullopt
        ) noexcept;

        bool TouchSegmentLocalClock48HighPriority() noexcept;

        bool TryAdvanceSegmentsLastAcceptedClock(APCPagedNodeRelMaskClasses desired_rel_class) noexcept;


        MasterClockConf* GetMasterClockConfPtr() noexcept
        {
            return this;
        }

        inline uint64_t NowTicks48() const noexcept
        {
            return MasterTimer48_.NowTicks();
        }

        inline clk16_t GetImmidiateDownShiftedClock16(uint64_t now_ticks48) const noexcept
        {
            return static_cast<clk16_t>((now_ticks48 >> TimerDownShift_) & MaskBits(CLK_B16));
        }

        inline clk16_t NowClock16() const noexcept
        {
            return GetImmidiateDownShiftedClock16(NowTicks48());
        }

        std::optional<uint64_t> ReconstructCellClock16toFull48BySegmentLocalClock48(size_t index_of_packed_cell) noexcept;


        inline packed64_t ComposeValue32WithCurrentThreadStamp16(
            val32_t provided_cell_value32,
            APCPagedNodeRelMaskClasses desired_rel_mask = APCPagedNodeRelMaskClasses::NONE,
            PriorityPhysics desired_priority = PriorityPhysics::IDLE,
            PackedCellLocalityTypes desired_locality = PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode32 desired_reloffset = RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType desired_dtype = PackedCellDataType::UnsignedPCellDataType
        )
        {
            const clk16_t now_clock16 = NowClock16();
            const strl16_t strlfor32 = MakeSTRLMode32_t(desired_priority, desired_locality, static_cast<tag8_t>(desired_rel_mask), desired_reloffset, desired_dtype);
            return PackedCell64_t::ComposeValue32u_64(provided_cell_value32, now_clock16, strlfor32);
        }

        inline packed64_t ComposePureClockCell48(
            PriorityPhysics desired_priority = PriorityPhysics::IDLE,
            PackedCellLocalityTypes desired_locality = PackedCellLocalityTypes::ST_PUBLISHED
        ) noexcept
        {
            const uint64_t full_clock48 = NowTicks48();
            const strl16_t strl_for_pure48_clock = MakeStrl4ForMode48_t(desired_priority, desired_locality, 
                                static_cast<tag8_t>(APCPagedNodeRelMaskClasses::CLOCK_PURE_TIME),
                                RelOffsetMode48::RELOFFSET_PURE_TIMER,
                                PackedCellDataType::UnsignedPCellDataType
                            );
            return PackedCell64_t::ComposeCLK48u_64(full_clock48, strl_for_pure48_clock);
        }

        inline uint8_t SetAndGetTimerDownShift(unsigned down_shift_value = NO_VAL) noexcept
        {
            if (down_shift_value >= MIN_TIMER_DOWNSHIFT && down_shift_value <= MAX_TIMER_DOWNSHIFT)
            {
                TimerDownShift_ = down_shift_value;
            }
            return static_cast<uint8_t>(TimerDownShift_);
        }

    };
    


}