#pragma once

#include "PackedStRel.h"


#define ATOMIC_THRESHOLD 64u

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
        static inline packed64_t PackV32x_64(val32_t v, clk16_t clk, tag8_t st, tag8_t rel) noexcept {
            packed64_t p = (packed64_t(v) & MaskBits(VALBITS));
            p |= (packed64_t(clk) & MaskBits(CLK_B16)) << VALBITS;
            p |= (packed64_t(st)  & MaskBits(STBITS))  << (VALBITS + CLK_B16);
            p |= (packed64_t(rel) & MaskBits(RELBITS)) << (VALBITS + CLK_B16 + STBITS);
            return p;
        }

        static inline packed64_t PackCLK48x_64(uint64_t clk, tag8_t st, tag8_t rel) noexcept {
            packed64_t p = (packed64_t(clk) & MaskBits(CLK_B48));
            p |= (packed64_t(st)  & MaskBits(STBITS))  << CLK_B48;
            p |= (packed64_t(rel) & MaskBits(RELBITS)) << (CLK_B48 + STBITS);
            return p;
        }
        static inline tag8_t PackRel8x_t(tag8_t rel_mask_5, tag8_t priority_3) noexcept
        {
            return static_cast<tag8_t>(((priority_3 & RELATION_PRIORITY) << MASK_OF_RELBIT) | (rel_mask_5 & RELATION_MASK_5));
        }
        static inline strl16_t PackSTRL16x_t(tag8_t st, tag8_t rel) noexcept
        {
            return static_cast<strl16_t>((static_cast<strl16_t>(st) << STBITS) | (static_cast<strl16_t>(rel)));
        }
        static inline packed64_t SetSTRLInPacked(packed64_t p, strl16_t strl) noexcept
        {
            constexpr packed64_t top_mask = MaskBits(STRL_B16) << (TOTAL_LOW);
            p = (p & ~top_mask) | ((packed64_t(strl & MaskBits(STRL_B16))) << (TOTAL_LOW));
            return p;
        }
        static inline packed64_t MakeCommitFromPayloadV32(packed64_t payload) noexcept
        {
            val32_t v = ExtractValue32(payload);
            clk16_t clk = ExtractClk16(payload);
            tag8_t rel = RelationFromSTRL(ExtractSTRL(payload));
            return PackV32x_64(v, clk, ST_COMPLETE, rel);
        }
        static inline packed64_t MakeCommitFromPayloadCLK48(packed64_t payload) noexcept
        {
            uint64_t clk48 = ExtractClk48(payload);
            tag8_t rel = RelationFromSTRL(ExtractSTRL(payload));
            return PackCLK48x_64(clk48, ST_COMPLETE, rel);
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
        static inline strl16_t ExtractSTRL(packed64_t p) noexcept
        {
            return static_cast<strl16_t>((p >> TOTAL_LOW) & MaskBits(STRL_B16));
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

        static inline packed64_t ModifyPriorityRelInPacked(packed64_t p, tag8_t prio3) noexcept
        {
            strl16_t old = ExtractSTRL(p);
            tag8_t old_rel = RelationFromSTRL(old);
            tag8_t new_rel = PackRel8x_t(RelMaskBSetFromRelation(old_rel), prio3);
            return SetSTRLInPacked(p, PackSTRL16x_t(StateFromSTRL(old), new_rel));
        }
        static inline packed64_t ModifyRelationMaskSetInPacked(packed64_t p, tag8_t rel_mask_5) noexcept
        {
            strl16_t old = ExtractSTRL(p);
            tag8_t old_rel = RelationFromSTRL(old);
            tag8_t new_rel = PackRel8x_t(rel_mask_5, PriorityFromRelation(old_rel));
            return SetSTRLInPacked(p, PackSTRL16x_t(StateFromSTRL(old), new_rel));
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
        template<bool IsValue32>
        static inline packed64_t MakeCommittedFromPayload(packed64_t payload)
        {
            if constexpr (IsValue32)
            {
                return MakeCommitFromPayloadV32(payload);
            }
            else
            {
                return MakeCommitFromPayloadCLK48(payload);
            }
            
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
