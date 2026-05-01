#pragma once

#include "MetaAndLogicDefinationOfPackedCell.h"
namespace PredictedAdaptedEncoding
{

    // when user extracting a cell it can return UINT64_MAX as a symbole of invalid extraction method for that cell.

    struct PackedCell64_t 
    {
        static constexpr size_t METACELL_COUNT_FIRST = 96;
        static constexpr uint16_t CLOCK_16_SENTINAL = UINT16_MAX;
        static constexpr uint64_t PACKED_CELL_SENTINAL = UINT64_MAX;

        static inline bool IsCellFaulty(packed64_t packed_cell) noexcept
        {
            if (packed_cell == PACKED_CELL_SENTINAL)
            {
                return true;
            }
            return ExtractLocalityFromPacked(packed_cell) == PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY;
        }

        struct AuthoritiveCellView
        {
            packed64_t RawCell{0};
            meta16_t  InCellMeta16{0};
            PriorityPhysics Priority{PriorityPhysics::IDLE};
            PackedCellNodeAuthority NodeAuthority{PackedCellNodeAuthority::LOCAL_OR_UNDEFINED};
            PackedCellLocalityTypes LocalityOfCell{PackedCellLocalityTypes::ST_IDLE};
            PackedMode CellMode{PackedMode::MODE_VALUE32};
            APCPagedNodeRelMaskClasses PageClass{APCPagedNodeRelMaskClasses::NONE};
            std::optional<RelOffsetMode32> RelationOffsetForMode32{std::nullopt};
            std::optional<RelOffsetMode48> RelationOffsetForMode48{std::nullopt};
            PackedCellDataType CellValueDataType{PackedCellDataType::UnsignedPCellDataType};
            std::optional<clk16_t> InCellClock16{std::nullopt};
            std::optional<uint64_t> CellClock48{std::nullopt};
            std::optional<val32_t> CellValue32{std::nullopt};
            bool IsCellValid{false};
        };

        static inline packed64_t MakeFaultyCell() noexcept
        {
            return MakeACell_(
                PackedMode::MODE_VALUE32,
                UINT32_MAX,
                UINT16_MAX,
                PriorityPhysics::ERROR_DEPENDENCY,
                PackedCellNodeAuthority::LOCAL_OR_UNDEFINED,
                PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY
            );
        }

        static inline packed64_t ComposeValue32u_64(val32_t in_cell_value, clk16_t clock16, meta16_t meta16) noexcept
        {
            if(static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(meta16)) != PackedMode::MODE_VALUE32)
            {
                return MakeFaultyCell();
            }
            packed64_t packed_cell = (packed64_t(in_cell_value) & MaskBits(VALBITS));
            packed_cell = SetCLK16InPacked(packed_cell, clock16);
            packed_cell = SetMETA16InPacked(packed_cell, meta16);
            return packed_cell;
        }

