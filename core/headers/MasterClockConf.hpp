#pragma once 

#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp"

#define HALF16Bit_THRESHOLD_WRAP 0x8000u

namespace AtomicCScompact
{


    class MasterClockConf
    {
    private:

        bool OwnsSlots_ = false;

        static inline size_t& ThreadLocalMasterClockID_() noexcept
        {
            static thread_local size_t id = SIZE_MAX;
            return id;
        }
        std::unique_ptr<std::atomic<packed64_t>[]> SlotLast48_{nullptr};

        packed64_t MakeInitialCellTimer48_(uint64_t now_ticks) noexcept
        {
            packed64_t init_clk48_packed = PackedCell64_t::ComposeCLK48u_64(now_ticks, MakeSTRL4_t(DEFAULT_INTERNAL_PRIORITY, ST_IDLE, REL_NONE, 
                static_cast<unsigned>(RelOffsetMode::RELOFFSET_GENERIC_VALUE), static_cast<unsigned>(PackedMode::MODE_CLKVAL48)));
        }

    public:
        Timer48& MasterTimer48;
        int UsedNode = 0;
        //master clock
        std::atomic<packed64_t>* MasterClockSlotsPtr = nullptr;
        size_t MasterCLKCapacity = 0;
        std::atomic<size_t> MasterClockAlloc = 0;

        unsigned TimerDownShift_ = 10u;
        unsigned RelShift_ = 12u;
        static constexpr unsigned REL_BUCKET_BITS__RELMASK = 4u;

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
            SlotLast48_(std::move(other.SlotLast48_)), TimerDownShift_(other.TimerDownShift_), RelShift_(other.RelShift_)
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
            if (max_slots == 0)
            {
                throw std::invalid_argument("MAX SLOTS == 0");
            }
            if (MasterClockSlotsPtr != nullptr)
            {
                return false;
            }
            size_t bytes = sizeof(std::atomic<packed64_t>) * max_slots;

            void* mem = AllocNW::AlignedAllocONnode(allignment, bytes, UsedNode);
            if (!mem)
            {
                throw std::bad_alloc();
            }
            MasterClockSlotsPtr = reinterpret_cast<std::atomic<packed64_t>*>(mem);
            SlotLast48_.reset(new std::atomic<packed64_t>[max_slots]);
            uint64_t current_now_48 = MasterTimer48.NowTicks();
            packed64_t init_clk48_packed = MakeInitialCellTimer48_(current_now_48);
            for (size_t i = 0; i < max_slots; i++)
            {
                new (&MasterClockSlotsPtr[i]) std::atomic<packed64_t>(init_clk48_packed);
                SlotLast48_[i].store(current_now_48, MoStoreSeq_);
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
                for (size_t i = 0; i < MasterCLKCapacity; i++)
                {
                    std::destroy_at(&MasterClockSlotsPtr[i]);
                }
                size_t bytes = sizeof(std::atomic<packed64_t>) * MasterCLKCapacity;
                AllocNW::FreeONNode(static_cast<void*>(MasterClockSlotsPtr), bytes);
            }
            MasterClockSlotsPtr = nullptr;
            MasterCLKCapacity = 0;
            MasterClockAlloc.store(0, MoStoreSeq_);
            OwnsSlots_ = false;
            SlotLast48_.reset(nullptr);
        }

        size_t RegisterMasterClockSlot(packed64_t given_init_clk = 0, size_t m_id = SIZE_MAX) noexcept
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
            if (m_id != SIZE_MAX)
            {
                if (m_id >= MasterCLKCapacity)
                {
                    return SIZE_MAX;
                }
                packed64_t packed_clk48 = prepare_cell_clock(given_init_clk);
                MasterClockSlotsPtr[m_id].store(packed_clk48, MoStoreSeq_);
                MasterClockSlotsPtr[m_id].notify_all();
                return m_id;
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
                stamp_result.RelMask4 = rel_mask4 & MaskBits(4);
            }
            if (master_clock_id >= MasterCLKCapacity || !SlotLast48_)
            {
                return stamp_result;
            }
            SlotLast48_[master_clock_id].store(now_ticks, MoStoreSeq_);
            return stamp_result;
            
        }

        inline std::optional<uint64_t> ComputeReconstructed48fromCLK16(size_t master_clock_id, clk16_t clk16) const noexcept
        {
            if (!SlotLast48_ || master_clock_id >= MasterCLKCapacity)
            {
                return std::nullopt;
            }
            uint64_t last48_clock = SlotLast48_[master_clock_id].load(MoLoad_);
            uint16_t base_low16 = static_cast<uint16_t>((last48_clock >> TimerDownShift_) & MaskBits(CLK_B16));
            uint16_t delta = static_cast<uint16_t>((static_cast<uint16_t>(clk16) - base_low16));
            if (delta > HALF16Bit_THRESHOLD_WRAP)
            {
                return std::nullopt;
            }
            uint64_t reconstructe_clock48 = last48_clock + (static_cast<uint64_t>(delta) << TimerDownShift_);
            return reconstructe_clock48 & MaskBits(CLK_B48);
        }


    };
    
    


}