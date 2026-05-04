
#pragma once 
#include <array>
#include <utility>
#include "APCStaticsFirst.hpp"

namespace PredictedAdaptedEncoding
{


    struct APCAndPagedNodeHelpers
    {
        static constexpr uint8_t HIGH_FOUR_NIBBLE = 0x0Fu;
        static constexpr uint8_t HIGH_ALL_EIGHT_NIBBLE = 0xFFu;
        static constexpr size_t SIZE_OF_APCPagedNodeRelMaskClasses = 16u;

        static inline bool INewerClock16(clk16_t candidate, clk16_t baseline) noexcept
        {
            if (candidate == baseline)
            {
                return false;
            }
            return static_cast<uint16_t>(candidate - baseline) < HALF16Bit_THRESHOLD_WRAP;
            
        }
        static APCPagedNodeRelMaskClasses ExtractPagedRelMaskFromPacked (packed64_t packed_cell) noexcept
        {
            return static_cast<APCPagedNodeRelMaskClasses>(PackedCell64_t::ExtractRelMaskFromPacked(packed_cell));
        }

        static inline bool IsCellPublishedMode32Generic (packed64_t packed_cell) noexcept
        {
            return PackedCell64_t::ExtractModeOfPackedCellFromPacked(packed_cell) == PackedMode::MODE_VALUE32 && 
                PackedCell64_t::ExtractLocalityFromPacked(packed_cell) == PackedCellLocalityTypes::ST_PUBLISHED &&
                PackedCell64_t::ExtractRelOffset32FromPacked(packed_cell) == RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
        }

        static constexpr uint32_t MakeOneAPCNodeClassReadyBit(APCPagedNodeRelMaskClasses desired_rel_class) noexcept
        {
            const uint32_t rel_class = static_cast<uint8_t>(desired_rel_class) & HIGH_FOUR_NIBBLE;
            if (rel_class == static_cast<uint8_t>(APCPagedNodeRelMaskClasses::NONE) || rel_class == static_cast<uint8_t>(APCPagedNodeRelMaskClasses::NANNULL))
            {
                return UNSIGNED_ZERO;
            }
            return (1u << rel_class);
        }


        static bool CanCellBeConsumedForThisRegion(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind) noexcept
        {
            return PackedCell64_t::ExtractLocalityFromPacked(packed_cell) == PackedCellLocalityTypes::ST_PUBLISHED &&
                ExtractPagedRelMaskFromPacked(packed_cell) == region_kind &&
                PackedCell64_t::ExtractRelOffset32FromPacked(packed_cell) == RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
        }

        static constexpr MetaIndexOfAPCNode GetOccupancyMetIndexByRegionClass(
            APCPagedNodeRelMaskClasses desired_region_class
        )noexcept
        {
            return static_cast<MetaIndexOfAPCNode>(
                static_cast<size_t>(MetaIndexOfAPCNode::REGION_OCCUPANCY_NONE) +
                (static_cast<uint8_t>(desired_region_class) & HIGH_FOUR_NIBBLE)
                );
        }

        static inline bool IsEmbededControlCell(const PackedCell64_t::AuthoritiveCellView& a_cell_view) noexcept
        {
            if (a_cell_view.CellMode == PackedMode::MODE_VALUE32 && a_cell_view.RelationOffsetForMode32.has_value())
            {
                return *a_cell_view.RelationOffsetForMode32 == RelOffsetMode32::CONTROL_SLOT;
            }
            if (a_cell_view.CellMode == PackedMode::MODE_CLKVAL48 && a_cell_view.RelationOffsetForMode48.has_value())
            {
                return *a_cell_view.RelationOffsetForMode48 == RelOffsetMode48::CONTROL_SLOT;
            }
            return false;
        }

        static inline bool IsEmbededTimerCell(const PackedCell64_t::AuthoritiveCellView& a_cell_view) noexcept
        {
            return a_cell_view.CellMode == PackedMode::MODE_CLKVAL48 && 
                a_cell_view.RelationOffsetForMode48.has_value() &&
                *a_cell_view.RelationOffsetForMode48 == RelOffsetMode48::RELOFFSET_PURE_TIMER;
        }

        static inline bool IsValidAccountingPageClass(
            APCPagedNodeRelMaskClasses page_class
        ) noexcept
        {
            return page_class != APCPagedNodeRelMaskClasses::NONE &&
                page_class != APCPagedNodeRelMaskClasses::NANNULL &&
                page_class != APCPagedNodeRelMaskClasses::FREE_SLOT &&
                page_class != APCPagedNodeRelMaskClasses::CLOCK_PURE_TIME &&
                page_class != APCPagedNodeRelMaskClasses::CONTROL_SLOT;
        }

