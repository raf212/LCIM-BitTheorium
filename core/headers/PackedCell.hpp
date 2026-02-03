#pragma once

#include "PackedStRel.h"


namespace AtomicCScompact
{
    enum class PackedMode : int
    {
        MODE_VALUE32 = 0,
        MODE_CLKVAL48 = 1
    };
    static inline constexpr packed64_t MaskBits(unsigned n) noexcept
    {
        if (n == NO_VAL) return packed64_t(0);
        if (n >= MAX_VAL) return ~packed64_t(0);
        // produce low-n ones without shifting by >= width
        return ((packed64_t(1) << n) - 1u);                  
    }

    struct PackedCell64_t 
    {
        static inline packed64_t MakeInitialPacked(PackedMode mode) noexcept
        {
            packed64_t p = 0;
            if (mode == PackedMode::MODE_VALUE32)
            {
                p = static_cast<packed64_t>(PackV32x_64(0u, 0u, ST_IDLE, REL_NONE));
            }
            else if (mode == PackedMode::MODE_CLKVAL48)
            {
                p = static_cast<packed64_t>(PackCLK48x_64(0u, ST_IDLE, REL_NONE));
            }
            return p;
        }
        static inline packed64_t PackV32x_64(val32_t v, clk16_t clk, tag8_t st, tag8_t rel) noexcept {
            packed64_t p = (packed64_t(v) & MaskBits(VALBITS));
            p |= (packed64_t(clk) & MaskBits(CLK_B16)) << VALBITS;
            p |= (packed64_t(rel)  & MaskBits(RELBITS))  << (VALBITS + CLK_B16);
            p |= (packed64_t(st) & MaskBits(STBITS)) << (VALBITS + CLK_B16 + RELBITS);
            return p;
        }

        static inline packed64_t ComposeValue32x_64(val32_t v, clk16_t clk, strl16_t strl)
        {
            packed64_t p = (packed64_t(v) & MaskBits(VALBITS));
            p = SetCLK16InPacked<PackedMode::MODE_VALUE32>(p, clk);
            p = SetSTRLInPacked(p, strl);
            return p;
        }
        
        template <PackedMode MODE>
        static inline packed64_t SetCLK16InPacked(packed64_t p, clk16_t clk16)
        {
            static_assert(MODE == PackedMode::MODE_VALUE32, "SetCLK16InPacked only valid for MODE_VALUE32");
            constexpr packed64_t clk16_mask = (MaskBits(CLK_B16) << VALBITS);
            p &= ~clk16_mask;
            p |= (packed64_t(clk16 & MaskBits(CLK_B16)) << VALBITS);
            return p;
        }

        static inline packed64_t PackCLK48x_64(uint64_t clk, tag8_t st, tag8_t rel) noexcept {
            packed64_t p = (packed64_t(clk) & MaskBits(CLK_B48));
            p |= (packed64_t(rel)  & MaskBits(RELBITS))  << CLK_B48;
            p |= (packed64_t(st) & MaskBits(STBITS)) << (CLK_B48 + RELBITS);
            return p;
        }

        static inline packed64_t SetSTRLInPacked(packed64_t p, strl16_t strl) noexcept
        {
            constexpr packed64_t top_mask = MaskBits(STRL_B16) << (TOTAL_LOW);
            p = (p & ~top_mask) | ((packed64_t(strl & MaskBits(STRL_B16))) << (TOTAL_LOW));
            return p;
        }

        static inline strl16_t ExtractSTRL(packed64_t p) noexcept
        {
            return static_cast<strl16_t>((p >> TOTAL_LOW) & MaskBits(STRL_B16));
        }

        static inline val32_t ExtractValue32(packed64_t p) noexcept
        {
            return static_cast<val32_t>(p & MaskBits(VALBITS));
        }
        static inline clk16_t ExtractClk16(packed64_t p) noexcept
        {
            return static_cast<clk16_t>((p >> (VALBITS)) & MaskBits(CLK_B16));
        }

        static inline uint64_t ExtractClk48(packed64_t p) noexcept
        {
            return static_cast<uint64_t>(p & MaskBits(CLK_B48));
        }


        static inline tag8_t ExtractPriorityFromPacked(packed64_t p) noexcept
        {
            return ExtractPriorityFromSTRL(ExtractSTRL(p));
        }

        static inline tag8_t ExtractLocalityFromPacked(packed64_t p) noexcept
        {
            return ExtractLocalityFromSTRL(ExtractSTRL(p));
        }

        static inline tag8_t ExtractRelMaskFromPacked(packed64_t p) noexcept
        {
            return ExtractRelMaskFromSTRL(ExtractSTRL(p));
        }

        static inline tag8_t ExtractRelOffsetFromPacked(packed64_t p) noexcept
        {
            return ExtractRelOffsetFromSTRL(ExtractSTRL(p));
        }

