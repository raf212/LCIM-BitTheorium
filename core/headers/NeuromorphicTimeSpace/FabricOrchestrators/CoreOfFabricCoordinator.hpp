#pragma once 
#include "../../AdaptivePackedCellContainer/AdaptivePackedCellContainer.hpp"

namespace BidirectionalInMemGraph
{

    struct CoreOfFabricCoordinator
    {
        /// UNCHECKED
        static constexpr size_t RELATION_WIDTH_OF_FABRIC = 0u;
        static constexpr size_t DEVICE_PLANNER_RECORD_LEN = 0u;
        static constexpr size_t WORK_RECORD_WIDTH_OF_FABRIC = 0u;
        static constexpr size_t DEFAULT_FABRIC_CONTROLIO_LENGTH = 512u;
        ///--------------------------
        static constexpr size_t COMPILED_DAG_LEN = 2u;

        static constexpr uint32_t FABRIC_MAGIC = 0x41504643u;
        static constexpr uint32_t FABRIC_META_EOF = 0x41474946u;
        static constexpr uint8_t EACH_TABLE_RECORD_SENTINAL = UINT8_MAX;
        static constexpr uint32_t HASH32_GRATIO_1 = 2654435769u;
        static constexpr uint32_t HASH32_GRATIO_2 = 123456789u;

        enum class RecordBookInternalIndexing : uint8_t
        {
            BEGIN64 = 0,
            END64 = 1,
        };
        static constexpr uint8_t RECORD_BOOK_WIDTH = static_cast<uint8_t>(RecordBookInternalIndexing::END64) + 1u;

        enum class FabricMetaIndicies : uint8_t
        {
            MAGIC = 0,
            TOTAL_CELLS = 1,
            PER_APC_RUNTIME_CELL_COUNT = 2,
            RECORD_BOOK_OF_TSC_BEGIN = 3,
            RECORD_BOOK_OF_TSC_END = 4,
            SEGMENT_POOL_BEGIN_IDX = 5,
            FIRST_FREE_IDX = 6,
            MAX_DIRECT_PARENTS_PER_AXIS = 7,
            EDGE_TABLE_RECORD_WIDTH = 8,

            ACTIVE_REGION_MASK = 9,
            ACTIVE_REGION_COUNT = 10,
            REGION_SCHEMA_RECORD_CELL_COUNT = 11,
            DEVICE_VIEW_ROW_CELL_COUNT = 12,
            MATRIC_BATCH_CAPACITY = 13,
            REGION_ALLIGNMENT_CELL_COUNT = 14,

            EOF_FABRIC_HEADER = 15
        };
        static constexpr uint8_t FABRIC_UNIT_COUNT = static_cast<uint8_t>(FabricMetaIndicies::EOF_FABRIC_HEADER) + 1u;

        static constexpr bool IsValidEdgeTable(FabricSegments table_class) noexcept
        {
            return
                table_class == FabricSegments::VALUE_PARENT_EDGE_TABLE_H ||
                table_class == FabricSegments::VOLATILE_PARENT_EDGE_TABLE_V;
        }

        static constexpr size_t DefaultFabricAlignment16Cell_(size_t value) noexcept
        {
            const uint8_t alignment_value_15 = 16 - 1;
            return (value + alignment_value_15) & ~static_cast<size_t>(alignment_value_15);
        }
    
    };


    struct RecordBookConf
    {
        static constexpr uint8_t RECORD_BOOK_INTERNAL_SEGMENT_COUNT = static_cast<uint8_t>(FabricSegments::SEGMENT_POOL) + 1;

        struct FabricSegmentBounds
        {
            uint64_t BeginIndex = UNSIGNED_ZERO;
            uint64_t EndIndex = UNSIGNED_ZERO;
            FabricSegments OwnerTableOfTheBounds{};
            bool IsValid = false;  
        };

    };


    struct RawPackedCellAllocator
    {
        using AllocateFunction = uint64_t* (*)(
            size_t count_of_packed_cell, size_t alignment, void* user
        ) noexcept;

        using FreeFunction = void (*)(
            uint64_t* packed_cell_storage_ptr, 
            size_t count_of_cell, size_t alignment, void*user
        ) noexcept;

        AllocateFunction AllocatePackedCellStorage{nullptr};
        FreeFunction FreePackedCellStorage{nullptr};
        void* User{nullptr};
        size_t Alignment{BIT_COUNT_OF_UINT64_T};

        static size_t AlignBiteCount_(size_t bytes, size_t alignment) noexcept
        {
            if (alignment == UNSIGNED_ZERO)
            {
                return bytes;
            }

            const size_t remaining_bytes = bytes % alignment;
            return remaining_bytes == UNSIGNED_ZERO ? bytes : bytes + (alignment - remaining_bytes);
        }

        static uint64_t* DefaultAllocateAtomicCells(
            size_t count_of_packed_cell, size_t alignment, void*
        ) noexcept
        {
            if (count_of_packed_cell == UNSIGNED_ZERO)
            {
                return nullptr;
            }

            alignment = std::max<size_t>(alignment, alignof(uint64_t));
            const size_t byte_count = sizeof(uint64_t) * count_of_packed_cell;
            const size_t aligned_bytes = AlignBiteCount_(byte_count, alignment);

#if defined(_MSC_VER)

            void* raw_packed_cell_memory = _aligned_malloc(aligned_bytes, alignment);
#else
            void* raw_packed_cell_memory = std::aligned_alloc(alignment, aligned_bytes);
#endif
            if (!raw_packed_cell_memory)
            {
                return nullptr;
            }
            std::memset(raw_packed_cell_memory, UNSIGNED_ZERO, aligned_bytes);
            return static_cast<uint64_t*>(raw_packed_cell_memory);
            
        }

        static void DefaultFreeAtomicCells(
            uint64_t* packed_cell_storage_ptr, 
            size_t, size_t, void*
        ) noexcept
        {
            if (!packed_cell_storage_ptr)
            {
                return;
            }
#if defined(_MSC_VER)
            _aligned_free(packed_cell_storage_ptr);
#else
            std::free(packed_cell_storage_ptr);
#endif
        }
    };


}
