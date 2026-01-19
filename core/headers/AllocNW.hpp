#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <memoryapi.h>
#elif defined(HAVE_LIBNUMA)
    #include <numa.h>
    #include <numaif.h>
    #include <unistd.h>
#else
  #error "AllocNW.hpp requires either Windows NUMA (VirtualAllocExNuma) or Linux libnuma. Define HAVE_LIBNUMA and link -lnuma for Linux."
#endif

namespace AtomicCScompact::AllocNW
{
    inline void* AlignedAllocP(size_t alignment, size_t size)
    {
        if (alignment == 0) alignment = alignof(void*);
    #if defined(_MSC_VER)
        void* p = _aligned_malloc(size, alignment);
        if (!p) throw std::bad_alloc();
        return p;
    #else
        void* p = nullptr;
        int rc = posix_memalign(&p, alignment, size);
        if (rc != 0 || !p) throw std::bad_alloc();
        return p;
    #endif
    }

    inline void AlignedFreeP(void* p) noexcept
    {
    #if defined(_MSC_VER)
        _aligned_free(p);
    #else
        free(p);
    #endif
    }

    inline size_t PageSize()
    {
    #if defined(_WIN32)
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        return static_cast<size_t>(sysinfo.dwPageSize);
    #else
        long ps = sysconf(_SC_PAGESIZE);
        return (ps > 0) ? static_cast<size_t>(ps) : 4096u;
    #endif
    }

    // AlignedAllocONnode(alignment, sizeBytes, node)
#if defined(HAVE_LIBNUMA)
    inline void* AlignedAllocONnode(size_t alignment, size_t sizeBytes, int node)
    {
        (void)alignment;
        size_t ps = PageSize();
        size_t rounded = ((sizeBytes + ps - 1) / ps) * ps;
        if (numa_available() < 0) throw std::runtime_error("libnuma not available");
        if (node < 0 || node > numa_max_node()) throw std::invalid_argument("Invalid node");
        // allocate with rounding
        void* p = numa_alloc_onnode(rounded, node);
        if (!p) throw std::bad_alloc();
        return p;
    }

    inline void FreeONNode(void* p, size_t sizeBytes) noexcept
    {
        if (!p) return;
        size_t ps = PageSize();
        size_t rounded = ((sizeBytes + ps - 1) / ps) * ps;
        numa_free(p, rounded);
    }

#elif defined(_WIN32)
    inline void* AlignedAllocONnode(size_t alignment, size_t sizeBytes, int node)
    {
        (void *)alignment;
        size_t ps = PageSize();
        size_t rounded = ((sizeBytes + ps - 1) / ps) * ps;
        HANDLE HProc = GetCurrentProcess();
        DWORD alloc_type = MEM_RESERVE | MEM_COMMIT;
        DWORD protect = PAGE_READWRITE;
        LPVOID result = VirtualAllocExNuma(HProc, nullptr, rounded, alloc_type, protect, static_cast<DWORD>(node));
        if (!result) throw std::bad_alloc();
        return result;
    }

    inline void FreeONNode(void* p, size_t /*sizeBytes*/) noexcept
    {
        if (!p) return;
        VirtualFree(p, 0, MEM_RELEASE);
    }
#endif

} // namespace AtomicCScompact::AllocNW
