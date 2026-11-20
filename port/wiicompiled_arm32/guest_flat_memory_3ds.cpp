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
// app's memory is split between them; main() sizes both).
#include "guest_flat_memory.h"

#include "runtime_log.h"

#include <3ds.h>

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

uint8_t* Allocate(uint64_t size, bool& linear)
{
    const auto bytes = static_cast<size_t>(size);
    if (void* p = std::aligned_alloc(0x1000, (bytes + 0xFFF) & ~static_cast<size_t>(0xFFF))) {
        linear = false;
        return static_cast<uint8_t*>(p);
    }
    if (void* p = linearMemAlign(bytes, 0x1000)) {
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

bool IsActive() { return false; }

void Initialize(const std::vector<RegionRequest>& regions)
{
    if (g_initialized) return;  // once per process, as on the other hosts

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
