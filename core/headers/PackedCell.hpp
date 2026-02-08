#pragma once

#include "PackedStRel.h"
#define PC_MODE_V32 0u
#define PC_MODE_CLK48 1u

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
        static inline packed64_t MakeInitialPacked(PackedMode mode, PackedCellDataType pcdata_type = PackedCellDataType::UnsignedPCellDataType) noexcept
        {
            packed64_t p = 0;
            if (mode == PackedMode::MODE_VALUE32)
            {
                p = static_cast<packed64_t>(ComposeValue32u_64(0u, 0u, MakeSTRL4_t(DEFAULT_INTERNAL_PRIORITY, ST_IDLE, 0u, 0u, PC_MODE_V32, pcdata_type)));
            }
            else if (mode == PackedMode::MODE_CLKVAL48)
            {
                p = static_cast<packed64_t>(ComposeCLK48u_64(0u, MakeSTRL4_t(DEFAULT_INTERNAL_PRIORITY, ST_IDLE, 0u, 0u, PC_MODE_CLK48, pcdata_type)));
            }
            return p;
        }

        static inline packed64_t ComposeValue32u_64(val32_t v, clk16_t clk, strl16_t strl) noexcept
        {
            if(ExtractPCellTypeFromSTRL(strl) != PC_MODE_V32)
            {
                std::fputs(
                    "FATAL-> STRL defined mode MODE_CLKVAL48::Composing->PackedMode::MODE_VALUE32\n",
                    stderr
                );  
            }
            packed64_t p = (packed64_t(v) & MaskBits(VALBITS));
            p = SetCLK16InPacked<PackedMode::MODE_VALUE32>(p, clk);
            p = SetSTRLInPacked(p, strl);
            return p;
        }

        
        static inline packed64_t ComposeCLK48u_64(uint64_t clk48, strl16_t strl) noexcept
        {
            if(ExtractPCellTypeFromSTRL(strl) != PC_MODE_CLK48)
            {
                std::fputs(
                    "FATAL-> STRL defined mode MODE_VALUE32::Composing->PackedMode::MODE_CLKVAL48\n",
                    stderr
                );  
            }
            packed64_t p = (packed64_t(clk48) & MaskBits(CLK_B48));
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

        static inline tag8_t ExtractFullRelFromPacked(packed64_t p) noexcept
        {
            strl16_t strl = ExtractSTRL(p);
            return static_cast<tag8_t>(strl & MASK_8_BIT);
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

        static inline tag8_t ExtractPCellTypeFromPacked(packed64_t p) noexcept
        {
            return ExtractPCellTypeFromSTRL(ExtractSTRL(p));
        }

        static inline bool IsPackedCellVal32(packed64_t p) noexcept
        {
            if (ExtractPCellTypeFromPacked(p) == PC_MODE_V32)
            {
                return true;
            }
            return false;
        }

        static inline tag8_t ExtractRelMaskFromPacked(packed64_t p) noexcept
        {
            return ExtractRelMaskFromSTRL(ExtractSTRL(p));
        }

        static inline tag8_t ExtractRelOffsetFromPacked(packed64_t p) noexcept
        {
            return ExtractRelOffsetFromSTRL(ExtractSTRL(p));
        }

        static inline PackedCellDataType ExtractPCellDataTypeFromPacked(packed64_t p) noexcept
        {
            return static_cast<PackedCellDataType>(ExtractPCellDataTypeFromSTRL(ExtractSTRL(p)));
        }

        static inline packed64_t SetPriorityInPacked(packed64_t p, tag8_t priority)
        {
            if (priority > MAX_PRIORITY)
            {
                priority = MAX_PRIORITY; // priority will be capped if more than 15;
            }
            strl16_t sr = ExtractSTRL(p);
            tag8_t locality = ExtractLocalityFromSTRL(sr);
            tag8_t pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(priority, locality, rm, ro, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
            
        }

        static inline packed64_t SetLocalityInPacked(packed64_t p, tag8_t local_state) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(prio, local_state, rm, ro, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t BlindModeSwitchOfPacked(PackedMode out_mode, packed64_t p) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t loc = ExtractLocalityFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            if (out_mode == PackedMode::MODE_CLKVAL48)
            {
                return ComposeCLK48u_64(ExtractClk48(p), MakeSTRL4_t(prio, loc, rm, ro, PC_MODE_CLK48, pcdata_type));
            }
            else if (out_mode == PackedMode::MODE_VALUE32)
            {
                return ComposeValue32u_64(ExtractValue32(p), ExtractClk16(p), MakeSTRL4_t(prio, loc, rm, ro, PC_MODE_V32, pcdata_type));
            }
            return SetSTRLInPacked(0u, MakeSTRL4_t(MAX_PRIORITY, ST_EXCEPTION_BIT_FAULTY, 0u, 0u, 0u, PackedCellDataType::UnsignedPCellDataType));
        }

        static inline packed64_t SetRelMaskInPacked(packed64_t p, tag8_t rel_mask) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t loc = ExtractLocalityFromSTRL(sr);
            tag8_t pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(prio, loc, rel_mask, ro, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t SetRelOffsetInPacked(packed64_t p, tag8_t rel_offset) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t loc = ExtractLocalityFromSTRL(sr);
            tag8_t pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            PackedCellDataType pcdata_type = static_cast<PackedCellDataType>(ExtractPCellDataTypeFromSTRL(sr));
            strl16_t new_strl = MakeSTRL4_t(prio, loc, rm, rel_offset, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t SetPCellDataTypeInPacked(packed64_t p, PackedCellDataType pc_dtype)
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            tag8_t loc = ExtractLocalityFromSTRL(sr);
            tag8_t pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(prio, loc, rm, ro, pctype, pc_dtype);
            return SetSTRLInPacked(p, new_strl);
        }

        //old functions

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
