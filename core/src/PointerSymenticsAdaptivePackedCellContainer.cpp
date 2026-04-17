#include "AdaptivePackedCellContainer.hpp"
#include "PackedCellContainerManager.hpp"
#include <iostream>

namespace PredictedAdaptedEncoding
{
    class PackedCellContainerManager;
    uint64_t Assemble64BitFrom2Cell(packed64_t head_cell, packed64_t tail_cell) noexcept
    {
        val32_t head_val32 = PackedCell64_t::ExtractValue32(head_cell);
        val32_t tail_val32 = PackedCell64_t::ExtractValue32(tail_cell);
        uint64_t assembeled64 = (
            (static_cast<uint64_t>(tail_val32) << VALBITS) | static_cast<uint64_t>(head_val32)
        );
        return assembeled64;
    }
    std::optional<AcquirePairedPointerStruct> AdaptivePackedCellContainer::AcquirePairedAtomicPtr(
        size_t probable_idx, bool claim_ownership, int max_claim_attempts
    ) noexcept
    {
        if (!IfValidPayloadIndex_(probable_idx))
        {
            return std::nullopt;
        }
        if (max_claim_attempts <= 0)
        {
            max_claim_attempts = 256;
        }
        int curent_tries = 0;
        while (curent_tries++ < max_claim_attempts)
        {
            packed64_t packed_cell_value64 = BackingPtr[probable_idx].load(MoLoad_);
            RelOffsetMode32 curent_ptr_position = static_cast<RelOffsetMode32>(PackedCell64_t::ExtractRelOffsetFromPacked(packed_cell_value64));
            size_t head_idx = SIZE_MAX;
            size_t tail_idx = SIZE_MAX;
            if (curent_ptr_position == RelOffsetMode32::REL_OFFSET_HEAD_PTR)
            {
                head_idx = probable_idx;
                tail_idx = (probable_idx + 1) % GetPayloadCapacity();
            }
            else if (curent_ptr_position == RelOffsetMode32::RELOFFSET_TAIL_PTR)
            {
                head_idx = (probable_idx + GetPayloadCapacity() - 1) % GetPayloadCapacity();
                tail_idx = probable_idx;
            }
            else
            {
                return std::nullopt;
            }
            
            packed64_t head_screenshot = BackingPtr[head_idx].load(MoLoad_);
            packed64_t tail_screenshot = BackingPtr[tail_idx].load(MoLoad_);

            if (!PackedCell64_t::IsPackedCellVal32(head_screenshot) || !PackedCell64_t::IsPackedCellVal32(tail_screenshot))
            {
                return std::nullopt;
            }

            PackedCellLocalityTypes head_locality = PackedCell64_t::ExtractLocalityFromPacked(head_screenshot);
            PackedCellLocalityTypes tail_locality = PackedCell64_t::ExtractLocalityFromPacked(tail_screenshot);

            if (!claim_ownership)
            {
                if (head_locality != PackedCellLocalityTypes::ST_PUBLISHED || tail_locality != PackedCellLocalityTypes::ST_PUBLISHED)
                {
                    return std::nullopt;
                }
                AcquirePairedPointerStruct out_paired_ptr_struct{};
                out_paired_ptr_struct.AssembeledPtr = Assemble64BitFrom2Cell(head_screenshot, tail_screenshot);
                out_paired_ptr_struct.HeadIdx = head_idx;
                out_paired_ptr_struct.TailIdx = tail_idx;
                out_paired_ptr_struct.HeadScreenshot = head_screenshot;
                out_paired_ptr_struct.Position = curent_ptr_position;
                out_paired_ptr_struct.Ownership = false;
                return out_paired_ptr_struct;
            }
            
            if (head_locality != PackedCellLocalityTypes::ST_PUBLISHED || tail_locality != PackedCellLocalityTypes::ST_PUBLISHED)
            {
                return std::nullopt;
            }
            packed64_t want_head = PackedCell64_t::SetLocalityInPacked(head_screenshot, PackedCellLocalityTypes::ST_CLAIMED);
            packed64_t want_tail = PackedCell64_t::SetLocalityInPacked(tail_screenshot, PackedCellLocalityTypes::ST_CLAIMED);
            packed64_t expected_head = head_screenshot;
            if (!BackingPtr[head_idx].compare_exchange_strong(expected_head, want_head, OnExchangeSuccess, OnExchangeFailure))
            {
                if (APCManagerPtr_)
                {
                    APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(expected_head);
                }
                continue;
            }
            packed64_t expected_tail = tail_screenshot;
            if (!BackingPtr[tail_idx].compare_exchange_strong(expected_tail, want_tail, OnExchangeSuccess, OnExchangeFailure))
            {
                BackingPtr[head_idx].compare_exchange_strong(want_head, head_screenshot, OnExchangeSuccess, OnExchangeFailure);
                BackingPtr[head_idx].notify_all();
                if (APCManagerPtr_)
                {
                    APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(expected_tail);
                }
                continue;
            }
            
            AcquirePairedPointerStruct out_paired_ptr_struct_last{};
            out_paired_ptr_struct_last.AssembeledPtr = Assemble64BitFrom2Cell(head_screenshot, tail_screenshot);
            out_paired_ptr_struct_last.HeadIdx = head_idx;
            out_paired_ptr_struct_last.TailIdx = tail_idx;
            out_paired_ptr_struct_last.HeadScreenshot = head_screenshot;
            out_paired_ptr_struct_last.TailScreenshot = tail_screenshot;
            out_paired_ptr_struct_last.Position = curent_ptr_position;
            out_paired_ptr_struct_last.Ownership = true;
            return out_paired_ptr_struct_last;
        }
        return std::nullopt;
    }