        static inline bool DoesPublishedCellContributeToRegionOccupancy(const PackedCell64_t::AuthoritiveCellView& a_cell_view) noexcept
        {
            if (!a_cell_view.IsCellValid)
            {
                return false;
            }
            if (a_cell_view.LocalityOfCell != PackedCellLocalityTypes::ST_PUBLISHED)
            {
                return false;
            }
            if (!IsValidAccountingPageClass(a_cell_view.PageClass))
            {
                return false;
            }
            if (IsEmbededControlCell(a_cell_view) || IsEmbededTimerCell(a_cell_view))
            {
                return false;
            }
            return true;
        }

        static inline bool IsPublishedDataCellForRegion(
            const PackedCell64_t::AuthoritiveCellView& view,
            APCPagedNodeRelMaskClasses region_kind
        ) noexcept
        {
            return DoesPublishedCellContributeToRegionOccupancy(view) &&
                view.PageClass == region_kind;
        }    

        static inline MetaIndexOfAPCNode GetDesiredMetaIndexBucketForOccupancy(const PackedCell64_t::AuthoritiveCellView& a_cell_view) noexcept
        {
            if (!a_cell_view.IsCellValid || a_cell_view.LocalityOfCell == PackedCellLocalityTypes::ST_EXCEPTION_BIT_FAULTY)
            {
                return MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_FAULTY_CELLS;
            }
            switch (a_cell_view.LocalityOfCell)
            {
                case PackedCellLocalityTypes::ST_IDLE :
                    return MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_IDLE_CELLS;

                case PackedCellLocalityTypes::ST_PUBLISHED :
                    return MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_PUBLISHED_CELLS;

                case PackedCellLocalityTypes::ST_CLAIMED :
                    return MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_CLAIMED_CELLS;

                default :
                    return MetaIndexOfAPCNode::OCCUPANCY_SNAPSHOT_OF_FAULTY_CELLS;
            }
            
        } 


};
    
    struct LayoutBoundsOfSingleRelNodeClass
    {
        static constexpr uint32_t BRANCH_SENTINAL = UINT32_MAX;
        uint32_t BeginIndex = BRANCH_SENTINAL;
        uint32_t EndIndex = BRANCH_SENTINAL;
        APCPagedNodeRelMaskClasses LAYOUT_CLASS = APCPagedNodeRelMaskClasses::NANNULL;
        float InitialOrCurrentPercentage = 0u;

        void SetOrResetPercentage(uint32_t total_capacity_of_apc) noexcept
        {
            InitialOrCurrentPercentage = static_cast<float>((static_cast<float>(GetPayloadSpan()) / static_cast<float>(total_capacity_of_apc)) * 100.00);
        }

        constexpr bool IsValid(uint32_t payload_begain, uint32_t payload_end) const noexcept
        {
            return BeginIndex >= payload_begain && EndIndex >= BeginIndex && EndIndex <= payload_end && LAYOUT_CLASS!= APCPagedNodeRelMaskClasses::NANNULL;
        }

        bool IsEmpty() const noexcept
        {
            return EndIndex <= BeginIndex || LAYOUT_CLASS == APCPagedNodeRelMaskClasses::NANNULL;
        }

        uint32_t GetPayloadSpan() const noexcept
        {
            return (EndIndex > BeginIndex) ? (EndIndex - BeginIndex) : 0u;
        }

        constexpr bool CanBorrowRightFrom(const LayoutBoundsOfSingleRelNodeClass& right) const noexcept
        {
            return EndIndex == right.BeginIndex && right.GetPayloadSpan() > 0u && right.LAYOUT_CLASS != APCPagedNodeRelMaskClasses::NANNULL;
        }

        constexpr bool CanBorrowLeftFrom(const LayoutBoundsOfSingleRelNodeClass& left) const noexcept
        {
            return BeginIndex == left.EndIndex && left.GetPayloadSpan() > 0u && left.LAYOUT_CLASS != APCPagedNodeRelMaskClasses::NANNULL;
        }

        bool TryGrowRight(uint32_t amount, LayoutBoundsOfSingleRelNodeClass& right) noexcept
        {
            if (!CanBorrowRightFrom(right) || amount == 0u || right.GetPayloadSpan() < amount)
            {
                return false;
            }
            EndIndex +=amount;
            right.BeginIndex +=amount;
            return true;            
        }

