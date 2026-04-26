
#pragma once 
#include <array>
#include <utility>
#include "PackedCell.hpp"

namespace PredictedAdaptedEncoding
{
    enum class MetaIndexOfAPCNode : size_t
    {
        //identity
        MAGIC_ID = 0,
        MANAGER_CONTROL_FLAGS = 1,
        RESERVED_2 = 2,
        //logical-node Identity
        BRANCH_ID = 3,
        LOGICAL_NODE_ID = 4,
        SHARED_ID = 5,
        SHARED_PREVIOUS_ID = 6,
        SHARED_NEXT_ID = 7,

        //runtime-controle
        BRANCH_DEPTH = 8,
        BRANCH_PRIORITY = 9,
        SEGMENT_CONF_FLAGS = 10,
        CURRENT_ACTIVE_THREADS = 11,
        OCCUPANCY_SNAPSHOT = 12,
        SPLIT_THRESHOLD_PERCENTAGE = 13,
        SEGMENT_KIND = 14,
        MAX_DEPTH = 15,

        //payload-Bounds
        PAYLOAD_END = 16,

        //timing
        LOCAL_CLOCK48 = 17,
        LAST_SPLIT_EPOCH = 18,

        //region summery
        REGION_DIR_COUNT = 19,
        REGION_SIZE = 20,
        REGION_COUNT = 21,
        READY_REL_MASK = 22,
        PRODUCER_BLOCK_SIZE = 23,
        BACKGROUND_EPOCH_ADVANCE_MS =  24,
        DEFINED_MODE_OF_CURRENT_APC = 25,
        RETIRE_BRANCH_THRASHOLD = 26,
        PRODUCER_CURSOR_PLACEMENT = 27,
        CONSUMER_CURSORE_PLACEMENT = 28,
        CURRENTLY_OWNED = 29,
        TOTAL_CAS_FAILURE_FOR_THIS_APC_BRANCH = 30,
        NODE_GROUP_SIZE = 31,
        NODE_AUX_PARAM_U32 = 32,

        //graph ports 
        FEEDFORWARD_IN_TARGET_ID = 33,
        FEEDFORWARD_OUT_TARGET_ID = 34,
        FEEDBACKWARD_IN_TARGET_ID = 35,
        FEEDBACKWARD_OUT_TARGET_ID = 36,
        LATERAL_0_TARGET_ID = 37,
        LATERAL_1_TARGET_ID = 38,
        NODE_ROLE_FLAGS_RESERVED = 39,
        LAST_ACCEPTED_FEED_FORWARD_CLOCK16 = 40,
        LAST_EMITTED_FEED_FORWARD_CLOCK16 = 41,
        LAST_ACCEPTED_FEED_BACKWARD_CLOCK16 = 42,
        LAST_EMITTED_FEED_BACKWARD_CLOCK16 = 43,
        NODE_COMPUTE_KIND = 44,

        //payload--bounds
        MESSAGE_FEEDFORWARD_BEGAIN = 45,
        MESSAGE_FEEDFORWARD_END = 46,
        MESSAGE_FEEDBACKWARD_BEGAIN = 47,
        MESSAGE_FEEDBACKWARD_END = 48,
        STATE_BEGAINING = 49,
        STATE_END = 50,
        ERROR_BEGAIN = 51,
        ERROR_END = 52,
        EDGE_DESCRIPTIOR_BEGAIN = 53,
        EDGE_DESCRIPTIOR_END = 54,
        WEIGHT_BEGIN = 55,
        WEIGHT_END = 56,
        AUX_BEGAIN = 57,
        AUX_END = 58,
        FREE_BEGAIN = 59,
        FREE_END = 60,
        //end

        EDGE_TABLE_COUNT = 61,
        WEIGHT_TABLE_COUNT = 62,


        RESERVED_63 = 63,
        EOF_APC_HEADER = 95
    };

