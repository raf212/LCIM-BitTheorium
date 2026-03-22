#pragma once 
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp"



namespace PredictedAdaptedEncoding
{
#define HALF16Bit_THRESHOLD_WRAP 0x8000u
#define MIN_TIMER_DOWNSHIFT 6
#define MAX_TIMER_DOWNSHIFT 14
#define A_BILLION 1000000000ull
#define THRESHHOLD_64BIT 1e-12

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
    class MasterClockConf
    {
    private:

        bool OwnsSlots_ = false;
        unsigned TimerDownShift_ = 10u;
        unsigned RelShift_ = 12u;
        static constexpr unsigned REL_BUCKET_BITS__RELMASK = 4u;

        static inline size_t& ThreadLocalMasterClockID_() noexcept
        {
            static thread_local size_t id = SIZE_MAX;
            return id;
        }
        std::unique_ptr<std::atomic<uint64_t>[]> SlotLast48_{nullptr};
        std::unique_ptr<std::atomic<uint64_t>[]> SlotEpochHigh_{nullptr};
        
        packed64_t MakeInitialCellTimer48_(uint64_t now_ticks) noexcept
        {
            packed64_t init_clk48_packed = PackedCell64_t::ComposeCLK48u_64(now_ticks, MakeSTRL4_t(ZERO_PRIORITY, PackedCellLocalityTypes::ST_IDLE, REL_NONE, 
                RelOffsetMode::RELOFFSET_GENERIC_VALUE, PackedMode::MODE_CLKVAL48));
            return init_clk48_packed;
        }

        inline uint64_t GetCLK16Window_() const noexcept
        {
            return (uint64_t(1) << (TimerDownShift_ + CLK_B16));
        }

        inline uint64_t GetHalfWindow_() const noexcept
        {
            return (GetCLK16Window_() >> 1);
        }


    public:
        Timer48& MasterTimer48;
        int UsedNode = 0;
        //master clock
        std::atomic<packed64_t>* MasterClockSlotsPtr = nullptr;
        size_t MasterCLKCapacity = 0;
        std::atomic<size_t> MasterClockAlloc = 0;

        MasterClockConf(Timer48& ab, int used_node = REL_NODE0) noexcept:
            MasterTimer48(ab), UsedNode(used_node)
        {}

        ~MasterClockConf() noexcept
        {
            try
            {
                FreeMasterClockSlots();
            }
            catch(...)
            {
                // do not throw any exception ??
            }
        }

        MasterClockConf(const MasterClockConf&) = delete;
        MasterClockConf& operator = (const MasterClockConf&) = delete;

        MasterClockConf(MasterClockConf&& other) noexcept:
            MasterTimer48(other.MasterTimer48), UsedNode(other.UsedNode), MasterClockSlotsPtr(other.MasterClockSlotsPtr),
            MasterCLKCapacity(other.MasterCLKCapacity), MasterClockAlloc(other.MasterClockAlloc.load(MoLoad_)), OwnsSlots_(other.OwnsSlots_),
            SlotLast48_(std::move(other.SlotLast48_)), SlotEpochHigh_(std::move(other.SlotEpochHigh_)), TimerDownShift_(other.TimerDownShift_), RelShift_(other.RelShift_)
        {
            other.MasterClockSlotsPtr = nullptr;
            other.MasterCLKCapacity = 0;
            other.MasterClockAlloc.store(0, MoStoreUnSeq_);
            other.OwnsSlots_ = false;
        }

        MasterClockConf& operator = (MasterClockConf&& other) noexcept = delete;

        struct StampResult
        {
            clk16_t SequentialClock16;
            tag8_t RelMask4;
            uint64_t NowTicks;
        };

        bool InitMasterClockSlots(size_t max_slots, size_t allignment = 64)
        {
            (void)allignment;
            if (max_slots == 0)
            {
                throw std::invalid_argument("MAX SLOTS == 0");
            }
            if (MasterClockSlotsPtr != nullptr)
            {
                return false;
            }
            MasterClockSlotsPtr = new std::atomic<packed64_t>[max_slots];
            SlotLast48_.reset(new std::atomic<uint64_t>[max_slots]);
            SlotEpochHigh_.reset(new std::atomic<uint64_t>[max_slots]);
            uint64_t current_now_48 = MasterTimer48.NowTicks();
            packed64_t init_clk48_packed = MakeInitialCellTimer48_(current_now_48);
            uint64_t window = GetCLK16Window_();
            uint64_t initial_epoch = current_now_48 / window;
            for (size_t i = 0; i < max_slots; i++)
            {
                MasterClockSlotsPtr[i].store(init_clk48_packed, MoStoreSeq_);
                SlotLast48_[i].store(current_now_48, MoStoreSeq_);
                SlotEpochHigh_[i].store(initial_epoch, MoStoreSeq_);
            }
            MasterCLKCapacity = max_slots;
            MasterClockAlloc.store(0, MoStoreSeq_);
            OwnsSlots_ = true;
            return true;
                        
        }