        bool TryGrowLeft(uint32_t amount, LayoutBoundsOfSingleRelNodeClass& left) noexcept
        {
            if (!CanBorrowLeftFrom(left) || amount == 0u || left.GetPayloadSpan() < amount)
            {
                return false;
            }
            BeginIndex -= amount;
            left.EndIndex -= amount;
            return true;
        }

        uint32_t ClampOrNormalize(uint32_t idx) const noexcept
        {
            if (IsEmpty())
            {
                return BeginIndex;
            }
            if (idx < BeginIndex || idx >= EndIndex)
            {
                return BeginIndex;
            }
            return BeginIndex + ((idx - BeginIndex) % GetPayloadSpan());
        }

        constexpr uint32_t ComputeWantedSpanFromTotal(uint32_t total_payload_span) const noexcept
        {
            return (static_cast<uint32_t>(InitialOrCurrentPercentage) * total_payload_span) / 100u;
        }

        inline bool DoseThisIndexPhysicallyExistInThisRegion(size_t index) const noexcept
        {
            return index >= BeginIndex && index < EndIndex;
        }
        inline bool CanCellBEConsumedForThisPhysicalRegion(
            packed64_t packed_cell,
            APCPagedNodeRelMaskClasses region_kind,
            size_t idx
        ) noexcept
        {
            return DoseThisIndexPhysicallyExistInThisRegion(idx) && 
                PackedCell64_t::ExtractLocalityFromPacked(packed_cell) == PackedCellLocalityTypes::ST_PUBLISHED &&
                APCAndPagedNodeHelpers::ExtractPagedRelMaskFromPacked(packed_cell) == region_kind &&
                PackedCell64_t::ExtractRelOffset32FromPacked(packed_cell) == RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
        }

    };

    struct CompleteAPCNodeRegionsLayout
    {
        static constexpr LayoutBoundsOfSingleRelNodeClass MakeDefaultDesiredLayout(
            APCPagedNodeRelMaskClasses desired_layout_class,
            uint8_t initial_percentage
        ) noexcept
        {
            return LayoutBoundsOfSingleRelNodeClass{
                LayoutBoundsOfSingleRelNodeClass::BRANCH_SENTINAL,
                LayoutBoundsOfSingleRelNodeClass::BRANCH_SENTINAL,
                desired_layout_class,
                static_cast<float>(initial_percentage)
            };
        }