    enum class APCPagedNodeRelMaskClasses : tag8_t
    {
        NONE = 0x0,
        FEEDFORWARD_MESSAGE  = 0x1,
        FEEDBACKWARD_MESSAGE = 0x2,
        LATERAL_MESAGE = 0x3,
        STATE_SLOT = 0x4,
        ERROR_SLOT = 0x5,
        EDGE_DESCRIPTOR = 0x6,
        WEIGHT_SLOT = 0x7,
        CONTROL_SLOT = 0x8,
        AUX_SLOT = 0x9,
        FREE_SLOT = 0xA,
        SELF_REFARANCE = 0xB,
        CLOCK_PURE_TIME = 0xC,
        RESERVED_14     = 0xD,
        COMPLEX_STORAGE = 0xE,
        NANNULL     = 0xF
    };

    struct ContainerConf
    {
        PackedMode InitialMode = PackedMode::MODE_VALUE32;
        size_t ProducerBlockSize = MIN_PRODUCER_BLOCK_SIZE;
        size_t RegionSize = MIN_REGION_SIZE;
        uint32_t RetireBatchThreshold = MIN_RETIRE_BATCH_THRESHOLD;
        uint32_t BackgroundEpochAdvanceMS = MIN_BACKGROUND_EPOCH_MS;
        bool EnableBranching = true;
        uint32_t BranchSplitThresholdPercentage = INITIAL_BRANCH_SPLIT_THRESHOLD_PERCENTAGE;
        uint32_t BranchMaxDepth = MAX_BRANCH_DEPTH;
        size_t BranchMinChildCapacity = MINIMUM_BRANCH_CAPACITY;
        uint32_t NodeGroupSize = 1u;

        enum class APCSegmentExtendOrder : uint8_t
        {
            FIFO = 0,
            PRIORITY = 1,
            RANDOM = 2
        };
    };

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
                static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(packed_cell)) == RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
        }
        
        template<typename PCDT>
        static inline bool IsMode32TypedPublishedCell(packed64_t packed_cell) noexcept
        {
            if (!IsCellPublishedMode32Generic(packed_cell))
            {
                return false;
            }
            return PackedCell64_t::ExtractPCellDataTypeFromPacked(packed_cell)  == PackedCellTypeBridge<PCDT>::DType;
        }

        static packed64_t SetRelMaskForPagedNode(packed64_t packed_cell, APCPagedNodeRelMaskClasses rel_mask) noexcept
        {
            return PackedCell64_t::SetRelMaskInPacked(packed_cell, static_cast<tag8_t>(rel_mask));
        }

        static constexpr uint32_t ReadyBitForRelClass(APCPagedNodeRelMaskClasses desired_rel_class) noexcept
        {
            const uint32_t rel_class = static_cast<uint8_t>(desired_rel_class) & HIGH_FOUR_NIBBLE;
            if (rel_class == static_cast<uint8_t>(APCPagedNodeRelMaskClasses::NONE) || rel_class == static_cast<uint8_t>(APCPagedNodeRelMaskClasses::NANNULL))
            {
                return NO_VAL;
            }
            return (1u << rel_class);
        }

        static constexpr uint32_t ReadyRelationBitMapForPackedCell(packed64_t packed_cell) noexcept
        {
            return ReadyBitForRelClass(ExtractPagedRelMaskFromPacked(packed_cell));
        }
        //will be removed
        static bool CanCellBeConsumedForThisRegion(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind) noexcept
        {
            return PackedCell64_t::ExtractLocalityFromPacked(packed_cell) == PackedCellLocalityTypes::ST_PUBLISHED &&
                ExtractPagedRelMaskFromPacked(packed_cell) == region_kind &&
                static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(packed_cell)) == RelOffsetMode32::RELOFFSET_GENERIC_VALUE;
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
                PackedCell64_t::ExtractRelOffsetFromPacked(packed_cell) == PackedCell64_t::REL_OFFSET_GENERIC_VALUE;
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

    enum class APCOccupancyQuery : uint8_t
    {
        NON_IDLE_PAYLOAD = 0,
        PUBLISHABLE_FOR_ANY_REGION = 1,
        IDLE_FOR_DESIRED_REGION = 2,
        RESERVED_OR_CLAIMED = 3
    };


    


    
}
