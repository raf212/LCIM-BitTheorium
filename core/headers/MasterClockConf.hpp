#pragma once 
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <optional>
#include <cassert>
#include <limits>
#include <memory>

#include "AllocNW.hpp"
#include "PackedCell.hpp"
#include "AtomicAdaptiveBackoff.hpp" 


namespace AtomicCScompact
{

    static inline void CpuRelaxHint()
    {
    #if defined(_MSC_VER)
        YieldProcessor();
    #else   
        __asm__ __volatile__("pause" ::: "memory");
    #endif
    }

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
        AtomicAdaptiveBackoff& Adaptivebkof;
        int UsedNode = 0;
        //master clock
        std::atomic<packed64_t>* MasterClockSlotsPtr = nullptr;
        size_t MasterCLKCapacity = 0;
        std::atomic<size_t> MasterClockAlloc = 0;

        MasterClockConf(AtomicAdaptiveBackoff& ab, int used_node = REL_NODE0) noexcept:
            Adaptivebkof(ab), UsedNode(used_node)
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
            Adaptivebkof(other.Adaptivebkof), UsedNode(other.UsedNode), MasterClockSlotsPtr(other.MasterClockSlotsPtr),
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
            uint64_t now = Adaptivebkof.PublicTimer48.NowTicks();
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

        size_t RegisterMasterClockSlot(packed64_t initial = 0) noexcept
        {
            if (!MasterClockSlotsPtr || MasterCLKCapacity == 0)
            {
                return SIZE_MAX;
            }

            size_t id = MasterClockAlloc.fetch_add(1, std::memory_order_acq_rel);
            if (id >= MasterCLKCapacity)
            {
                return SIZE_MAX;
            }
            packed64_t p = PackedCell64_t::PackCLK48x_64((initial & MaskBits(CLK_B48)), ST_PUBLISHED, REL_NONE);
            MasterClockSlotsPtr[id].store(p, MoStoreSeq_);
            MasterClockSlotsPtr[id].notify_all();
            return id;
        }

        bool UpdateMasterClock(size_t mclock_id, packed64_t in_clock48) noexcept
        {
            if (!MasterClockSlotsPtr || mclock_id >= MasterCLKCapacity)
            {
                return false;
            }

            auto& slot = MasterClockSlotsPtr[mclock_id];
            packed64_t oldv = slot.load(MoLoad_);
            while (true)
            {
                strl16_t sr = PackedCell64_t::ExtractSTRL(oldv);
                tag8_t st = PackedCell64_t::StateFromSTRL(sr);
                tag8_t rel = PackedCell64_t::RelationFromSTRL(sr);
                packed64_t desired = PackedCell64_t::PackCLK48x_64((in_clock48 & MaskBits(CLK_B48)), st, rel);
                if (slot.compare_exchange_weak(oldv, desired, EXsuccess_, MoLoad_))
                {
                    slot.notify_all();
                }
                return true;
            }
            
            CpuRelaxHint();
        }

        packed64_t ReadMasterClock(size_t mclock_id) const noexcept
        {
            if (!MasterClockSlotsPtr || mclock_id >= MasterCLKCapacity)
            {
                return 0;
            }
            return MasterClockSlotsPtr[mclock_id].load(MoLoad_); 
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