        static inline tag8_t GetFullRelationFromPacked(packed64_t p) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t rel_mask = ExtractRelMaskFromSTRL(sr);
            tag8_t rel_offset = ExtractRelOffsetFromSTRL(sr);
            return MakeRelByte(rel_mask, rel_offset);
        }

        static inline packed64_t SetPriorityInPacked(packed64_t p, tag8_t priority)
        {
            if (priority > MAX_PRIORITY)
            {
                priority = MAX_PRIORITY; // priority will be capped if more than 15;
            }
            strl16_t sr = ExtractSTRL(p);
            tag8_t locality = ExtractLocalityFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);

            strl16_t new_strl = MakeSTRL4_t(priority, locality, rm, ro);
            return SetSTRLInPacked(p, new_strl);
            
        }

        static inline packed64_t SetLocalityInPacked(packed64_t p, tag8_t local_state) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);

            strl16_t new_strl = MakeSTRL4_t(prio, local_state, rm, ro);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t SetRelMaskInPacked(packed64_t p, tag8_t rel_mask) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t loc = ExtractLocalityFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);

            strl16_t new_strl = MakeSTRL4_t(prio, loc, rel_mask, ro);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t SetRelOffsetInPacked(packed64_t p, tag8_t rel_offset) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t loc = ExtractLocalityFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(prio, loc, rm, rel_offset);
            return SetSTRLInPacked(p, new_strl);
        }


        //old functions
        static inline tag8_t PackRel8x_t(tag8_t rel_mask_5, tag8_t priority_3) noexcept
        {
            return static_cast<tag8_t>(((priority_3 & RELATION_PRIORITY) << MASK_OF_RELBIT) | (rel_mask_5 & RELATION_MASK_5));
        }

        static inline bool RelationMatches(tag8_t slot_rel, tag8_t rel_mask) noexcept
        {
            return ((static_cast<strl16_t>(slot_rel) & static_cast<uint8_t>(rel_mask)) != 0);
        }


        static inline tag8_t StateFromSTRL(strl16_t strl) noexcept
        {
            return static_cast<tag8_t>((strl >> LN_OF_BYTE_IN_BITS) & MASK_8_BIT);
        }
        static inline tag8_t RelationFromSTRL(strl16_t strl) noexcept
        {
            return static_cast<tag8_t>(strl & MASK_8_BIT);
        }
        static inline tag8_t RelMaskBSetFromRelation(tag8_t rel_bit) noexcept
        {
            return static_cast<tag8_t>(rel_bit & RELATION_MASK_5);
        }
        static inline tag8_t PriorityFromRelation(tag8_t rel_bit) noexcept
        {
            return static_cast<tag8_t>((rel_bit >> MASK_OF_RELBIT) & RELATION_PRIORITY);
        }
        static inline tag8_t RelationMaskBSetFromSTRL(strl16_t strl) noexcept
        {
            return RelMaskBSetFromRelation(RelationFromSTRL(strl));
        }
        static inline tag8_t PriorityRelFromSTRL(strl16_t strl) noexcept
        {
            return PriorityFromRelation(RelationFromSTRL(strl));
        }



        static inline packed64_t SetState(packed64_t p, tag8_t st) noexcept
        {
            strl16_t old = ExtractSTRL(p);
            strl16_t sr = static_cast<strl16_t>((static_cast<strl16_t>(st) << LN_OF_BYTE_IN_BITS) | (old & MASK_8_BIT));
            return SetSTRLInPacked(p, sr);
        }
        static inline packed64_t SetRelation(packed64_t p, tag8_t rel) noexcept
        {
            strl16_t old = ExtractSTRL(p);
            strl16_t sr = static_cast<strl16_t>(static_cast<strl16_t>((StateFromSTRL(old)) << STBITS) | static_cast<strl16_t>(rel));
            return SetSTRLInPacked(p, sr);
        }
        static inline void UnpackV32x_64(packed64_t p, val32_t& v, clk16_t& clk, tag8_t& st, tag8_t& rel) noexcept
        {
            v = ExtractValue32(p);
            clk = ExtractClk16(p);
            strl16_t sr = ExtractSTRL(p);
            st = StateFromSTRL(sr);
            rel = RelationFromSTRL(sr);
        }
        static inline void UnpackCLK48x_64(packed64_t p, uint64_t& clk48, tag8_t& st, tag8_t& rel) noexcept
        {
            clk48 = ExtractClk48(p);
            strl16_t sr = ExtractSTRL(p);
            st = StateFromSTRL(sr);
            rel = RelationFromSTRL(sr);
        }

        template<typename T>
        static inline T AsValue(packed64_t p) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>,"T must be trivially copyable");
            static_assert(sizeof(T) <= sizeof(packed64_t), "T must fit into 64 bit");
            T out;
            std::memcpy(&out, &p, sizeof(T));
            return out;
        }

    };


}
