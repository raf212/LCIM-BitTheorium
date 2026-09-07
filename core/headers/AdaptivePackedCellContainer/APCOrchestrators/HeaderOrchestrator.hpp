#pragma once
#include <functional>
#include "LayerLayout.hpp"

namespace BidirectionalInMemGraph
{

    struct DescriptionOfAPC 
    {
        
        struct SeqLockAndStateStruct
        {
            uint32_t SeqLock = UINT32_MAX;
            StateOfAPC StateOfTheAPC = StateOfAPC::RETIRED;
            bool IsValid = false;
        };

        static_assert(sizeof(SeqLockAndStateStruct) <= sizeof(uint64_t));

        static constexpr uint64_t ComposeSeqLockAndState(SeqLockAndStateStruct& files) noexcept
        {
            if (
                !APCDataStructure::IsValid32BitAPCUnit(files.SeqLock) ||
                !ValidateStateAgainstSeqLock(files)
            )
            {
                return FABRIC_CELL_SENTINAL;
            }
            return TwinU32ToU64::PackDoubleUnsigned32In64(files.SeqLock, static_cast<uint32_t>(files.StateOfTheAPC));
        }

        static constexpr bool GetSeqLockAndLifeCycle(
            uint64_t desc_id_state,
            SeqLockAndStateStruct& values
        ) noexcept
        {
            values = SeqLockAndStateStruct{};
            values.SeqLock = TwinU32ToU64::ExtractLow32Of64(desc_id_state);
            values.StateOfTheAPC = static_cast<StateOfAPC>(TwinU32ToU64::ExtractHigh32Of64(desc_id_state));

            if (!APCDataStructure::IsValidFabricUnit(values.SeqLock))
            {
                return false;
            }
            return ValidateStateAgainstSeqLock(values);
        }

        static constexpr bool ValidateStateAgainstSeqLock(SeqLockAndStateStruct& files) noexcept
        {
            if (!APCDataStructure::IsValidFabricUnit(files.SeqLock))
            {
                files.IsValid = false;
                return false;
            }
            if (
                files.StateOfTheAPC == StateOfAPC::RESERVED &&
                APCDataStructure::IsValidEven64(files.SeqLock)
            )
            {
                files.IsValid = false;
                return false;
            }
            if (
                files.StateOfTheAPC != StateOfAPC::RESERVED &&
                !APCDataStructure::IsValidEven64(files.SeqLock)
            )
            {
                files.IsValid = false;
                return false;
            }

            files.IsValid = true;
            return true;
        }

        static constexpr bool IsTransitionStateLeagal(StateOfAPC current_state, StateOfAPC desired_state) noexcept
        {
            return (current_state == StateOfAPC::FREE && desired_state == StateOfAPC::RESERVED) ||
                (current_state == StateOfAPC::RESERVED && desired_state == StateOfAPC::FREE) ||
                (current_state == StateOfAPC::RESERVED && desired_state == StateOfAPC::LIVE) ||
                (current_state == StateOfAPC::LIVE && desired_state == StateOfAPC::RESERVED) ||
                (current_state == StateOfAPC::RESERVED && desired_state == StateOfAPC::RETIRED) ||
                (current_state == StateOfAPC::RETIRED && desired_state == StateOfAPC::RESERVED) ||
                (current_state == StateOfAPC::LIVE && desired_state == StateOfAPC::HAULTED) ||
                (current_state == StateOfAPC::HAULTED && desired_state == StateOfAPC::LIVE);
                
        }

    };

    struct HeaderOrchestrator : DescriptionOfAPC
    {
        static constexpr uint8_t LEN_OF_APC_META_BUFFER_OR_COUNT = APCDataStructure::META_CELL_COUNT;


        using DefaultMemCopyBuffer = std::array<uint64_t, UINT8_MAX>;

        using APCMetaBuffer = std::array<uint64_t, LEN_OF_APC_META_BUFFER_OR_COUNT>;


        static constexpr void BuildNullMemCopyBuffer(DefaultMemCopyBuffer& a_default_buffer) noexcept
        {
            for (size_t i = 0; i < a_default_buffer.size(); i++)
            {
                a_default_buffer[i] = FABRIC_CELL_SENTINAL;
            }
        }

        static constexpr void ConstructNullHeaderBuffer(APCMetaBuffer& a_meta_buffer) noexcept
        {
            for (size_t i = 0; i < a_meta_buffer.size(); i++)
            {
                a_meta_buffer[i] = FABRIC_CELL_SENTINAL;
            }
        }

        static constexpr bool InitializeDefaultHeaderBuffer(
            APCMetaBuffer& header,
            uint32_t apc_slot_idx,
            uint32_t capacity_of_apc
        ) noexcept
        {
            for (uint64_t& word : header)
            {
                word = UNSIGNED_ZERO;
            }
            
            if (
                !APCDataStructure::IsCapacityOfAPCValid(capacity_of_apc) ||
                !APCDataStructure::IsValid32BitAPCUnit(apc_slot_idx)
            )
            {
                return false;
            }

            header[static_cast<std::size_t>(APCDataStructure::HeaderIdentifierOfAPC::MAGIC_ID)] = APCDataStructure::BRANCH_MAGIC;
            header[static_cast<std::size_t>(APCDataStructure::HeaderIdentifierOfAPC::APC_SLOT_IDX)] = apc_slot_idx;
            header[static_cast<std::size_t>(APCDataStructure::HeaderIdentifierOfAPC::EOF_APC_HEADER)] = APCDataStructure::EOF_HEADER;

            return true;
        }


    };
    

}