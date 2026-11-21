// GuestFlat for the Nintendo 3DS: replaces runtime/src/guest_flat_memory.cpp in the 3DS build.
//
// A 32-bit host cannot reserve the 4 GiB flat guest window, so on the 3DS the flat fast paths are
// compiled out (guest_flat_memory.h: RequiresCheckedAccess() is constexpr true) and every translated
// access goes through Memory's 1 MiB page table. That checked path already materializes deferred
// (EFB) reads and applies the MMIO / executable-write policies, so this module has no guest view to
// protect: it only owns the backing stores and answers HostPointer.
//
// Mirrors share storage, as on the other hosts: MEM1's physical/cached/uncached views (0x00000000,
// 0x80000000, 0xC0000000) are one allocation, and so are MEM2's (0x10000000, 0x90000000, 0xD0000000).
// Any other region (Kamek overlay, locked cache) gets its own.
//
// Allocation: the regular heap first, then the GPU-visible linear heap (in the 178 MB system mode the
#if defined(__3DS__)  // the PC build globs runtime/src; this file is 3DS-only
// app's memory is split between them; main() sizes both).
#include "guest_flat_memory.h"

#include "memory.h"
#include "memory_access.h"
#include "runtime_log.h"

#include <3ds.h>
#include <malloc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace GuestFlat {
namespace {

struct Mapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct Store {
    Backing backing = Backing::Owned;
    uint8_t* host = nullptr;
    uint64_t size = 0;
    bool linear = false;
};

std::vector<Mapping> g_mappings;
std::vector<Store> g_stores;
bool g_initialized = false;

// The regular heap also serves every runtime allocation (newlib malloc), while linear memory is only used by audio
// and GPU buffers. So a guest region goes to linear memory whenever it fits with this much left over for those, and
// to the regular heap otherwise.
// The headless timing build has no GPU buffers and dummy audio, so 1 MiB is enough; the renderer will need its own
// memory plan (this was 4 MiB, but the extra code of the -O2 hot shards needs that room in the regular heap).
constexpr size_t kLinearReserve = 1u * 1024u * 1024u;

// Blocks set aside by Brawl3dsReserveGuestMemory() before the runtime starts, so the big guest regions get their
// memory before the runtime's own allocations fragment the regular heap.
struct Reserved {
    uint8_t* host = nullptr;
    size_t size = 0;
    bool linear = false;
};
Reserved g_reserved[8];

uint8_t* AllocateFresh(size_t rounded, bool& linear);

uint8_t* Allocate(uint64_t size, bool& linear)
{
    const auto bytes = static_cast<size_t>(size);
    const auto rounded = (bytes + 0xFFF) & ~static_cast<size_t>(0xFFF);
    for (auto& block : g_reserved) {
        if (block.host && block.size == rounded) {
            uint8_t* host = block.host;
            linear = block.linear;
            block = {};
            return host;
        }
    }
    return AllocateFresh(rounded, linear);
}

uint8_t* AllocateFresh(size_t rounded, bool& linear)
{
    if (linearSpaceFree() >= rounded + kLinearReserve) {
        if (void* p = linearMemAlign(rounded, 0x1000)) {
            linear = true;
            return static_cast<uint8_t*>(p);
        }
    }
    if (void* p = std::aligned_alloc(0x1000, rounded)) {
        linear = false;
        return static_cast<uint8_t*>(p);
    }
    if (void* p = linearMemAlign(rounded, 0x1000)) {  // last resort: dip into the reserve
        linear = true;
        return static_cast<uint8_t*>(p);
    }
    return nullptr;
}

Store* FindShared(Backing backing)
{
    for (auto& store : g_stores) {
        if (store.backing == backing) return &store;
    }
    return nullptr;
}

} // namespace