        void FreeMasterClockSlots() noexcept
        {
            if (!MasterClockSlotsPtr)
            {
                return;
            }
            if (OwnsSlots_)
            {
                delete[] MasterClockSlotsPtr;
            }
            MasterClockSlotsPtr = nullptr;
            MasterCLKCapacity = 0;
            MasterClockAlloc.store(0, MoStoreSeq_);
            OwnsSlots_ = false;
            SlotLast48_.reset(nullptr);
            SlotEpochHigh_.reset(nullptr);
        }

        size_t RegisterMasterClockSlot(packed64_t given_init_clk = 0, size_t master_clock_id = SIZE_MAX) noexcept
        {
            if (!MasterClockSlotsPtr || MasterCLKCapacity == 0)
            {
                return SIZE_MAX;
            }

            auto prepare_cell_clock = [&](packed64_t seed)->packed64_t
            {
                if (seed == 0)
                {
                    uint64_t now = MasterTimer48.NowTicks();
                    return MakeInitialCellTimer48_(now);
                }
                else
                {
                    return seed;
                }
            };
            if (master_clock_id != SIZE_MAX)
            {
                if (master_clock_id >= MasterCLKCapacity)
                {
                    return SIZE_MAX;
                }
                packed64_t packed_clk48 = prepare_cell_clock(given_init_clk);
                MasterClockSlotsPtr[master_clock_id].store(packed_clk48, MoStoreSeq_);
                MasterClockSlotsPtr[master_clock_id].notify_all();
                uint64_t now_ticks = PackedCell64_t::ExtractClk48(packed_clk48);
                SlotLast48_[master_clock_id].store(now_ticks, MoStoreSeq_);
                SlotEpochHigh_[master_clock_id].store(now_ticks / GetCLK16Window_(), MoStoreSeq_);
                return master_clock_id;
            }
            else
            {
                size_t id = MasterClockAlloc.fetch_add(1, std::memory_order_acq_rel);
                if (id < MasterCLKCapacity)
                {
                    return RegisterMasterClockSlot(given_init_clk, id);
                }
                else
                {
                    return SIZE_MAX;
                }
                
            }
        }

        size_t AttachThreadMClockID(size_t mclock_id) const noexcept
        {
            size_t previous = ThreadLocalMasterClockID_();
            ThreadLocalMasterClockID_() = mclock_id;
            return previous;
        }

        packed64_t ReadMasterClockPacked(size_t mclock_id) const noexcept
        {
            if (!MasterClockSlotsPtr || mclock_id >= MasterCLKCapacity)
            {
                return 0;
            }
            return MasterClockSlotsPtr[mclock_id].load(MoLoad_);
        }

        clk16_t GetImmidiateDownshiftedClock16(uint64_t now_ticks) noexcept
        {
            return static_cast<clk16_t>((now_ticks >> TimerDownShift_) & MaskBits(CLK_B16));
        }

        inline tag8_t ComputeRelationMask4FromCLK16(clk16_t clock16) const noexcept
        {
            if (RelShift_ >= 16)
            {
                return 0;
            }
            return static_cast<tag8_t>((static_cast<unsigned>(clock16) >> RelShift_) & ((1u << REL_BUCKET_BITS__RELMASK) - 1u));
        }

        inline StampResult StampFromMasterSlot(size_t master_clock_id, unsigned rel_mask4 = REL_MASK4_NONE) noexcept
        {
            StampResult stamp_result{
                NO_VAL,
                NO_VAL,
                NO_VAL
            };
            uint64_t now_ticks = MasterTimer48.NowTicks();
            stamp_result.NowTicks = now_ticks;
            stamp_result.SequentialClock16 = GetImmidiateDownshiftedClock16(now_ticks);
            if (!rel_mask4)
            {
                stamp_result.RelMask4 = ComputeRelationMask4FromCLK16(stamp_result.SequentialClock16);
            }
            else
            {
                stamp_result.RelMask4 = static_cast<tag8_t>(rel_mask4 & MaskBits(4));
            }
            if (master_clock_id < MasterCLKCapacity && SlotLast48_)
            {
                SlotLast48_[master_clock_id].store(now_ticks, MoStoreSeq_);
                SlotEpochHigh_[master_clock_id].store(now_ticks / GetCLK16Window_(),  MoStoreSeq_);
            }
            return stamp_result;
        }

