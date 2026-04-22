#pragma once

#include "PackedStRel.h"
namespace PredictedAdaptedEncoding
{
// #define PC_MODE_V32 0u
// #define PC_MODE_CLK48 1u

    static inline constexpr packed64_t MaskBits(unsigned n) noexcept
    {
        if (n == NO_VAL) return packed64_t(0);
        if (n >= MAX_VAL) return ~packed64_t(0);
        // produce low-n ones without shifting by >= width
        return ((packed64_t(1) << n) - 1u);                  
    }

    struct PackedCell64_t 
    {
        private:
        template <typename pcdt32>
        static inline packed64_t ComposeValue32M_64_(pcdt32 value32, clk16_t clk, strl16_t strl) noexcept
        {
            constexpr PackedCellDataType expected_pcdt = BridgeOfPackedCellDataType_v<pcdt32>;
            static_assert(sizeof(pcdt32) <= (VALBITS / LN_OF_BYTE_IN_BITS), "Data Type length should be less than 32 bits\n");
            PackedMode pcmode = static_cast<PackedMode>(ExtractPCellTypeFromSTRL(strl));
            if (pcmode != PackedMode::MODE_VALUE32)
            {
                return ComposeValue32u_64(0u, 0u, MakeSTRLMode32_t(PriorityPhysics::ERROR_DEPENDENCY, PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY, 0u, RelOffsetMode32::RELOFFSET_GENERIC_VALUE, PackedCellDataType::UnsignedPCellDataType)); // assert(false)->is an option
            }
            PackedCellDataType strl_pcdt = ExtractPCellDataTypeFromSTRL(strl);
            assert(strl_pcdt == expected_pcdt && "STRL PackedCellDataType mismatch; ComposeValue32M_64_ == MODE_VALUE32\n");

            val32_t valbits32 = 0u;
            valbits32 = BitCastMaybe<val32_t>(value32);
            packed64_t p = (packed64_t(valbits32) & MaskBits(VALBITS));
            p = SetCLK16InPacked(p, clk);
            p = SetSTRLInPacked(p, strl);
            return p;
        }
        public:
        static constexpr size_t METACELL_COUNT_FIRST = 96;
        static inline packed64_t MakeInitialPacked(PackedMode mode, PackedCellDataType pcdata_type = PackedCellDataType::UnsignedPCellDataType, tag8_t rel_mask = NO_VAL, PriorityPhysics priority = PriorityPhysics::IDLE) noexcept
        {
            packed64_t p = 0;
            if (mode == PackedMode::MODE_VALUE32)
            {
                p = static_cast<packed64_t>(ComposeValue32u_64(0u, 0u, MakeSTRLMode32_t(priority, PackedCellLocalityTypes::ST_IDLE, rel_mask, RelOffsetMode32::RELOFFSET_GENERIC_VALUE, pcdata_type)));
            }
            else if (mode == PackedMode::MODE_CLKVAL48)
            {
                p = static_cast<packed64_t>(ComposeCLK48u_64(0u, MakeStrl4ForMode48_t(priority, PackedCellLocalityTypes::ST_IDLE, rel_mask, RelOffsetMode48::RELOFFSET_GENERIC_VALUE, pcdata_type)));
            }
            return p;
        }

        static inline packed64_t ComposeValue32u_64(val32_t v, clk16_t clk, strl16_t strl) noexcept
        {
            if(ExtractPCellTypeFromSTRL(strl) != PackedMode::MODE_VALUE32)
            {
                std::fputs(
                    "FATAL-> STRL defined mode MODE_CLKVAL48::Composing->PackedMode::MODE_VALUE32\n",
                    stderr
                );  
            }
            packed64_t p = (packed64_t(v) & MaskBits(VALBITS));
            p = SetCLK16InPacked(p, clk);
            p = SetSTRLInPacked(p, strl);
            return p;
        }

