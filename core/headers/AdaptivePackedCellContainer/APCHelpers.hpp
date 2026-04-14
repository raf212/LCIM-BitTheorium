
#pragma once 
#include "PackedCell.hpp"

namespace PredictedAdaptedEncoding
{

    #define MIN_PRODUCER_BLOCK_SIZE 96
    #define MIN_REGION_SIZE 4
    #define MIN_RETIRE_BATCH_THRESHOLD 16
    #define MIN_BACKGROUND_EPOCH_MS 50
    #define INITIAL_BRANCH_SPLIT_THRESHOLD_PERCENTAGE 70
    #define MINIMUM_BRANCH_CAPACITY 128
    #define MAX_BRANCH_DEPTH 10

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
        STRUCTRUAL = 0xC,
        ///
        COMPLEX_STORAGE = 0xD,
        RESERVED_14     = 0xE,
        RESERVED_15     = 0xF
    };

    struct LayoutBoundsUint32
    {
        static constexpr uint32_t BRANCH_SENTINAL = UINT32_MAX;
        uint32_t BeginIndex = BRANCH_SENTINAL;
        uint32_t EndIndex = BRANCH_SENTINAL;

        bool IsValid(uint32_t payload_begain, uint32_t payload_end) const noexcept
        {
            return BeginIndex >= payload_begain && EndIndex >= BeginIndex && EndIndex <= payload_end;
        }

        uint32_t GetPayloadSpan() const noexcept
        {
            return (EndIndex > BeginIndex) ? (EndIndex - BeginIndex) : 0u;
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

    struct APCPagedNodeRegionBounds
    {
        size_t BeginIdx = SIZE_MAX;
        size_t EndIdx = SIZE_MAX;

        bool IsValidRegion() const noexcept
        {
            return BeginIdx != SIZE_MAX && EndIdx != SIZE_MAX && EndIdx > BeginIdx;
        }

        size_t GetRegionSpan() const noexcept
        {
            return (IsValidRegion() ? (EndIdx - BeginIdx) : 0);
        }

        uint32_t NormalizeIdxToRegion(uint32_t idx) const noexcept
        {
            if (!IsValidRegion())
            {
                return PackedCell64_t::METACELL_COUNT_FIRST;
            }

            const uint32_t begin = static_cast<uint32_t>(BeginIdx);
            const uint32_t end = static_cast<uint32_t>(EndIdx);
            const uint32_t span = end - begin;
            if (span == 0)
            {
                return begin;
            }
            if (idx < begin || idx >= end)
            {
                return begin;
            }
            
            return static_cast<uint32_t>(begin + ((idx - begin) % span));
        }
    };

    struct APCAndPagedNodeHelpers
    {
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

        static bool DoseCellBelongsToThisPagedRegion(packed64_t packed_cell, APCPagedNodeRelMaskClasses region_kind) noexcept
        {
            return ExtractPagedRelMaskFromPacked(packed_cell) == region_kind;
        }

        static packed64_t SetRelMaskForPagedNode(packed64_t packed_cell, APCPagedNodeRelMaskClasses rel_mask) noexcept
        {
            return PackedCell64_t::SetRelMaskInPacked(packed_cell, static_cast<tag8_t>(rel_mask));
        }

        static packed64_t PackValue32withAPCPageNodeClasses(val32_t value32, APCPagedNodeRelMaskClasses rel_mask, PackedCellLocalityTypes locality = PackedCellLocalityTypes::ST_PUBLISHED,
                            MasterClockConf* master_clock_ptr = nullptr ,PackedCellDataType dtype = PackedCellDataType::UnsignedPCellDataType, tag8_t priority = NO_VAL,
                            RelOffsetMode32 rel_offset = RelOffsetMode32::RELOFFSET_GENERIC_VALUE, clk16_t clock16 = NO_VAL) noexcept
        {
            packed64_t desired = 0;
            if (master_clock_ptr)
            {
                desired = master_clock_ptr->ComposeValue32WithCurrentThreadStamp16(
                    value32,
                    static_cast<tag8_t>(rel_mask),
                    priority,
                    locality,
                    rel_offset,
                    dtype
                );
            }
            else
            {
                desired = PackedCell64_t::ComposeValue32u_64(value32, clock16, 
                        MakeSTRLMode32_t(priority, locality, static_cast<tag8_t>(rel_mask), rel_offset, dtype)
                    );
            }
            return desired;
            }
    };
    


    
}
