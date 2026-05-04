#pragma once
#include <array>
#include <utility>
#include "AdaptivePackedCellContainer/APCHElpers.hpp"
#include "AdaptivePackedCellContainer/AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

namespace PredictedAdaptedEncoding
{
class APCSegmentsCausalCordinator : public AdaptivePackedCellContainer
{
private:
    static MetaIndexOfAPCNode AcceptedClockIndex_(APCPagedNodeRelMaskClasses region) noexcept
    {
        return region == APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE
            ? MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_BACKWARD_CLOCK16
            : MetaIndexOfAPCNode::LAST_ACCEPTED_FEED_FORWARD_CLOCK16;
    }

    static MetaIndexOfAPCNode EmittedClockIndex_(APCPagedNodeRelMaskClasses region) noexcept
    {
        return region == APCPagedNodeRelMaskClasses::FEEDBACKWARD_MESSAGE
            ? MetaIndexOfAPCNode::LAST_EMITTED_FEED_BACKWARD_CLOCK16
            : MetaIndexOfAPCNode::LAST_EMITTED_FEED_FORWARD_CLOCK16;
    }

    bool TryAdvanceClock_(MetaIndexOfAPCNode idx, clk16_t candidate) noexcept
    {

        while (true)
        {
            const uint32_t current32 = ReadMetaCellValue32(idx);
            const clk16_t current = static_cast<clk16_t>(current32);

            if (current32 != UNSIGNED_ZERO &&
                !APCAndPagedNodeHelpers::INewerClock16(candidate, current))
            {
                return false;
            }

            if (JustUpdateValueOfMeta32(idx, current32, static_cast<uint32_t>(candidate), false))
            {
                return true;
            }
        }
    }

public:
    APCSegmentsCausalCordinator() noexcept = default;
    ~APCSegmentsCausalCordinator() = default;

    bool AcceptCausalCell(APCPagedNodeRelMaskClasses region, packed64_t cell) noexcept
    {
        const clk16_t incoming = PackedCell64_t::ExtractClk16(cell);
        return TryAdvanceClock_(AcceptedClockIndex_(region), incoming);
    }

    bool MarkEmittedCausalCell(APCPagedNodeRelMaskClasses region, packed64_t cell) noexcept
    {
        const clk16_t emitted = PackedCell64_t::ExtractClk16(cell);
        return TryAdvanceClock_(EmittedClockIndex_(region), emitted);
    }

    bool PublishCausal(
        APCPagedNodeRelMaskClasses region,
        packed64_t cell,
        std::atomic<uint64_t>* growth_counter = nullptr
    ) noexcept
    {
        cell = PackedCell64_t::SetPageClassInPacked(cell, region);

        if (TryPublishRegionalSharedGrowthOnce(region, cell, growth_counter))
        {
            MarkEmittedCausalCell(region, cell);
            return true;
        }
        return false;
    }

    std::optional<packed64_t> ConsumeCausal(
        APCPagedNodeRelMaskClasses region,
        size_t& scan_cursor,
        std::atomic<uint64_t>* older_counter = nullptr,
        bool drop_older = false
    ) noexcept
    {
        while (true)
        {
            auto maybe = ConsumeCellByRegionMaskTraverseStartFromThisAPC(region, scan_cursor);
            if (!maybe) return std::nullopt;

            if (AcceptCausalCell(region, *maybe))
            {
                return maybe;
            }

            if (older_counter)
            {
                older_counter->fetch_add(1, std::memory_order_relaxed);
            }

            if (!drop_older)
            {
                return maybe;
            }
        }
    }

    uint32_t CountPublishedInRegion(APCPagedNodeRelMaskClasses region) noexcept
    {
        uint32_t count = 0;
        for (size_t i = PayloadBegin(); i < GetTotalCapacityForThisAPC(); ++i)
        {
            packed64_t cell = BackingPtr[i].load(MoLoad_);
            if (APCAndPagedNodeHelpers::CanCellBeConsumedForThisRegion(cell, region))
            {
                ++count;
            }
        }
        return count;
    }

    bool HasAnyPublishedInChain(APCPagedNodeRelMaskClasses region) noexcept
    {
        AdaptivePackedCellContainer* current = FindSharedRootOrThis();
        while (current)
        {
            auto* node = static_cast<APCSegmentsCausalCordinator*>(current);
            if (node->CountPublishedInRegion(region) > 0)
            {
                return true;
            }
            current = current->GetNextSharedSegment();
        }
        return false;
    }
};
}