    bool AdaptivePackedCellContainer::ReleaseAcquiredPairedPtr(const AcquirePairedPointerStruct& acquired_paired_pointer_struct, PackedCellLocalityTypes desired_locality) noexcept
    {
        if (!acquired_paired_pointer_struct.Ownership)
        {
            return false;
        }
        if (!IfValidPayloadIndex_(acquired_paired_pointer_struct.HeadIdx) || !IfValidPayloadIndex_(acquired_paired_pointer_struct.TailIdx))
        {
            return false;
        }
        packed64_t want_head =  PackedCell64_t::SetLocalityInPacked(acquired_paired_pointer_struct.HeadScreenshot, desired_locality);
        packed64_t want_tail = PackedCell64_t::SetLocalityInPacked(acquired_paired_pointer_struct.TailScreenshot, desired_locality);
        packed64_t expected_head = acquired_paired_pointer_struct.HeadScreenshot;
        bool head_ok = BackingPtr[acquired_paired_pointer_struct.HeadIdx].compare_exchange_strong(expected_head, want_head, OnExchangeSuccess, OnExchangeFailure);
        packed64_t expected_tail = acquired_paired_pointer_struct.TailScreenshot;
        bool tail_ok = BackingPtr[acquired_paired_pointer_struct.TailIdx].compare_exchange_strong(expected_tail, want_tail, OnExchangeSuccess, OnExchangeFailure);

        if (!head_ok || !tail_ok)
        {
            if (head_ok)
            {
                BackingPtr[acquired_paired_pointer_struct.HeadIdx].compare_exchange_strong(want_head, acquired_paired_pointer_struct.HeadScreenshot, OnExchangeSuccess, OnExchangeFailure);
            }
            else if (tail_ok)
            {
                BackingPtr[acquired_paired_pointer_struct.TailIdx].compare_exchange_strong(want_tail, acquired_paired_pointer_struct.TailScreenshot, OnExchangeSuccess, OnExchangeFailure);
            }
            BackingPtr[acquired_paired_pointer_struct.HeadIdx].notify_all();
            BackingPtr[acquired_paired_pointer_struct.TailIdx].notify_all();
            return false;
        }
        BackingPtr[acquired_paired_pointer_struct.HeadIdx].notify_all();
        BackingPtr[acquired_paired_pointer_struct.TailIdx].notify_all();
        return true;
    }