        template<typename PCDT>
        static inline packed64_t ComposeModeValue32Typed(
            PCDT value,
            clk16_t in_cell_clock16,
            PriorityPhysics priority = PriorityPhysics::IDLE,
            PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
            tag8_t rel_mask = REL_MASK4_NONE,
            RelOffsetMode32 reloffset = RelOffsetMode32::RELOFFSET_GENERIC_VALUE
        )
        {
            static_assert(PackedCellTypeBridge<PCDT>::IS_SUPPORTED_TYPE, "Packed Cell allowes only unsigned, int, float, char\n");
            static_assert(PackedCellTypeBridge<PCDT>::FITS_MODE_32, "Type too large for MODE_VALUE32 <= (4 byte/32 bit)\n");
            constexpr PackedCellDataType PACKED_CELL_DTYPE = PackedCellTypeBridge<PCDT>::DType;
            const strl16_t strl16_value32 = MakeSTRLMode32_t(priority, locality, rel_mask, reloffset, PACKED_CELL_DTYPE);
            return ComposeValue32M_64_<PCDT>(value, in_cell_clock16, strl16_value32);
        }

        template <typename pcdt32_48>
        static inline packed64_t ComposeCLKVal48X_64(pcdt32_48 value48, strl16_t strl) noexcept
        {
            constexpr PackedCellDataType expected_pcdt = BridgeOfPackedCellDataType_v<pcdt32_48>;
            static_assert(sizeof(pcdt32_48) <= (CLK_B48 / LN_OF_BYTE_IN_BITS), "Passed Datat Type length should be less than 48 bits\n");
            PackedMode pcmode = static_cast<PackedMode>(ExtractPCellTypeFromSTRL(strl));
            if (pcmode != PackedMode::MODE_CLKVAL48)
            {
                return ComposeCLK48u_64(0u, MakeStrl4ForMode48_t(PriorityPhysics::IDLE, PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY, 0u, RelOffsetMode48::RELOFFSET_GENERIC_VALUE, PackedCellDataType::UnsignedPCellDataType)); // assert(false)->is an option
            }
            PackedCellDataType strl_pcdt = ExtractPCellDataTypeFromSTRL(strl);
            assert(strl_pcdt == expected_pcdt && "STRL PackedCellDataType mismatch; ComposeCLKVal48X_64 == MODE_CLKVAL48");
            uint64_t clkval48 = 0ull;
            clkval48 = BitCastMaybe<uint64_t>(value48);
            packed64_t p = (packed64_t(clkval48) & MaskBits(CLK_B48));
            p = SetSTRLInPacked(p, strl);
            return p;
        }

