#pragma once
#include "FabricToAPCLinker.hpp"
#include <span>

namespace BidirectionalInMemGraph
{

    template<class DType>
    class RegionView
    {
    public:
        using SD = SchemaDefinition;

    private:
        SD::SchemaProtocols Protocol_{SD::SchemaProtocols::PRIVATE_REGION};
        std::span<DType> Elements_{};
        APCUseScope Use_{};
    
    public:
        constexpr RegionView() noexcept = default;

        constexpr RegionView(
            std::span<DType> elements,
            SD::SchemaProtocols protocol,
            APCUseScope use
        ) noexcept:
            Elements_(elements),
            Protocol_(protocol),
            Use_(std::move(use))
        {}

        constexpr bool IsValid() const noexcept
        {
            return static_cast<bool>(Use_) && !Elements_.empty();
        }

        constexpr size_t Size() const noexcept
        {
            return Use_ ?  Elements_.size() : UNSIGNED_ZERO;
        }

        constexpr SD::SchemaProtocols GetProtocol() const noexcept
        {
            return Protocol_;
        }

        std::optional<std::span<DType>> RawMutableSpan() noexcept
        {
            if (!Use_ || Protocol_ != SD::SchemaProtocols::PRIVATE_REGION)
            {
                return std::nullopt;
            }
            
            return Elements_;
        }

        DType AtomicLoad(size_t idx, std::memory_order mem_order = std::memory_order_acquire) const noexcept
        {
            if (
                !Use_ ||
                Protocol_ != SD::SchemaProtocols::ATOMIC_WORD_ARRAY ||
                idx >= Elements_.size()
            )
            {
                return DType{};
            }

            return std::atomic_ref<DType>(Elements_[idx]).load(mem_order);
            
        }


        bool AtomicStore(
            size_t idx,
            DType value,
            std::memory_order order = std::memory_order_release
        ) noexcept
        {
            if (
                !Use_ ||
                Protocol_ != SD::SchemaProtocols::ATOMIC_WORD_ARRAY ||
                idx >= Elements_.size()
            )
            {
                return false;
            }

            std::atomic_ref<DType>(Elements_[idx]).store(value, order);
            return true;
        }

        bool AtomicCompareExchangeStrong(
            size_t idx,
            DType& expected,
            DType desired,
            std::memory_order success = std::memory_order_acq_rel,
            std::memory_order failure = std::memory_order_acquire
        ) noexcept
        {
            if (
                !Use_ ||
                Protocol_ != SD::SchemaProtocols::ATOMIC_WORD_ARRAY ||
                idx >= Elements_.size()
            )
            {
                return false;
            }

            return std::atomic_ref<DType>(Elements_[idx]).compare_exchange_strong(
                expected,
                desired,
                success,
                failure
            );
        }

    };


    class RegionViewConstructor : public FabricToAPCLinker
    {   
    private:
        bool ResolveRegionView_(
            MacroColumnOfAPC column_name,
            uint32_t record_ordinal,
            ResolveRegionBiteView& out
        ) noexcept;

    public:
        using SD = SchemaDefinition;

        template<class DType>
        std::optional<RegionView<DType>> BuildAViewOverRegion(
            MacroColumnOfAPC macro_column,
            uint32_t record_ordinal = UNSIGNED_ZERO
        ) noexcept
        {
            static_assert(std::is_trivially_copyable_v<DType>);

            APCUseScope use = AcquireAPCUse_();
            if (!use)
            {
                return std::nullopt;
            }

            ResolveRegionBiteView resolved{};
            if (!ResolveRegionView_(macro_column, record_ordinal, resolved))
            {
                return std::nullopt;
            }


            switch (resolved.Schema->Protocol)
            {
            case SD::SchemaProtocols::PRIVATE_REGION:
            case SD::SchemaProtocols::IMMUTABLE_SNAPSHOT:
                if (!APCStorageGeometry::CanInstallTypedSpan<DType>(resolved))
                {
                    return std::nullopt;
                }
                break;
            
            case SD::SchemaProtocols::ATOMIC_WORD_ARRAY:
                if (!APCStorageGeometry::CanInstallAtomicSpan<DType>(resolved))
                {
                    return std::nullopt;
                }
                break;
            
            default:
                return std::nullopt;
            }
            
            DType* type_based = reinterpret_cast<DType*>(resolved.Bytes.data());
            const size_t element_count = static_cast<uint64_t>(resolved.Schema->MatrixHeight) * resolved.Schema->MatrixWidth;

            if (element_count > SIZE_MAX)
            {
                return std::nullopt;
            }
            
            return RegionView<DType>(
                std::span<DType>(type_based, element_count),
                resolved.Schema->Protocol,
                std::move(use)
            );
        }

        template<class DType>
        bool ZeroARegion(MacroColumnOfAPC macro_column) noexcept
        {
            std::optional<RegionView<DType>> maybe_view = BuildAViewOverRegion<DType>(macro_column);
            if (!maybe_view.has_value())
            {
                return false;
            }

            RegionView<DType>& view = maybe_view.value();

            using SD = SchemaDefinition;

            switch (view.GetProtocol())
            {
            case SD::SchemaProtocols::PRIVATE_REGION:
            {
                std::optional<std::span<DType>> maybe_mutable_span = view.RawMutableSpan();
                if (!maybe_mutable_span.has_value())
                {
                    return false;
                }
                
                for (DType& value : maybe_mutable_span.value())
                {
                    value = DType{};
                }
                return true;
            }
            case SD::SchemaProtocols::ATOMIC_WORD_ARRAY:
                for (size_t i = 0; i < view.Size(); i++)
                {
                    if (!view.AtomicStore(i, DType{}, std::memory_order_relaxed))
                    {
                        return false;
                    }
                }
                return true;
            
            default:
                return false;
            }
            
        }

    };
    
    
    
}