        LayoutBoundsOfSingleRelNodeClass FeedForwardLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE, FEEDFOEWARD_PERCENTAGE)};
        LayoutBoundsOfSingleRelNodeClass FeedBackwardLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE, FEEDBACKWARD_PERCENTAGE)};
        LayoutBoundsOfSingleRelNodeClass StateLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::STATE_SLOT, STATESLOT_PERCENTAGE)};
        LayoutBoundsOfSingleRelNodeClass ErrorLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::ERROR_SLOT, ERRORSLOT_PERCENTAGE)};
        LayoutBoundsOfSingleRelNodeClass EdgeDescriptorLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::EDGE_DESCRIPTOR, EDGEDESCRIPTOR_PERCENTAGE)};
        LayoutBoundsOfSingleRelNodeClass WeightLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::WEIGHT_SLOT, WEIGHTSLOT_PERCENTAGE)};
        LayoutBoundsOfSingleRelNodeClass AUXLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::AUX_SLOT, AUXSLOT_PERCENTAGE)};
        LayoutBoundsOfSingleRelNodeClass FreeLayout{MakeDefaultDesiredLayout(APCPagedNodeRelMaskClasses::FREE_SLOT, FREE_PERCENTAGE)};
        //we can add 8 more threrritically rel_mask = 4 bit ->16 classes 
        static constexpr uint8_t CURRENT_TOTAL_APC_REL_NODE_CLASSES = 8u;

        constexpr float SumOfPercentage() const noexcept
        {
            return FeedForwardLayout.InitialOrCurrentPercentage + FeedBackwardLayout.InitialOrCurrentPercentage + StateLayout.InitialOrCurrentPercentage +
                    ErrorLayout.InitialOrCurrentPercentage + EdgeDescriptorLayout.InitialOrCurrentPercentage + WeightLayout.InitialOrCurrentPercentage +
                    AUXLayout.InitialOrCurrentPercentage + FreeLayout.InitialOrCurrentPercentage;//+8more if
        }

        bool NormalizePercentagesIfNeeded() noexcept
        {
            const float sum_of_default = SumOfPercentage();
            if (sum_of_default == 100.00)
            {
                return true;
            }
            if (sum_of_default == 0.00)
            {
                FreeLayout.InitialOrCurrentPercentage = 100.00;
                return true;
            }
            auto NormalizeOne = [sum_of_default](LayoutBoundsOfSingleRelNodeClass& one) noexcept
            {
                one.InitialOrCurrentPercentage = (one.InitialOrCurrentPercentage * 100) / sum_of_default;
            };
            
            NormalizeOne(FeedForwardLayout);
            NormalizeOne(FeedBackwardLayout);
            NormalizeOne(StateLayout);
            NormalizeOne(ErrorLayout);
            NormalizeOne(EdgeDescriptorLayout);
            NormalizeOne(WeightLayout);
            NormalizeOne(AUXLayout);
            NormalizeOne(FreeLayout);
            //noramalize 8 morfe if

            float repaired_sum = SumOfPercentage();
            if (repaired_sum < 100)
            {
                FreeLayout.InitialOrCurrentPercentage = FreeLayout.InitialOrCurrentPercentage + (100 - repaired_sum);
            }
            return true;
        }

        LayoutBoundsOfSingleRelNodeClass* GetALayoutByRelMask(APCPagedNodeRelMaskClasses desired_rel_mask) noexcept
        {
            switch (desired_rel_mask)
            {
                case APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE:  return &FeedForwardLayout;
                case APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE: return &FeedBackwardLayout;
                case APCPagedNodeRelMaskClasses::STATE_SLOT:           return &StateLayout;
                case APCPagedNodeRelMaskClasses::ERROR_SLOT:           return &ErrorLayout;
                case APCPagedNodeRelMaskClasses::EDGE_DESCRIPTOR:      return &EdgeDescriptorLayout;
                case APCPagedNodeRelMaskClasses::WEIGHT_SLOT:          return &WeightLayout;
                case APCPagedNodeRelMaskClasses::AUX_SLOT:             return &AUXLayout;
                case APCPagedNodeRelMaskClasses::FREE_SLOT:            return &FreeLayout;
                default:                                               return nullptr;
            }
        }
        const LayoutBoundsOfSingleRelNodeClass* GetALayoutByRelMask(APCPagedNodeRelMaskClasses desired_rel_mask) const noexcept
        {
            switch (desired_rel_mask)
            {
                case APCPagedNodeRelMaskClasses::FEEDFORWARD_MESSAGE:  return &FeedForwardLayout;
                case APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE: return &FeedBackwardLayout;
                case APCPagedNodeRelMaskClasses::STATE_SLOT:           return &StateLayout;
                case APCPagedNodeRelMaskClasses::ERROR_SLOT:           return &ErrorLayout;
                case APCPagedNodeRelMaskClasses::EDGE_DESCRIPTOR:      return &EdgeDescriptorLayout;
                case APCPagedNodeRelMaskClasses::WEIGHT_SLOT:          return &WeightLayout;
                case APCPagedNodeRelMaskClasses::AUX_SLOT:             return &AUXLayout;
                case APCPagedNodeRelMaskClasses::FREE_SLOT:            return &FreeLayout;
                default:                                               return nullptr;
            }
        }

        std::array<LayoutBoundsOfSingleRelNodeClass*, CURRENT_TOTAL_APC_REL_NODE_CLASSES> OrderedViewsFIFO() noexcept
        {
            return {
                &FeedForwardLayout, &FeedBackwardLayout, &StateLayout, 
                &ErrorLayout, &EdgeDescriptorLayout, &WeightLayout, &AUXLayout, &FreeLayout
            };
        }
    };



    struct AcquirePairedPointerStruct
    {
        uint64_t AssembeledPtr = 0;
        size_t HeadIdx = SIZE_MAX;
        size_t TailIdx = SIZE_MAX;
        packed64_t HeadScreenshot = 0;
        packed64_t TailScreenshot = 0;
        RelOffsetMode32 Position = RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
        bool Ownership = false;
    };

    enum class PublishStatus : uint8_t
    {
        OK = 0,
        FULL = 1,
        INVALID = 2
    };

    struct PublishResult
    {
        PublishStatus ResultStatus{PublishStatus::INVALID};
        size_t Index{SIZE_MAX};
    };
    
}
