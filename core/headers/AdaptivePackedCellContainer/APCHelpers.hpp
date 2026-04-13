
#pragma once 
#include "PackedCell.hpp"

namespace PredictedAdaptedEncoding
{


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

    enum class APCRegionKind : uint8_t
    {
        INVALID = 0,
        FEEDFORWARD_MESSAGE = 1,
        FEEDBACKWARD_MESSAGE = 2,
        STATE = 3,
        ERROR = 4,
        EDGE_DESCRIPTOR = 5,
        WEIGHT = 6,
        AUX_PARAMETER = 7,
        FREE = 8,
        COMPLEX_COMPUTE = 9,
        GENERIC_COMPUTE = 10,
        GENERIC_STORAGE = 11
    };

    struct APCRegionBounds
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
    };
    
}