    void AdaptivePackedCellContainer::RetireAcquiredPointerPair(const AcquirePairedPointerStruct& acquired_paired_pointer_struct) noexcept
    {
        if (!acquired_paired_pointer_struct.Ownership)
        {
            return;
        }
        packed64_t idle32 = PackedCell64_t::MakeInitialPacked(PackedMode::MODE_VALUE32);
        BackingPtr[acquired_paired_pointer_struct.HeadIdx].store(idle32, MoStoreSeq_);
        BackingPtr[acquired_paired_pointer_struct.TailIdx].store(idle32, MoStoreSeq_);
        BackingPtr[acquired_paired_pointer_struct.HeadIdx].notify_all();
        BackingPtr[acquired_paired_pointer_struct.TailIdx].notify_all();
        OccupancyAddOrSubAndGetAfterChange(-1);

        RefreshAPCMeta_();
        if (APCManagerPtr_)
        {
            APCManagerPtr_->RequestForReclaimationOfTheAdaptivePackedCellContainer(this);
        }
        
    }

    template<typename PtrDtype>
    std::optional<PtrDtype> AdaptivePackedCellContainer::ViewPointerMemoryIfAssembeled(size_t probable_idx) noexcept
    {
        static_assert(std::is_trivially_copyable_v<PtrDtype>, "Data Type must be trivally copyable");
        auto maybe_ptr = AcquirePairedAtomicPtr(probable_idx, false);
        if (!maybe_ptr)
        {
            return std::nullopt;
        }
        AcquirePairedPointerStruct acquire_paired_ptr_struct = *maybe_ptr;
        QSBRGuard qsber_guard(this);
        packed64_t now_head = BackingPtr[acquire_paired_ptr_struct.HeadIdx].load(MoLoad_);
        packed64_t now_tail = BackingPtr[acquire_paired_ptr_struct.TailIdx].load(MoLoad_);
        if (now_head != acquire_paired_ptr_struct.HeadScreenshot || now_tail != acquire_paired_ptr_struct.TailScreenshot)
        {
            return std::nullopt;
        }
        void* raw_ptr = reinterpret_cast<void*>(acquire_paired_ptr_struct.AssembeledPtr);
        if (!raw_ptr)
        {
            return std::nullopt;
        }

        PtrDtype out{};
        std::memcpy(&out, raw_ptr, sizeof(PtrDtype));
        return out;
    }