        template <typename pcdt>
        static inline pcdt ExtractAnyPackedValueX(packed64_t p) noexcept
        {
            constexpr PackedCellDataType expected_pcdt = BridgeOfPackedCellDataType_v<pcdt>;
            strl16_t sr = ExtractSTRL(p);
            PackedMode pcmode = static_cast<PackedMode>(ExtractPCellTypeFromSTRL(sr));
            if (pcmode == PackedMode::MODE_VALUE32)
            {
                static_assert(sizeof(pcdt) <= (VALBITS / LN_OF_BYTE_IN_BITS), "Data Type length should be less than 32 bits\n");
            }
            else
            {
                static_assert(sizeof(pcdt) <= (CLK_B48 / LN_OF_BYTE_IN_BITS), "Data Type length should be less than 32 bits\n");
            }
            PackedCellDataType actual_pcdt = ExtractPCellDataTypeFromSTRL(sr);
            assert(actual_pcdt == expected_pcdt && "Packed Cell data type dosent match Requested datatype");

            if constexpr (std::is_floating_point_v<pcdt>)
            {
                if (pcmode == PackedMode::MODE_VALUE32)
                {
                    val32_t bits32 = ExtractValue32(p);
                    pcdt out;
                    out = BitCastMaybe<pcdt>(bits32);
                    return out;
                }
                else
                {
                    uint64_t low48 = (ExtractClk48(p) & MaskBits(CLK_B48)); //only correct if clk48 stored 0 extended
                    pcdt out;
                    uint64_t clk48 = low48;
                    std::memcpy(&out, &clk48, sizeof(pcdt));
                    return out;
                }
                
            }
            else if constexpr (std::is_integral_v<pcdt> && std::is_signed_v<pcdt>)
            {
                if (pcmode == PackedMode::MODE_VALUE32)
                {
                    val32_t valbits32 = ExtractValue32(p);
                    constexpr unsigned number_of_bits_valbits32 = sizeof(pcdt) * LN_OF_BYTE_IN_BITS;
                    val32_t masked_valbits32 = valbits32 & static_cast<val32_t>(MaskBits(number_of_bits_valbits32));
                    int32_t signed_valbits32;
                    if constexpr (number_of_bits_valbits32 == VALBITS)
                    {
                        signed_valbits32 = static_cast<int>(masked_valbits32);
                    }
                    else
                    {
                        int shift = VALBITS - static_cast<int>(number_of_bits_valbits32);
                        signed_valbits32 = (static_cast<int32_t>(masked_valbits32) << shift) >> shift;
                    }
                    return static_cast<pcdt>(signed_valbits32);
                }
                else
                {
                    uint64_t low48 = (ExtractClk48(p) & MaskBits(CLK_B48));
                    uint64_t masked_clk48 = (low48 & MaskBits(sizeof(pcdt) * LN_OF_BYTE_IN_BITS));
                    if constexpr (sizeof(pcdt) == LN_OF_BYTE_IN_BITS)
                    {
                        return static_cast<pcdt>(static_cast<int64_t>(masked_clk48));
                    }
                    else
                    {
                        uint64_t sign_bit = (uint64_t(1) << ((sizeof(pcdt) * 8) - 1));
                        int64_t signed64 = (masked_clk48 ^ sign_bit);
                        signed64 -=  sign_bit;
                        return static_cast<pcdt>(signed64);
                    }
                }
            }
            else
            {
                if (pcmode == PackedMode::MODE_VALUE32)
                {
                    val32_t valbits32 = ExtractValue32(p);
                    return static_cast<pcdt>(valbits32 & MaskBits(sizeof(pcdt) * LN_OF_BYTE_IN_BITS));
                }
                else
                {
                    uint64_t clk48_low = (ExtractClk48(p) & MaskBits(CLK_B48));
                    return static_cast<pcdt>(clk48_low & MaskBits(sizeof(pcdt) * LN_OF_BYTE_IN_BITS));
                }
            }
        }
        
