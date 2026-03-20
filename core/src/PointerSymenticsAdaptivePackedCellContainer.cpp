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
        if (!IfIdxValid_(probable_idx))
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
            RelOffsetMode curent_ptr_position = static_cast<RelOffsetMode>(PackedCell64_t::ExtractRelOffsetFromPacked(packed_cell_value64));
            size_t head_idx = SIZE_MAX;
            size_t tail_idx = SIZE_MAX;
            if (curent_ptr_position == RelOffsetMode::REL_OFFSET_HEAD_PTR)
            {
                head_idx = probable_idx;
                tail_idx = (probable_idx + 1) % ContainerCapacity_;
            }
            else if (curent_ptr_position == RelOffsetMode::RELOFFSET_TAIL_PTR)
            {
                head_idx = (probable_idx + ContainerCapacity_ - 1) % ContainerCapacity_;
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

            tag8_t head_locality = PackedCell64_t::ExtractLocalityFromPacked(head_screenshot);
            tag8_t tail_locality = PackedCell64_t::ExtractLocalityFromPacked(tail_screenshot);

            if (!claim_ownership)
            {
                if (head_locality != ST_PUBLISHED || tail_locality != ST_PUBLISHED)
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
            
            if (head_locality != ST_PUBLISHED || tail_locality != ST_PUBLISHED)
            {
                return std::nullopt;
            }
            packed64_t want_head = PackedCell64_t::SetLocalityInPacked(head_screenshot, ST_CLAIMED);
            packed64_t want_tail = PackedCell64_t::SetLocalityInPacked(tail_screenshot, ST_CLAIMED);
            packed64_t expected_head = head_screenshot;
            if (!BackingPtr[head_idx].compare_exchange_strong(expected_head, want_head, EXsuccess_, EXfailure_))
            {
                if (APCManagerPtr_)
                {
                    APCManagerPtr_->GetCellsAdaptiveBackoffFromManager(expected_head);
                }
                continue;
            }
            packed64_t expected_tail = tail_screenshot;
            if (!BackingPtr[tail_idx].compare_exchange_strong(expected_tail, want_tail, EXsuccess_, EXfailure_))
            {
                BackingPtr[head_idx].compare_exchange_strong(want_head, head_screenshot, EXsuccess_, EXfailure_);
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

    bool AdaptivePackedCellContainer::ReleaseAcquiredPairedPtr(const AcquirePairedPointerStruct& acquired_paired_pointer_struct, tag8_t desired_locality) noexcept
    {
        if (!acquired_paired_pointer_struct.Ownership)
        {
            return false;
        }
        if (!IfIdxValid_(acquired_paired_pointer_struct.HeadIdx) || !IfIdxValid_(acquired_paired_pointer_struct.TailIdx))
        {
            return false;
        }
        packed64_t want_head =  PackedCell64_t::SetLocalityInPacked(acquired_paired_pointer_struct.HeadScreenshot, desired_locality);
        packed64_t want_tail = PackedCell64_t::SetLocalityInPacked(acquired_paired_pointer_struct.TailScreenshot, desired_locality);
        packed64_t expected_head = acquired_paired_pointer_struct.HeadScreenshot;
        bool head_ok = BackingPtr[acquired_paired_pointer_struct.HeadIdx].compare_exchange_strong(expected_head, want_head, EXsuccess_, EXfailure_);
        packed64_t expected_tail = acquired_paired_pointer_struct.TailScreenshot;
        bool tail_ok = BackingPtr[acquired_paired_pointer_struct.TailScreenshot].compare_exchange_strong(expected_tail, want_tail, EXsuccess_, EXfailure_);

        if (!head_ok || !tail_ok)
        {
            if (head_ok && !tail_ok)
            {
                BackingPtr[acquired_paired_pointer_struct.HeadIdx].compare_exchange_strong(want_head, acquired_paired_pointer_struct.HeadScreenshot, EXsuccess_, EXfailure_);
            }
            else if (!head_ok && tail_ok)
            {
                BackingPtr[acquired_paired_pointer_struct.TailIdx].compare_exchange_strong(want_tail, acquired_paired_pointer_struct.TailScreenshot, EXsuccess_, EXfailure_);
            }
            BackingPtr[acquired_paired_pointer_struct.HeadIdx].notify_all();
            BackingPtr[acquired_paired_pointer_struct.TailIdx].notify_all();
            return false;
        }
        BackingPtr[acquired_paired_pointer_struct.HeadIdx].notify_all();
        BackingPtr[acquired_paired_pointer_struct.TailIdx].notify_all();
        return true;
    }

    void AdaptivePackedCellContainer::RetiredAcquiredPointerPair(const AcquirePairedPointerStruct& acquired_paired_pointer_struct, DeviceFence_ device_fence) noexcept
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
        Occupancy_.fetch_add(1, std::memory_order_acq_rel);

        void* object_ptr = reinterpret_cast<void*>(acquired_paired_pointer_struct.AssembeledPtr);
        if (object_ptr)
        {
            RelEntry_* rel_entry_ptr = new RelEntry_ (acquired_paired_pointer_struct.AssembeledPtr);
            rel_entry_ptr->KindFinalizer = FinalizerKind_::NONE;
            rel_entry_ptr->APCDeviceFence = std::move(device_fence);
            uint64_t cur_epoch = GlobalEpoch_.load(MoLoad_);
            rel_entry_ptr->RetireEpoch.store(cur_epoch, MoStoreSeq_);
            //have to convert to mannager 
            RetirePushLocked_(rel_entry_ptr);
            TryReclaimRetirePairedPtr_();
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

        PtrDtype out;
        std::memcpy(&out, raw_ptr, sizeof(PtrDtype));
        return out;
    }

}