    PublishResult AdaptivePackedCellContainer::PublishHeapPtrPair_(void* object_ptr, tag8_t rel_mask_with_ptrflag, int max_probs) noexcept
    {
        if (!IfAPCBranchValid())
        {
            return { PublishStatus::INVALID, SIZE_MAX};
        }
        if (GetPayloadCapacity() < MINIMUM_BRANCH_CAPACITY)
        {
            return {PublishStatus::FULL, SIZE_MAX};
        }
        
        uint64_t full_ptrval = reinterpret_cast<uint64_t>(object_ptr);
        uint32_t low32_half = static_cast<uint32_t>(full_ptrval & MaskBits(VALBITS));
        uint32_t high32_half = static_cast<uint32_t>((full_ptrval >> VALBITS) & MaskBits(VALBITS));
        size_t next_sequence = NextProducerSequence();
        if (next_sequence == SIZE_MAX)
        {
            return {PublishStatus::INVALID, SIZE_MAX};
        }
        
        size_t start = PayloadBegin() + ((next_sequence - PayloadBegin()) % GetPayloadCapacity());
        size_t step = GetHashedRendomizedStep_(next_sequence);
        int probes = 0;
        size_t idx = start;
        while (true)
        {
            size_t head = idx;
            size_t tail = (head + 1);
            packed64_t cur_head = BackingPtr[head].load(MoLoad_);
            packed64_t cur_tail = BackingPtr[tail].load(MoLoad_);
            PackedCellLocalityTypes head_locality = PackedCell64_t::ExtractLocalityFromPacked(cur_head);
            PackedCellLocalityTypes tail_locality = PackedCell64_t::ExtractLocalityFromPacked(cur_tail);
            if (head_locality == PackedCellLocalityTypes::ST_IDLE && tail_locality == PackedCellLocalityTypes::ST_IDLE)
            {
                packed64_t claimed_cur_head = PackedCell64_t::SetLocalityInPacked(cur_head, PackedCellLocalityTypes::ST_CLAIMED);
                packed64_t claimed_cur_tail = PackedCell64_t::SetLocalityInPacked(cur_tail, PackedCellLocalityTypes::ST_CLAIMED);
                packed64_t expected_head = cur_head;
                if (!BackingPtr[head].compare_exchange_strong(expected_head, claimed_cur_head, OnExchangeSuccess, OnExchangeFailure))
                {
                    BranchPluginOfAPC_->TotalCASFailForThisBranchIncreaseAndGet(1);
                }
                else
                {
                    packed64_t expected_tail = cur_tail;
                    if (!BackingPtr[tail].compare_exchange_strong(expected_tail, claimed_cur_tail, OnExchangeSuccess, OnExchangeFailure))
                    {
                        BackingPtr[head].store(cur_head, MoStoreSeq_);
                        BackingPtr[head].notify_all();
                        BranchPluginOfAPC_->TotalCASFailForThisBranchIncreaseAndGet(1);
                    }
                    else
                    {
                        val32_t tail_ptr_val32 = high32_half;
                        strl16_t strl_tail = MakeSTRLMode32_t(ZERO_PRIORITY, PackedCellLocalityTypes::ST_PUBLISHED, rel_mask_with_ptrflag, RelOffsetMode32::RELOFFSET_TAIL_PTR);
                        packed64_t tail_packed = PackedCell64_t::ComposeValue32u_64(tail_ptr_val32, 0u, strl_tail);
                        BackingPtr[tail].store(tail_packed, MoStoreSeq_);

                        val32_t head_ptr_value32 = low32_half;
                        strl16_t strl_head = MakeSTRLMode32_t(DEFAULT_PAIRED_HEAD_HALF_PRIORITY, PackedCellLocalityTypes::ST_PUBLISHED, rel_mask_with_ptrflag, RelOffsetMode32::REL_OFFSET_HEAD_PTR);
                        packed64_t head_packed = PackedCell64_t::ComposeValue32u_64(head_ptr_value32, 0u, strl_head);
                        BackingPtr[head].store(head_packed, MoStoreSeq_);
                        BackingPtr[tail].notify_all();
                        BackingPtr[head].notify_all();
                        OccupancyAddOrSubAndGetAfterChange(+1);
                        return {PublishStatus::OK, head};
                    }
                }
            }
            ++probes;
            if ((max_probs >=0 && probes >= max_probs) || probes >= static_cast<int>(GetPayloadCapacity()))
            {
                return {PublishStatus::FULL, SIZE_MAX};
            }
            idx = (((idx - PayloadBegin()) + step) % GetPayloadCapacity()) + PayloadBegin();
        }
    }

    bool AdaptivePackedCellContainer::PublishHeapPtrWithAdaptiveBackoff(void* target_publishable_ptr, uint16_t max_retries)
    {
        int publish_attempt = 0;

        while (publish_attempt <= max_retries)
        {
            PublishResult publish_result = PublishHeapPtrPair_(target_publishable_ptr, REL_NONE);
            if (publish_result.ResultStatus == PublishStatus::OK)
            {
                return true;
            }
            packed64_t observed = 0;
            if (BackingPtr && GetPayloadCapacity() > 0)
            {
                size_t idx = ( PayloadBegin() +
                    (std::hash<std::thread::id>{}(std::this_thread::get_id()) % GetPayloadCapacity())
                );
                observed = BackingPtr[idx].load(MoLoad_);
            }
            if (APCManagerPtr_)
            {
                auto& backoff = APCManagerPtr_->GetManagersAdaptiveBackoff();
                backoff.AdaptiveBackOffPacked(observed);
            }
            if (BranchPluginOfAPC_ && 
                BranchPluginOfAPC_->HasThisFlag(PackedCellBranchPlugin::ControlEnumOfAPCSegment::ENABLE_BRANCHING) && 
                BranchPluginOfAPC_->ShouldSplitNow() && APCManagerPtr_
            )
            {
                APCManagerPtr_->RequestBranchCreationForTheAdaptivePackedCellContainer(this);
            }
            
            ++publish_attempt;
        }
        return false;
    }


}