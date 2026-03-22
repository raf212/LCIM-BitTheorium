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
        static constexpr unsigned REL_MASK_SIZE = 4u;

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

        inline void RefreshSlotEpochState_(size_t master_clock_id, uint64_t now_ticks) noexcept
        {
            if (!SlotLast48_ || !SlotEpochHigh_ || master_clock_id >= MasterCLKCapacity)
            {
                return;
            }
            SlotLast48_[master_clock_id].store(now_ticks, MoStoreSeq_);
            SlotEpochHigh_[master_clock_id].store(now_ticks / GetCLK16Window_(), MoStoreSeq_);
        }

        inline tag8_t ResolveRelMask4_(clk16_t clk16, tag8_t rel_mask4) const noexcept
        {
            if (rel_mask4 != REL_MASK4_NONE)
            {
                return static_cast<tag8_t>(rel_mask4 & MaskBits(REL_MASK_SIZE));
            }
            return ComputeRelationMask4FromCLK16(clk16);
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
            clk16_t SequentialClock16 = 0;
            tag8_t RelMask4 = 0;
            uint64_t NowTicks = 0;
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
            //allocation 
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
                return seed;
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
                RefreshSlotEpochState_(master_clock_id, now_ticks);
                return master_clock_id;
            }

            size_t id = MasterClockAlloc.fetch_add(1, std::memory_order_acq_rel);
            if (id < MasterCLKCapacity)
            {
                return RegisterMasterClockSlot(given_init_clk, id);
            }
            return SIZE_MAX;
        }

        size_t AttachThreadMClockID(size_t mclock_id) const noexcept
        {
            size_t previous = ThreadLocalMasterClockID_();
            ThreadLocalMasterClockID_() = mclock_id;
            return previous;
        }

        size_t GetAttachedThreadMasterClockID() const noexcept
        {
            return ThreadLocalMasterClockID_();
        }

        size_t EnsureOrAssignThreadIdForMasterClock() noexcept
        {
            size_t current_thread_id = ThreadLocalMasterClockID_();
            if (current_thread_id != SIZE_MAX && current_thread_id < MasterCLKCapacity)
            {
                return current_thread_id;
            }
            size_t fresh_master_clock_thread_id = RegisterMasterClockSlot();
            if (fresh_master_clock_thread_id != SIZE_MAX)
            {
                ThreadLocalMasterClockID_() = fresh_master_clock_thread_id;
            }
            return fresh_master_clock_thread_id;
        }

        packed64_t ReadMasterClockPacked(size_t mclock_id) const noexcept
        {
            if (!MasterClockSlotsPtr || mclock_id >= MasterCLKCapacity)
            {
                return 0;
            }
            return MasterClockSlotsPtr[mclock_id].load(MoLoad_);
        }

        clk16_t GetImmidiateDownshiftedClock16(uint64_t now_ticks) const noexcept
        {
            return static_cast<clk16_t>((now_ticks >> TimerDownShift_) & MaskBits(CLK_B16));
        }

        inline tag8_t ComputeRelationMask4FromCLK16(clk16_t clock16) const noexcept
        {
            if (RelShift_ >= CLK_B16)
            {
                return NO_VAL;
            }
            return static_cast<tag8_t>((static_cast<unsigned>(clock16) >> RelShift_) & ((1u << REL_MASK_SIZE) - 1u));
        }

        inline StampResult StampFromMasterClockSlot(size_t master_clock_id, unsigned rel_mask4 = REL_MASK4_NONE) noexcept
        {
            StampResult stamp_result;
            uint64_t now_ticks = MasterTimer48.NowTicks();
            stamp_result.NowTicks = now_ticks;
            stamp_result.SequentialClock16 = GetImmidiateDownshiftedClock16(now_ticks);
            stamp_result.RelMask4 = ResolveRelMask4_(stamp_result.SequentialClock16, static_cast<tag8_t>(rel_mask4));
            RefreshSlotEpochState_(master_clock_id, now_ticks);
            return stamp_result;
        }

        inline StampResult StampFromCurrentThread(unsigned rel_mask_4 = REL_MASK4_NONE) noexcept
        {
            size_t current_master_clock_id = EnsureOrAssignThreadIdForMasterClock();
            if (current_master_clock_id == SIZE_MAX)
            {
                StampResult stamp_result{};
                uint64_t now_ticks = MasterTimer48.NowTicks();
                stamp_result.NowTicks = now_ticks;
                stamp_result.SequentialClock16 = GetImmidiateDownshiftedClock16(now_ticks);
                stamp_result.RelMask4 = ResolveRelMask4_(stamp_result.SequentialClock16, static_cast<tag8_t>(rel_mask_4));
                return stamp_result;
            }
            return StampFromMasterClockSlot(current_master_clock_id, rel_mask_4);
        }

        //have to re-wrire 
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
            RefreshSlotEpochState_(master_clock_id, now_ticks);
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
                return NO_VAL;
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

        inline packed64_t ComposeValue32WithMasterClockStamp16(
            val32_t cell_value32,
            size_t master_clock_slot_id,
            tag8_t priority = ZERO_PRIORITY,
            tag8_t rel_mask4 = REL_MASK4_NONE,
            PackedCellLocalityTypes locality_wanted = PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode reloffset_mode_wanted = RelOffsetMode::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType data_type_wanted = PackedCellDataType::UnsignedPCellDataType
        ) noexcept
        {
            StampResult stamp_result = StampFromMasterClockSlot(master_clock_slot_id, rel_mask4);
            strl16_t strl = MakeSTRL4_t(priority, locality_wanted, stamp_result.RelMask4, reloffset_mode_wanted, PackedMode::MODE_VALUE32, data_type_wanted);
            return PackedCell64_t::ComposeValue32u_64(cell_value32, stamp_result.SequentialClock16, strl);
        }

        inline packed64_t ComposeValue32WithCurrentThreadStamp16(
            val32_t cel_value32,
            tag8_t rel_mask4,
            tag8_t priority = ZERO_PRIORITY,
            PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode rel_offset = RelOffsetMode::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType dtype = PackedCellDataType::UnsignedPCellDataType

        ) noexcept
        {
            size_t slot_id = EnsureOrAssignThreadIdForMasterClock();
            if (slot_id == SIZE_MAX)
            {
                uint64_t now_ticks = MasterTimer48.NowTicks();
                clk16_t clk16 = GetImmidiateDownshiftedClock16(now_ticks);
                tag8_t rel_mask_internal = ResolveRelMask4_(clk16, rel_mask4);
                strl16_t strl = MakeSTRL4_t(priority, locality, rel_mask_internal, rel_offset, PackedMode::MODE_VALUE32, dtype);
                return PackedCell64_t::ComposeValue32u_64(cel_value32, clk16, strl);
            }
            return ComposeValue32WithMasterClockStamp16(cel_value32, slot_id, priority, rel_mask4, locality, rel_offset, dtype);
        }

        inline packed64_t ComposeClockCell48WithMasterClock(
            uint64_t clk_value48,
            size_t master_clock_slot_id,
            tag8_t rel_mask4 = REL_MASK4_NONE,
            tag8_t priority = ZERO_PRIORITY,
            PackedCellLocalityTypes locality_wanted = PackedCellLocalityTypes::ST_PUBLISHED,
            RelOffsetMode reloffset_mode_wanted = RelOffsetMode::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType data_type_wanted = PackedCellDataType::UnsignedPCellDataType
        ) noexcept
        {
            StampResult stamp_result = StampFromMasterClockSlot(master_clock_slot_id, rel_mask4);
            strl16_t strl = MakeSTRL4_t(priority, locality_wanted, stamp_result.RelMask4, reloffset_mode_wanted, PackedMode::MODE_CLKVAL48, data_type_wanted);
            uint64_t final48 = (clk_value48 != 0) ? clk_value48 : stamp_result.NowTicks;
            return PackedCell64_t::ComposeCLK48u_64(final48, strl);
        }

        inline packed64_t RefreshPackedCellClockOnly(
            packed64_t old_packed,
            size_t master_clock_id,
            tag8_t force_rel_mask = REL_MASK4_NONE,
            std::optional<PackedCellLocalityTypes> should_owned = std::nullopt
        )
        {
            StampResult stamp = StampFromMasterClockSlot(master_clock_id, force_rel_mask);
            tag8_t priority = PackedCell64_t::ExtractPriorityFromPacked(old_packed);
            PackedCellLocalityTypes locality = PackedCell64_t::ExtractLocalityFromPacked(old_packed);
            if (should_owned)
            {
                locality = *should_owned;
            };
            PackedMode packed_mode = PackedCell64_t::ExtractModeOfPackedCellFromPacked(old_packed);
            PackedCellDataType packed_cell_dtype = PackedCell64_t::ExtractPCellDataTypeFromPacked(old_packed);
            RelOffsetMode current_reloffset = PackedCell64_t::ExtractRelOffsetFromPacked(old_packed);
            tag8_t rel_mask4 = stamp.RelMask4;
            strl16_t strl = MakeSTRL4_t(priority, locality, rel_mask4, current_reloffset, packed_mode, packed_cell_dtype);
            if (packed_mode == PackedMode::MODE_VALUE32)
            {
                val32_t cell_value32 = PackedCell64_t::ExtractValue32(old_packed);
                return PackedCell64_t::ComposeValue32u_64(cell_value32, stamp.SequentialClock16, strl);
            }
            uint64_t cell_clk48 = stamp.NowTicks;
            return PackedCell64_t::ComposeCLK48u_64(cell_clk48, strl);
        }

        inline packed64_t RefreshPackedCellClockOnlyForCurrentThread(
            packed64_t old_packed,
            tag8_t force_rel_mask4 = REL_MASK4_NONE,
            std::optional<PackedCellLocalityTypes> should_owned = std::nullopt
        ) noexcept
        {
            size_t master_clock_slot_id = EnsureOrAssignThreadIdForMasterClock();
            if (master_clock_slot_id == SIZE_MAX)
            {
                return old_packed;
            }
            return RefreshPackedCellClockOnly(old_packed, master_clock_slot_id, force_rel_mask4, should_owned);
        }

        //Integrate AtomicAdaptiveBackoff
        inline bool TouchAtomicPackedCellClock(
            std::atomic<packed64_t>& atomic_cell,
            size_t master_slot_id,
            tag8_t force_rel_mask4 = REL_MASK4_NONE,
            std::optional<PackedCellLocalityTypes> should_owned = std::nullopt
        ) noexcept
        {
            packed64_t current_cell = atomic_cell.load(MoLoad_);
            while (true)
            {
                packed64_t current_cell_updated = RefreshPackedCellClockOnly(current_cell, master_slot_id, force_rel_mask4, should_owned);
                if (atomic_cell.compare_exchange_weak(current_cell, current_cell_updated, EXsuccess_, EXfailure_))
                {
                    return true;
                }
            }
        }

        inline bool TouchAtomicPackedCellClockForCurrentThread(
            std::atomic<packed64_t>& atomic_cell,
            tag8_t force_rel_mask4 = REL_MASK4_NONE,
            std::optional<PackedCellLocalityTypes> should_owned = std::nullopt
        ) noexcept
        {
            size_t master_clock_slot_id = EnsureOrAssignThreadIdForMasterClock();
            if (master_clock_slot_id == SIZE_MAX)
            {
                return false;
            }
            return TouchAtomicPackedCellClock(atomic_cell, master_clock_slot_id, force_rel_mask4, should_owned);
            
        }

    };
    
    


}