        static inline packed64_t ComposeCLK48u_64(uint64_t clockor_value48, meta16_t meta16) noexcept
        {
            if(static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(meta16)) != PackedMode::MODE_CLKVAL48)
            {
                return MakeFaultyCell();
            }
            packed64_t packed_cell = (packed64_t(clockor_value48) & MaskBits(CLK_B48));
            packed_cell = SetMETA16InPacked(packed_cell, meta16);
            return packed_cell;
        }

        static inline packed64_t SetMETA16InPacked(packed64_t packed_cell, meta16_t meta16) noexcept
        {
            constexpr packed64_t top_48_bit_mask = MaskBits(META16_B16) << TOTAL_LOW;
            packed_cell &= ~top_48_bit_mask;
            packed_cell |= (packed64_t(meta16) & MaskBits(META16_B16)) << TOTAL_LOW;
            return packed_cell;
        }

        static inline packed64_t MakeInitialPacked(
            PackedMode cell_mode,
            PriorityPhysics cell_priority = PriorityPhysics::IDLE,
            PackedCellLocalityTypes cell_locality = PackedCellLocalityTypes::ST_IDLE,
            APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::FREE_SLOT,
            PackedCellDataType cell_data_type = PackedCellDataType::UnsignedPCellDataType
        ) noexcept
        {
            return MakeACell_(
                cell_mode,
                NO_VAL,
                NO_VAL,
                cell_priority,
                PackedCellNodeAuthority::LOCAL_OR_UNDEFINED,
                cell_locality,
                page_class,
                NO_VAL,
                cell_data_type
            );
        }

        template <typename PCDT>
        static inline packed64_t ComposeTypedModeValue32Cell(PCDT value32, clk16_t clock16, meta16_t meta16) noexcept
        {
            static_assert(PackedCellTypeBridge<PCDT>::IS_SUPPORTED_TYPE, "Unsupported Cell type");
            static_assert(PackedCellTypeBridge<PCDT>::FITS_MODE_32, "Value type is too large to fite MODE_VALUE32");
            if (static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(meta16)) != PackedMode::MODE_VALUE32)
            {
                return MakeFaultyCell();
            }
            if (static_cast<PackedCellDataType>(ExtractValueDataTypeFromMETA16_U_(meta16))!= BridgeOfPackedCellDataType_v<PCDT> )
            {
                return MakeFaultyCell();
            }
            uint64_t value_casted_bit = BitCastMaybe<val32_t>(value32);
            return ComposeValue32u_64(value_casted_bit, clock16, meta16);
            
        }

        template <typename PCDT>
        static inline packed64_t ComposeTypedModeValue48Cell(PCDT value_clock48, meta16_t meta16) noexcept
        {
            static_assert(PackedCellTypeBridge<PCDT>::IS_SUPPORTED_TYPE, "Unsupported Cell type");
            static_assert(PackedCellTypeBridge<PCDT>::FITS_MODE_48, "Value type is too large to fite MODE_VALUE32");
            if (static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(meta16)) != PackedMode::MODE_CLKVAL48)
            {
                return MakeFaultyCell();
            }
            if (static_cast<PackedCellDataType>(ExtractValueDataTypeFromMETA16_U_(meta16)) != BridgeOfPackedCellDataType_v<PCDT> )
            {
                return MakeFaultyCell();
            }
            uint64_t value_casted_bit = BitCastMaybe<uint64_t>(value_clock48);
            return ComposeCLK48u_64(value_casted_bit & MaskBits(CLK_B48), meta16);
            
        }

        static inline packed64_t SetCLK16InPacked(packed64_t packed_cell, clk16_t clk16)
        {
            constexpr packed64_t clk16_mask = (MaskBits(CLK_B16) << VALBITS);
            packed_cell &= ~clk16_mask;
            packed_cell |= (packed64_t(clk16 & MaskBits(CLK_B16)) << VALBITS);
            return packed_cell;
        }

        static inline meta16_t ExtractMeta16fromPackedCell(packed64_t packed_cell) noexcept
        {
            return static_cast<meta16_t>((packed_cell >> TOTAL_LOW) & MaskBits(META16_B16));
        }


        static inline PackedMode ExtractModeOfPackedCellFromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline bool IsPackedCellVal32(packed64_t packed_cell) noexcept
        {
            if (ExtractModeOfPackedCellFromPacked(packed_cell) == PackedMode::MODE_VALUE32)
            {
                return true;
            }
            return false;
        }

        static inline val32_t ExtractValue32(packed64_t packed_cell) noexcept
        {
            if (!IsPackedCellVal32(packed_cell))
            {
                return UINT32_MAX;
            }
            return static_cast<val32_t>(packed_cell & MaskBits(VALBITS));
        }

        static inline clk16_t ExtractClk16(packed64_t packed_cell) noexcept
        {
            if (!IsPackedCellVal32(packed_cell))
            {
                return CLOCK_16_SENTINAL;
            }
            return static_cast<clk16_t>((packed_cell >> (VALBITS)) & MaskBits(CLK_B16));
        }

        static inline uint64_t ExtractClk48(packed64_t packed_cell) noexcept
        {
            if (IsPackedCellVal32(packed_cell))
            {
                return PACKED_CELL_SENTINAL;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          
            }
            return static_cast<uint64_t>(packed_cell & MaskBits(CLK_B48));
        }

        static inline PriorityPhysics ExtractPriorityFromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<PriorityPhysics>(ExtractPriorityFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline PackedCellLocalityTypes ExtractLocalityFromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<PackedCellLocalityTypes>(ExtractLocalityFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline PackedCellNodeAuthority ExtractNodeAuthorityFromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<PackedCellNodeAuthority>(ExtractCellLocalNodeAuthotityFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline APCPagedNodeRelMaskClasses ExtractRelMaskFromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<APCPagedNodeRelMaskClasses>(ExtractRelMaskFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline RelOffsetMode32 ExtractRelOffset32FromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<RelOffsetMode32>(ExtractRelOffsetFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline RelOffsetMode48 ExtractRelOffset48FromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<RelOffsetMode48>(ExtractRelOffsetFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline PackedCellDataType ExtractPCellDataTypeFromPacked(packed64_t packed_cell) noexcept
        {
            return static_cast<PackedCellDataType>(ExtractValueDataTypeFromMETA16_U_(ExtractMeta16fromPackedCell(packed_cell)));
        }

        static inline bool HasHigherPriorityBetweenCellA_B(packed64_t cell_A, packed64_t cell_B) noexcept
        {
            return ExtractPriorityFromPacked(cell_A) > ExtractPriorityFromPacked(cell_B);
        }


        static inline AuthoritiveCellView InspectPackedCell(packed64_t packed_cell) noexcept
        {
            const meta16_t meta16 = ExtractMeta16fromPackedCell(packed_cell);
            AuthoritiveCellView out_packed_cell_view{};
            
            out_packed_cell_view.RawCell = packed_cell;
            out_packed_cell_view.InCellMeta16 = meta16;
            out_packed_cell_view.Priority = static_cast<PriorityPhysics>(ExtractPriorityFromMETA16_U_(meta16));
            out_packed_cell_view.NodeAuthority =  static_cast<PackedCellNodeAuthority>(ExtractCellLocalNodeAuthotityFromMETA16_U_(meta16));
            out_packed_cell_view.LocalityOfCell = static_cast<PackedCellLocalityTypes>(ExtractLocalityFromMETA16_U_(meta16));
            out_packed_cell_view.CellMode = static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(meta16));
            out_packed_cell_view.PageClass = static_cast<APCPagedNodeRelMaskClasses>(ExtractRelMaskFromMETA16_U_(meta16));
            if (IsPackedCellVal32(packed_cell))
            {
                out_packed_cell_view.RelationOffsetForMode32 = static_cast<RelOffsetMode32>(ExtractRelOffsetFromMETA16_U_(meta16));
                out_packed_cell_view.InCellClock16 = ExtractClk16(packed_cell);
                out_packed_cell_view.CellValue32 = ExtractValue32(packed_cell);
            }
            else
            {
                out_packed_cell_view.RelationOffsetForMode48 = static_cast<RelOffsetMode48>(ExtractRelOffsetFromMETA16_U_(meta16));
                out_packed_cell_view.CellClock48 = ExtractClk48(packed_cell);
            }
            out_packed_cell_view.CellValueDataType = static_cast<PackedCellDataType>(ExtractValueDataTypeFromMETA16_U_(meta16));
            if (IsCellFaulty(packed_cell))
            {
                out_packed_cell_view.IsCellValid = false;
            }
            else
            {
                out_packed_cell_view.IsCellValid = true;
            } 
            
            return out_packed_cell_view;      
        }

        static inline constexpr meta16_t SetPriorityInMETA16(
            meta16_t meta16,
            PriorityPhysics priority
        ) noexcept
        {
            return SetIndicatedMetaInMeta16(
                meta16,
                PRIORITY_SHIFT,
                PRIORITY_MASK,
                static_cast<tag8_t>(priority)
            );
        }

        static inline constexpr meta16_t SetNodeAuthorityInMETA16(
            meta16_t meta16,
            PackedCellNodeAuthority node_authority
        ) noexcept
        {
            return SetIndicatedMetaInMeta16(
                meta16,
                NODE_AUTH_SHIFT,
                NODE_AUTH_MASK,
                static_cast<tag8_t>(node_authority)
            );
        }

        static inline constexpr meta16_t SetLocalityInMETA16(
            meta16_t meta16,
            PackedCellLocalityTypes locality
        ) noexcept
        {
            return SetIndicatedMetaInMeta16(
                meta16,
                LOCALITY_SHIFT,
                LOCALITY_MASK,
                static_cast<tag8_t>(locality)
            );
        }

        static inline constexpr meta16_t SetCellModeInMETA16(
            meta16_t meta16,
            PackedMode cell_mode
        ) noexcept
        {
            return SetIndicatedMetaInMeta16(
                meta16,
                CELL_MODE_SHIFT,
                CELL_MODE_MASK,
                static_cast<tag8_t>(cell_mode)
            );
        }

        static inline constexpr meta16_t SetPageClassInMETA16(
            meta16_t meta16,
            APCPagedNodeRelMaskClasses page_class
        ) noexcept
        {
            return SetIndicatedMetaInMeta16(
                meta16,
                RELMASK_SHIFT,
                RELMASK_MASK,
                static_cast<tag8_t>(page_class)
            );
        }


        static inline constexpr meta16_t SetRelOffsetInMETA16_U(
            meta16_t meta16,
            tag8_t rel_offset
        ) noexcept
        {
            return SetIndicatedMetaInMeta16(
                meta16,
                RELOFFSET_SHIFT,
                RELOFFSET_MASK,
                rel_offset
            );
        }

        static inline constexpr meta16_t SetRelOffset32InMETA16(
            meta16_t meta16,
            RelOffsetMode32 rel_offset_32
        ) noexcept
        {
            if (static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(meta16)) != PackedMode::MODE_VALUE32)
            {
                return meta16;
            }
            meta16 = SetCellModeInMETA16(meta16, PackedMode::MODE_VALUE32);
            return SetRelOffsetInMETA16_U(
                meta16,
                static_cast<tag8_t>(rel_offset_32)
            );
        }

        static inline constexpr meta16_t SetRelOffset48InMETA16(
            meta16_t meta16,
            RelOffsetMode48 rel_offset_48
        ) noexcept
        {
            if (static_cast<PackedMode>(ExtractCellModeFromMETA16_U_(meta16)) != PackedMode::MODE_CLKVAL48)
            {
                return meta16;
            }
            meta16 = SetCellModeInMETA16(meta16, PackedMode::MODE_CLKVAL48);
            return SetRelOffsetInMETA16_U(
                meta16,
                static_cast<tag8_t>(rel_offset_48)
            );
        }

        static inline constexpr meta16_t SetCellDataTypeInMETA16(
            meta16_t meta16,
            PackedCellDataType cell_data_type
        ) noexcept
        {
            return SetIndicatedMetaInMeta16(
                meta16,
                PCELL_DETATYPE_SHIFT,
                PCELL_DATATYPE_MASK,
                static_cast<tag8_t>(cell_data_type)
            );
        }

        static inline packed64_t SetPriorityInPacked(packed64_t packed_cell, PriorityPhysics priority) noexcept
        {
            const meta16_t new_desired_meta = SetPriorityInMETA16(ExtractMeta16fromPackedCell(packed_cell), priority);
            return SetMETA16InPacked(packed_cell, new_desired_meta);
        }

        static inline packed64_t SetLocalityInPacked(packed64_t packed_cell, PackedCellLocalityTypes local_state) noexcept
        {
            const meta16_t new_desired_meta = SetLocalityInMETA16(ExtractMeta16fromPackedCell(packed_cell), local_state);
            return SetMETA16InPacked(packed_cell, new_desired_meta);
        }


        static inline packed64_t SetPageClassInPacked(packed64_t packed_cell, APCPagedNodeRelMaskClasses page_class) noexcept
        {
            const meta16_t new_desired_meta = SetPageClassInMETA16(ExtractMeta16fromPackedCell(packed_cell), page_class);
            return SetMETA16InPacked(packed_cell, new_desired_meta);
        }

        static inline packed64_t SetRelOffsetForMode32(packed64_t packed_cell, RelOffsetMode32 reloffset) noexcept
        {
            const meta16_t new_desired_meta = SetRelOffset32InMETA16(ExtractMeta16fromPackedCell(packed_cell), reloffset);
            return SetMETA16InPacked(packed_cell, new_desired_meta);
        }

        static inline packed64_t SetRelOffsetForMode48(packed64_t packed_cell, RelOffsetMode48 reloffset) noexcept
        {
            const meta16_t new_desired_meta = SetRelOffset48InMETA16(ExtractMeta16fromPackedCell(packed_cell), reloffset);
            return SetMETA16InPacked(packed_cell, new_desired_meta);
        }

        static inline packed64_t SetPCellDataTypeInPacked(packed64_t packed_cell, PackedCellDataType cell_data_type)
        {
            const meta16_t new_desired_meta = SetCellDataTypeInMETA16(ExtractMeta16fromPackedCell(packed_cell), cell_data_type);
            return SetMETA16InPacked(packed_cell, new_desired_meta);
        }

        static inline constexpr meta16_t MakeInCellMetaForMode_32t(
            PriorityPhysics priority = PriorityPhysics::DEFAULT_PRIORITY, 
            PackedCellNodeAuthority authority = PackedCellNodeAuthority::LOCAL_OR_UNDEFINED,
            PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_IDLE,
            APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::FREE_SLOT,
            RelOffsetMode32 rel_offset_32 = RelOffsetMode32::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType cell_data_type = PackedCellDataType::UnsignedPCellDataType
        ) noexcept
        {
            return MakeInCellMetaFromUnsigned_16t_(
                static_cast<tag8_t>(priority),
                static_cast<tag8_t>(authority),
                static_cast<tag8_t>(locality),
                static_cast<tag8_t>(page_class),
                static_cast<tag8_t>(rel_offset_32),
                static_cast<tag8_t>(PackedMode::MODE_VALUE32),
                static_cast<tag8_t>(cell_data_type)
            );
        }

        static inline constexpr meta16_t MakeInCellMetaForMode_48t(
            PriorityPhysics priority = PriorityPhysics::DEFAULT_PRIORITY, 
            PackedCellNodeAuthority authority = PackedCellNodeAuthority::LOCAL_OR_UNDEFINED,
            PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_IDLE,
            APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::FREE_SLOT,
            RelOffsetMode48 rel_offset_48 = RelOffsetMode48::RELOFFSET_GENERIC_VALUE,
            PackedCellDataType cell_data_type = PackedCellDataType::UnsignedPCellDataType
        ) noexcept
        {
            return MakeInCellMetaFromUnsigned_16t_(
                static_cast<tag8_t>(priority),
                static_cast<tag8_t>(authority),
                static_cast<tag8_t>(locality),
                static_cast<tag8_t>(page_class),
                static_cast<tag8_t>(rel_offset_48),
                static_cast<tag8_t>(PackedMode::MODE_CLKVAL48),
                static_cast<tag8_t>(cell_data_type)
            );
        }

        template <typename PCDT>
        static inline std::optional<PCDT> ExtractAnyPackedValueX(packed64_t packed_cell)
        {
            constexpr PackedCellDataType expected_dtype = BridgeOfPackedCellDataType_v<PCDT>;
            if(ExtractPCellDataTypeFromPacked(packed_cell) != expected_dtype)
            {
                return std::nullopt;
            }
            if (IsPackedCellVal32(packed_cell))
            {
                if (sizeof(PCDT) > sizeof(val32_t))
                {
                    return std::nullopt;
                }
                const val32_t value_bits32 = ExtractValue32(packed_cell);
                return BitCastMaybe<PCDT>(value_bits32);
            }

            if (sizeof(PCDT) > SIZE_OF_MODE_48)
            {
                return std::nullopt;
            }
            uint64_t value_bits_48 = ExtractClk48(packed_cell);
            return BitCastMaybe<PCDT>(value_bits_48);
        }
        

    private:

        static inline constexpr meta16_t ClearIndicatedMeta16Field_(
            meta16_t meta16,
            unsigned shift,
            tag8_t mask
        ) noexcept
        {
            return static_cast<meta16_t>(
                meta16 & ~static_cast<meta16_t>(
                    static_cast<meta16_t>(mask) << shift
                )
            );
        }

        static inline constexpr meta16_t SetIndicatedMetaInMeta16(
            meta16_t meta16,
            unsigned shift,
            tag8_t mask,
            tag8_t value
        ) noexcept
        {
            const meta16_t cleared_indicated = ClearIndicatedMeta16Field_(
                meta16, shift, mask
            );
            const meta16_t only_inserted_meta16 = static_cast<meta16_t>(
                static_cast<meta16_t>(value & mask) << shift
            );
            return static_cast<meta16_t>(cleared_indicated | only_inserted_meta16);
        }

        static inline packed64_t MakeACell_(
            PackedMode cell_mode,
            uint64_t cell_value = NO_VAL,
            clk16_t clock16 = NO_VAL,
            PriorityPhysics cell_priority = PriorityPhysics::DEFAULT_PRIORITY,
            PackedCellNodeAuthority node_authority = PackedCellNodeAuthority::LOCAL_OR_UNDEFINED,
            PackedCellLocalityTypes cell_locality = PackedCellLocalityTypes::ST_IDLE, 
            APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::FREE_SLOT,
            tag8_t rel_offset = 0,
            PackedCellDataType cell_data_type = PackedCellDataType::UnsignedPCellDataType
        ) noexcept
        {
            if (cell_mode == PackedMode::MODE_VALUE32)
            {
                const meta16_t meta16_for_mode32 = MakeInCellMetaForMode_32t(
                    cell_priority,
                    node_authority,
                    cell_locality,
                    page_class,
                    static_cast<RelOffsetMode32>(rel_offset),
                    cell_data_type
                );

                return ComposeValue32u_64(
                    static_cast<val32_t>(cell_value),
                    clock16,
                    meta16_for_mode32
                );
            }
            else
            {
                const meta16_t meta16_for_mode48 = MakeInCellMetaForMode_48t(
                    cell_priority,
                    node_authority,
                    cell_locality,
                    page_class,
                    static_cast<RelOffsetMode48>(rel_offset),
                    cell_data_type
                );

                return ComposeCLK48u_64(cell_value, meta16_for_mode48);
            }
        }

        static inline constexpr meta16_t MakeInCellMetaForAny_(
            PackedMode mode_of_cell ,
            PriorityPhysics priority = PriorityPhysics::DEFAULT_PRIORITY, 
            PackedCellNodeAuthority authority = PackedCellNodeAuthority::LOCAL_OR_UNDEFINED,
            PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_IDLE,
            APCPagedNodeRelMaskClasses page_class = APCPagedNodeRelMaskClasses::FREE_SLOT,
            tag8_t rel_offset_any = static_cast<tag8_t>(RelOffsetMode32::RELOFFSET_GENERIC_VALUE),
            PackedCellDataType cell_data_type = PackedCellDataType::UnsignedPCellDataType
        ) noexcept
        {
            return MakeInCellMetaFromUnsigned_16t_(
                static_cast<tag8_t>(priority),
                static_cast<tag8_t>(authority),
                static_cast<tag8_t>(locality),
                static_cast<tag8_t>(page_class),
                static_cast<tag8_t>(rel_offset_any),
                static_cast<tag8_t>(mode_of_cell),
                static_cast<tag8_t>(cell_data_type)
            );
        }


        static inline constexpr meta16_t MakeInCellMetaFromUnsigned_16t_(
            tag8_t priority, tag8_t node_authority,
            tag8_t locality, tag8_t rel_mask, 
            tag8_t rel_offset, tag8_t pc_type, 
            tag8_t pc_datatype
        ) noexcept
        {

            meta16_t cell_priority = static_cast<meta16_t>(static_cast<tag8_t>(priority) & PRIORITY_MASK);
            meta16_t cell_authority = static_cast<meta16_t>(static_cast<tag8_t>(node_authority) & NODE_AUTH_MASK); 
            meta16_t cell_locality = static_cast<meta16_t>(static_cast<tag8_t>(locality) & LOCALITY_MASK);
            meta16_t cell_mode = static_cast<meta16_t>(static_cast<tag8_t>(pc_type) & CELL_MODE_MASK);
            meta16_t relation_mask = static_cast<meta16_t>(rel_mask & RELMASK_MASK);
            meta16_t relation_offset = static_cast<meta16_t>(static_cast<tag8_t>(rel_offset) & RELOFFSET_MASK);
            meta16_t cell_data_type = static_cast<meta16_t>(static_cast<unsigned>(pc_datatype) & PCELL_DATATYPE_MASK);

            meta16_t cell_meta = static_cast<meta16_t>(
                (cell_priority  << (PRIORITY_SHIFT))
                | (cell_authority << (NODE_AUTH_SHIFT))
                | (cell_locality << LOCALITY_SHIFT)
                | (cell_mode << CELL_MODE_SHIFT)
                | (relation_mask << RELMASK_SHIFT)
                | (relation_offset << RELOFFSET_SHIFT)
                | cell_data_type
            );
            return cell_meta;
        }

        static inline constexpr tag8_t ExtractPriorityFromMETA16_U_(meta16_t meta16) noexcept
        {
            return static_cast<tag8_t>((meta16 >> PRIORITY_SHIFT) & PRIORITY_MASK);
        }

        static inline constexpr tag8_t ExtractCellLocalNodeAuthotityFromMETA16_U_(meta16_t meta16) noexcept
        {
            return static_cast<tag8_t>((meta16 >> NODE_AUTH_SHIFT ) & NODE_AUTH_MASK);
        }
        
        static inline constexpr tag8_t ExtractLocalityFromMETA16_U_(meta16_t meta16) noexcept
        {
            return static_cast<tag8_t>((meta16 >> LOCALITY_SHIFT) & LOCALITY_MASK);
        }

        static inline constexpr tag8_t ExtractCellModeFromMETA16_U_(meta16_t meta16) noexcept
        {
            return static_cast<tag8_t>((meta16 >> CELL_MODE_SHIFT) & CELL_MODE_MASK);
        }

        static inline constexpr tag8_t ExtractRelMaskFromMETA16_U_(meta16_t meta16) noexcept
        {
            return static_cast<tag8_t>((meta16 >> RELMASK_SHIFT) & RELMASK_MASK);
        }

        static inline constexpr tag8_t ExtractRelOffsetFromMETA16_U_(meta16_t meta16) noexcept
        {
            return static_cast<tag8_t>((meta16 >> RELOFFSET_SHIFT) & RELOFFSET_MASK);
        }

        static inline constexpr tag8_t ExtractValueDataTypeFromMETA16_U_(meta16_t meta16) noexcept
        {
            return static_cast<tag8_t>((meta16 >> PCELL_DETATYPE_SHIFT) & PCELL_DATATYPE_MASK);
        }


    };


}
