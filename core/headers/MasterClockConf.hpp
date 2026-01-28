#pragma once 

#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp"


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
            MasterCLKCapacity(other.MasterCLKCapacity), MasterClockAlloc(other.MasterClockAlloc.load(MoLoad_)), OwnsSlots_(other.OwnsSlots_)
        {
            other.MasterClockSlotsPtr = nullptr;
            other.MasterCLKCapacity = 0;
            other.MasterClockAlloc.store(0, MoStoreUnSeq_);
            other.OwnsSlots_ = false;
        }

        MasterClockConf& operator = (MasterClockConf&& other) noexcept = delete;


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
            //init atomics in-place
            uint64_t now = MasterTimer48.NowTicks();
            packed64_t init_p = PackedCell64_t::PackCLK48x_64((now & MaskBits(CLK_B48)),ST_IDLE, REL_NONE);

            for (size_t i = 0; i < max_slots; i++)
            {
                new (&MasterClockSlotsPtr[i]) std::atomic<packed64_t>(init_p);
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
        }

        size_t RegisterMasterClockSlot(packed64_t given_init_clk = 0, size_t m_id = SIZE_MAX) noexcept
        {
            if (!MasterClockSlotsPtr || MasterCLKCapacity == 0)
            {
                return SIZE_MAX;
            }

            auto prepare = [&](packed64_t seed)->packed64_t
            {
                if (seed == 0)
                {
                    uint64_t now = MasterTimer48.NowTicks();
                    return PackedCell64_t::PackCLK48x_64((now & MaskBits(CLK_B48)), ST_PUBLISHED, REL_NONE);
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
                packed64_t p = prepare(given_init_clk);
                MasterClockSlotsPtr[m_id].store(p, MoStoreSeq_);
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
    };
    
    


}