        inline std::optional<uint64_t> ComputeReconstructed48fromCLK16(size_t master_clock_id, clk16_t clk16) const noexcept
        {
            if (!SlotLast48_ || master_clock_id >= MasterCLKCapacity || !SlotEpochHigh_)
            {
                return std::nullopt;
            }
            uint64_t last48_clock = SlotLast48_[master_clock_id].load(MoLoad_);
            uint64_t current_epoch = SlotEpochHigh_[master_clock_id].load(MoLoad_);
            uint64_t window = GetCLK16Window_();
            uint64_t low_bits = (static_cast<uint64_t>(clk16) << TimerDownShift_) & (window - 1);
            uint64_t candidate_master_time = current_epoch * window + low_bits;
            uint64_t diffarance_in_time = (candidate_master_time > last48_clock) ? (candidate_master_time - last48_clock) : (last48_clock - candidate_master_time);
            if (diffarance_in_time > GetHalfWindow_())
            {
                if (candidate_master_time > window)
                {
                    uint64_t down = candidate_master_time - window;
                    uint64_t down2 = (down > last48_clock) ? (down - last48_clock) : (last48_clock - down);
                    if (down2 < diffarance_in_time)
                    {
                        candidate_master_time = down;
                        diffarance_in_time = down2;
                    }
                    {
                        uint64_t up = candidate_master_time + window;
                        uint64_t downed_up = (up > last48_clock) ? (up - last48_clock) : (last48_clock - up);
                        if (downed_up < diffarance_in_time)
                        {
                            candidate_master_time = up;
                            diffarance_in_time = downed_up;
                        }
                    }
                }
            }
        
            if (diffarance_in_time  > GetHalfWindow_())
            {
                return std::nullopt;
            }
            return candidate_master_time & MaskBits(CLK_B48);
        }

        inline void BackgroundRefreshSlotWithEpoch(size_t master_clock_id) noexcept
        {
            if (!SlotLast48_ || master_clock_id >= MasterCLKCapacity || !SlotEpochHigh_)
            {
                return;
            }
            uint64_t now_ticks = MasterTimer48.NowTicks();
            SlotLast48_[master_clock_id].store(now_ticks, MoStoreSeq_);
            SlotEpochHigh_[master_clock_id].store(now_ticks / GetCLK16Window_(), MoStoreSeq_);
            if (MasterCLKCapacity)
            {
                packed64_t packed_timer48 = MakeInitialCellTimer48_(now_ticks);
                MasterClockSlotsPtr[master_clock_id].store(packed_timer48, MoStoreSeq_);
                MasterClockSlotsPtr[master_clock_id].notify_all();
            }
            
        }

        inline uint64_t ReadSlotLast48(size_t master_clock_id) noexcept
        {
            if (!SlotLast48_ || master_clock_id >= MasterCLKCapacity)
            {
                return NO_VAL;
            }
            return SlotLast48_[master_clock_id].load(MoLoad_);
        }

        inline uint64_t ReadSlotEpochHigh(size_t master_clock_id) noexcept
        {
            if (!SlotEpochHigh_ || master_clock_id >= MasterCLKCapacity)
            {
                return 0;
            }
            return SlotEpochHigh_[master_clock_id].load(MoStoreSeq_);
        }

        inline uint8_t SetAndGetTimerDownshift(unsigned downshift_value = 0) noexcept
        {
            if (downshift_value >= MIN_TIMER_DOWNSHIFT && downshift_value <= MAX_TIMER_DOWNSHIFT)
            {
                TimerDownShift_ = downshift_value;
            }
            return static_cast<uint8_t>(TimerDownShift_);
        }

        inline std::optional<uint64_t> TryReconstructOrRefresh(size_t master_ckock_id, clk16_t clock16, bool allow_refresh = true) noexcept
        {
            auto probable_reconstruction_clock48 = ComputeReconstructed48fromCLK16(master_ckock_id, clock16);
            if (probable_reconstruction_clock48)
            {
                return probable_reconstruction_clock48;
            }
            if (!allow_refresh)
            {
                return std::nullopt;
            }
            BackgroundRefreshSlotWithEpoch(master_ckock_id);
            return ComputeReconstructed48fromCLK16(master_ckock_id, clock16);
        }

        inline packed64_t ComposeValue32WithMasterStamp(val32_t value32, size_t master_slot_id, tag8_t rel_mask4 = REL_MASK4_NONE)
        {
            uint64_t now_ticks = MasterTimer48.NowTicks();
            clk16_t downshifted_clock16 = GetImmidiateDownshiftedClock16(now_ticks);
            tag8_t composed_rel_mask4 = 0;
            if (rel_mask4 != REL_MASK4_NONE)
            {
                composed_rel_mask4 = static_cast<tag8_t>((rel_mask4 & MaskBits(REL_BUCKET_BITS__RELMASK)));
            }
            else
            {
                composed_rel_mask4 = ComputeRelationMask4FromCLK16(downshifted_clock16);
            }
            strl16_t strl = MakeSTRL4_t(DEFAULT_INTERNAL_PRIORITY, PackedCellLocalityTypes::ST_PUBLISHED, rel_mask4, RelOffsetMode::RELOFFSET_GENERIC_VALUE, PackedMode::MODE_VALUE32);
            packed64_t packed_value_mode_32 = PackedCell64_t::ComposeValue32u_64(value32, downshifted_clock16, strl);

            if (master_slot_id < MasterCLKCapacity && SlotLast48_)
            {
                SlotLast48_[master_slot_id].store(now_ticks, MoStoreSeq_);
                SlotEpochHigh_[master_slot_id].store(now_ticks / GetCLK16Window_(), MoStoreSeq_);
            }
            return packed_value_mode_32;
        }

    };
    
    


}