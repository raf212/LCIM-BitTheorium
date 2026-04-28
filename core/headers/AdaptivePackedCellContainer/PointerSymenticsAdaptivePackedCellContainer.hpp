#pragma once
#include <array>
#include <utility>
#include "AdaptivePackedCellContainer/APCHElpers.hpp"
#include "AdaptivePackedCellContainer/AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"

namespace PredictedAdaptedEncoding
{
class PointerSymenticsAdaptivePackedCellContainer : public AdaptivePackedCellContainer
{
private:

public:
    PointerSymenticsAdaptivePackedCellContainer() noexcept = default;
    ~PointerSymenticsAdaptivePackedCellContainer() = default;

    PublishResult PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask = 0, int max_probs = -1) noexcept;
    bool PublishHeapPtrWithAdaptiveBackoff(void* target_publishable_ptr, uint16_t max_retries = 100);
    std::optional<AcquirePairedPointerStruct> AcquirePairedAtomicPtr(size_t probable_idx, bool claim_ownership = true, int max_claim_attempts = 256) noexcept;
    bool ReleaseAcquiredPairedPtr(const AcquirePairedPointerStruct& acquired_pair_struct, PackedCellLocalityTypes desired_locality = PackedCellLocalityTypes::ST_IDLE) noexcept;
    void RetireAcquiredPointerPair(const AcquirePairedPointerStruct& acquired_pair_struct) noexcept;
    template<typename TypePtr>
    std::optional<TypePtr> ViewPointerMemoryIfAssembeled(size_t probable_idx) noexcept;

};
}