        static inline packed64_t ComposeCLK48u_64(uint64_t clk48, strl16_t strl) noexcept
        {
            if(ExtractPCellTypeFromSTRL(strl) != PackedMode::MODE_CLKVAL48)
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

        static inline packed64_t SetCLK16InPacked(packed64_t p, clk16_t clk16)
        {
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


        static inline PriorityPhysics ExtractPriorityFromPacked(packed64_t p) noexcept
        {
            return static_cast<PriorityPhysics>(ExtractPriorityFromSTRL(ExtractSTRL(p)));
        }

        static inline PackedCellLocalityTypes ExtractLocalityFromPacked(packed64_t p) noexcept
        {
            return ExtractLocalityFromSTRL(ExtractSTRL(p));
        }

        static inline PackedMode ExtractModeOfPackedCellFromPacked(packed64_t p) noexcept
        {
            return ExtractPCellTypeFromSTRL(ExtractSTRL(p));
        }

        static inline bool IsPackedCellVal32(packed64_t p) noexcept
        {
            if (ExtractModeOfPackedCellFromPacked(p) == PackedMode::MODE_VALUE32)
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

        static inline packed64_t SetPriorityInPacked(packed64_t p, PriorityPhysics priority)
        {
            strl16_t sr = ExtractSTRL(p);
            PackedCellLocalityTypes locality = ExtractLocalityFromSTRL(sr);
            PackedMode pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(priority, locality, rm, ro, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
            
        }

        static inline packed64_t SetLocalityInPacked(packed64_t p, PackedCellLocalityTypes local_state) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            PackedMode pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(static_cast<PriorityPhysics>(prio), local_state, rm, ro, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t BlindModeSwitchOfPacked(PackedMode out_mode, packed64_t p) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            PriorityPhysics prio = static_cast<PriorityPhysics>(ExtractPriorityFromSTRL(sr));
            PackedCellLocalityTypes loc = ExtractLocalityFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            if (out_mode == PackedMode::MODE_CLKVAL48)
            {
                return ComposeCLK48u_64(ExtractClk48(p), MakeStrl4ForMode48_t(prio, loc, rm, static_cast<RelOffsetMode48>(ro), pcdata_type));
            }
            else if (out_mode == PackedMode::MODE_VALUE32)
            {
                return ComposeValue32u_64(ExtractValue32(p), ExtractClk16(p), MakeSTRLMode32_t(prio, loc, rm, static_cast<RelOffsetMode32>(ro), pcdata_type));
            }
            return SetSTRLInPacked(0u, MakeSTRLMode32_t(PriorityPhysics::ERROR_DEPENDENCY, PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY, 0u, RelOffsetMode32::RELOFFSET_GENERIC_VALUE, PackedCellDataType::UnsignedPCellDataType));
        }

        static inline packed64_t SetRelMaskInPacked(packed64_t p, tag8_t rel_mask) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            PackedCellLocalityTypes loc = ExtractLocalityFromSTRL(sr);
            PackedMode pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            PackedCellDataType pcdata_type = ExtractPCellDataTypeFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(static_cast<PriorityPhysics>(prio), loc, rel_mask, ro, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t SetRelOffsetInPacked(packed64_t p, tag8_t rel_offset) noexcept
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            PackedCellLocalityTypes loc = ExtractLocalityFromSTRL(sr);
            PackedMode pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            PackedCellDataType pcdata_type = static_cast<PackedCellDataType>(ExtractPCellDataTypeFromSTRL(sr));
            strl16_t new_strl = MakeSTRL4_t(static_cast<PriorityPhysics>(prio), loc, rm, rel_offset, pctype, pcdata_type);
            return SetSTRLInPacked(p, new_strl);
        }

        static inline packed64_t SetRelOffsetForMode32(packed64_t p, RelOffsetMode32 reloffset) noexcept
        {
            PackedMode current_packed_mode = ExtractModeOfPackedCellFromPacked(p);
            if (current_packed_mode != PackedMode::MODE_VALUE32)
            {
                return SetLocalityInPacked(p, PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY);
            }
            return SetRelOffsetInPacked(p, static_cast<tag8_t>(reloffset));
            
        }

        static inline packed64_t SetRelOffsetForMode48(packed64_t p, RelOffsetMode48 reloffset) noexcept
        {
            PackedMode current_packed_mode = ExtractModeOfPackedCellFromPacked(p);
            if (current_packed_mode != PackedMode::MODE_CLKVAL48)
            {
                return SetLocalityInPacked(p, PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY);
            }
            return SetRelOffsetInPacked(p, static_cast<tag8_t>(reloffset));
        }

        static inline packed64_t SetPCellDataTypeInPacked(packed64_t p, PackedCellDataType pc_dtype)
        {
            strl16_t sr = ExtractSTRL(p);
            tag8_t prio = ExtractPriorityFromSTRL(sr);
            PackedCellLocalityTypes loc = ExtractLocalityFromSTRL(sr);
            PackedMode pctype = ExtractPCellTypeFromSTRL(sr);
            tag8_t rm = ExtractRelMaskFromSTRL(sr);
            tag8_t ro = ExtractRelOffsetFromSTRL(sr);
            strl16_t new_strl = MakeSTRL4_t(static_cast<PriorityPhysics>(prio), loc, rm, ro, pctype, pc_dtype);
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