// Called from main() before RuntimeMain: allocate the guest regions Memory::Config::WiiDefaults() will ask for, in
// the order that packs best (the largest first into the regular heap, MEM1 into the linear heap).
extern "C" void Brawl3dsReserveGuestMemory()
{
    const size_t sizes[] = {
        Memory::kMem1Size,                          // MEM1 and its mirrors (fits the linear heap)
        Memory::kMem2Size,                          // MEM2 and its mirrors
        Memory::kModuleHomesSize,                   // Brawl's pre-linked module homes
        MemoryInline::kPageSize + 4096u,            // the locked cache (memory.cpp backs a page and a bit)
    };
    size_t n = 0;
    for (const size_t size : sizes) {
        const size_t rounded = (size + 0xFFF) & ~static_cast<size_t>(0xFFF);
        bool linear = false;
        if (uint8_t* host = AllocateFresh(rounded, linear)) {
            g_reserved[n++] = Reserved{host, rounded, linear};
            std::printf("[guest] reserved %zu KiB (%s heap)\n", rounded >> 10, linear ? "linear" : "regular");
        } else {
            std::printf("[guest] could not reserve %zu KiB\n", rounded >> 10);
        }
    }
}

bool IsActive() { return false; }

void Initialize(const std::vector<RegionRequest>& regions)
{
    if (g_initialized) return;  // once per process, as on the other hosts
    std::printf("[guest] Initialize: runtime malloc in use %d KiB, linear free %lu KiB\n", mallinfo().uordblks >> 10,
                static_cast<unsigned long>(linearSpaceFree() >> 10));

    // One shared store per mirrored backing, sized for its largest view.
    for (const auto& region : regions) {
        if (region.backing == Backing::Owned || region.size == 0) continue;
        Store* store = FindShared(region.backing);
        if (!store) {
            g_stores.push_back(Store{region.backing, nullptr, 0, false});
            store = &g_stores.back();
        }
        if (region.size > store->size) store->size = region.size;
    }
    for (auto& store : g_stores) {
        store.host = Allocate(store.size, store.linear);
        if (!store.host) {
            throw std::runtime_error("3DS: could not allocate " + std::to_string(store.size >> 20) +
                                     " MiB of guest RAM (heap " +
                                     std::to_string(osGetMemRegionFree(MEMREGION_APPLICATION) >> 20) + " MiB free)");
        }
        std::memset(store.host, 0, static_cast<size_t>(store.size));
    }

    for (const auto& region : regions) {
        if (region.size == 0) continue;
        Mapping mapping{region.base, region.size, nullptr};
        if (region.backing == Backing::Owned) {
            bool linear = false;
            mapping.host = Allocate(region.size, linear);
            if (!mapping.host) {
                throw std::runtime_error("3DS: could not allocate guest region at 0x" + std::to_string(region.base));
            }
            std::memset(mapping.host, 0, static_cast<size_t>(region.size));
            std::printf("[guest] region 0x%08lX: %llu KiB (%s heap)\n", static_cast<unsigned long>(region.base),
                        static_cast<unsigned long long>(region.size >> 10), linear ? "linear" : "regular");
        } else {
            mapping.host = FindShared(region.backing)->host;
        }
        g_mappings.push_back(mapping);
    }
    g_initialized = true;

    for (const auto& store : g_stores) {
        std::printf("[guest] %s backing: %llu MiB (%s heap)\n", store.backing == Backing::Mem1 ? "MEM1" : "MEM2",
                    static_cast<unsigned long long>(store.size >> 20), store.linear ? "linear" : "regular");
    }
}

uint8_t* HostPointer(uint32_t guestAddress)
{
    for (const auto& mapping : g_mappings) {
        if (guestAddress >= mapping.base && guestAddress - mapping.base < mapping.size) {
            return mapping.host + (guestAddress - mapping.base);
        }
    }
    return nullptr;
}

// No guest view exists on the 3DS: the checked page-table path enforces these policies itself.
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
void ReprotectExecutablePage(uint32_t) {}

FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }

} // namespace GuestFlat
#endif // defined(__3DS__)
