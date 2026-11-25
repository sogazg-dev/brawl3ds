// Live-allocation tracker for the 3DS build (linked with -Wl,--wrap= for malloc, calloc, realloc, free and
// operator new / new[]).
//
// After Brawl3dsAllocTrackStart() every allocation is recorded as pointer -> (caller, size) and every free removes
// its entry. Brawl3dsAllocTrackDump() sums what is still allocated per caller and writes the top callers, so a
// leak shows up as the call site that keeps allocating without freeing. Look the addresses up with
//   arm-none-eabi-addr2line -f -C -e brawl3ds.elf <address>
//
// operator new records its own caller (the C++ code, typically a container's growth function, whose name carries
// the element type) and the malloc it makes is not recorded a second time. The table lives in linear memory so
// it does not perturb the regular heap it measures, and a LightLock serialises it (SDL's audio thread allocates).
#include <3ds.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
void* __real__Znwj(size_t);
void* __real__Znaj(size_t);
}

namespace {

struct Entry {
    uintptr_t ptr;  // 0 = empty, 1 = deleted
    uint32_t caller;
    uint32_t size;
};

constexpr uint32_t kSlots = 1u << 18;  // 262144 live allocations, 3 MiB of linear memory
Entry* g_table = nullptr;
LightLock g_lock;
uint32_t g_dropped = 0;
int g_insideNew = 0;  // operator new's own malloc is not recorded twice

uint32_t Slot(uintptr_t p) { return static_cast<uint32_t>((p >> 3) * 2654435761u) & (kSlots - 1u); }

void Insert(void* p, size_t size, void* caller)
{
    if (!g_table || !p) return;
    LightLock_Lock(&g_lock);
    const auto key = reinterpret_cast<uintptr_t>(p);
    uint32_t i = Slot(key);
    for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSlots - 1u)) {
        if (g_table[i].ptr <= 1u) {
            g_table[i] = Entry{key, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(caller)), static_cast<uint32_t>(size)};
            LightLock_Unlock(&g_lock);
            return;
        }
    }
    ++g_dropped;
    LightLock_Unlock(&g_lock);
}

void Remove(void* p)
{
    if (!g_table || !p) return;
    LightLock_Lock(&g_lock);
    const auto key = reinterpret_cast<uintptr_t>(p);
    uint32_t i = Slot(key);
    for (uint32_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSlots - 1u)) {
        if (g_table[i].ptr == key) {
            g_table[i].ptr = 1u;
            break;
        }
        if (g_table[i].ptr == 0u) break;
    }
    LightLock_Unlock(&g_lock);
}

} // namespace

extern "C" {

void Brawl3dsAllocTrackStart()
{
    if (g_table) return;
    LightLock_Init(&g_lock);
    auto* table = static_cast<Entry*>(linearAlloc(sizeof(Entry) * kSlots));
    if (!table) return;
    std::memset(table, 0, sizeof(Entry) * kSlots);
    g_table = table;
}

void Brawl3dsAllocTrackDump(const char* path, unsigned frame)
{
    if (!g_table) return;
    struct Caller {
        uint32_t caller;
        uint32_t count;
        uint64_t bytes;
    };
    static Caller callers[1024];
    uint32_t n = 0;
    uint64_t total = 0;
    uint32_t live = 0;
    LightLock_Lock(&g_lock);
    for (uint32_t i = 0; i < kSlots; ++i) {
        const Entry& e = g_table[i];
        if (e.ptr <= 1u) continue;
        ++live;
        total += e.size;
        uint32_t k = 0;
        while (k < n && callers[k].caller != e.caller) ++k;
        if (k == n) {
            if (n == 1024) continue;
            callers[n++] = Caller{e.caller, 0, 0};
        }
        ++callers[k].count;
        callers[k].bytes += e.size;
    }
    LightLock_Unlock(&g_lock);
    // Top 30 by live bytes (selection sort: n is small and this runs once).
    for (uint32_t a = 0; a < n && a < 30; ++a) {
        uint32_t best = a;
        for (uint32_t b = a + 1; b < n; ++b) if (callers[b].bytes > callers[best].bytes) best = b;
        const Caller tmp = callers[a];
        callers[a] = callers[best];
        callers[best] = tmp;
    }
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    char line[160];
    int len = std::snprintf(line, sizeof(line),
                            "live allocations made since tracking started, at frame %u: %lu in %lu KiB (%lu not tracked)\n"
                            "   KiB   count  caller (arm-none-eabi-addr2line -f -C -e brawl3ds.elf <caller>)\n",
                            frame, static_cast<unsigned long>(live), static_cast<unsigned long>(total >> 10),
                            static_cast<unsigned long>(g_dropped));
    write(fd, line, static_cast<size_t>(len));
    for (uint32_t a = 0; a < n && a < 30; ++a) {
        len = std::snprintf(line, sizeof(line), "%6lu  %6lu  %08lx\n", static_cast<unsigned long>(callers[a].bytes >> 10),
                            static_cast<unsigned long>(callers[a].count), static_cast<unsigned long>(callers[a].caller));
        write(fd, line, static_cast<size_t>(len));
    }
    close(fd);
}

void* __wrap_malloc(size_t size)
{
    void* p = __real_malloc(size);
    if (!p && size) {
        // Every failed allocation (operator new too: it allocates through malloc), so an out-of-memory stop names
        // the size that did not fit. write(2) and a stack buffer: no stdio, which could itself allocate.
        char line[96];
        const int len = std::snprintf(line, sizeof(line), "[alloc] malloc(%lu) FAILED (caller %p)\n",
                                      static_cast<unsigned long>(size), __builtin_return_address(0));
        if (len > 0) write(2, line, static_cast<size_t>(len));
    }
    if (!g_insideNew) Insert(p, size, __builtin_return_address(0));
    return p;
}

void* __wrap_calloc(size_t count, size_t size)
{
    void* p = __real_calloc(count, size);
    Insert(p, count * size, __builtin_return_address(0));
    return p;
}

void* __wrap_realloc(void* old, size_t size)
{
    Remove(old);
    void* p = __real_realloc(old, size);
    Insert(p ? p : old, p ? size : 0, __builtin_return_address(0));
    return p;
}

void __wrap_free(void* p)
{
    Remove(p);
    __real_free(p);
}

void* __wrap__Znwj(size_t size)
{
    if (size >= (1u << 20)) {  // large C++ allocations and who makes them (memory is the limit on the 3DS)
        char line[96];
        const int len = std::snprintf(line, sizeof(line), "[alloc] new(%lu) from %p\n",
                                      static_cast<unsigned long>(size), __builtin_return_address(0));
        if (len > 0) write(2, line, static_cast<size_t>(len));
    }
    ++g_insideNew;
    void* p = nullptr;
    try {
        p = __real__Znwj(size);
    } catch (...) {
        // std::bad_alloc: malloc's own line names operator new as the caller; this one names who called it (a
        // caught bad_alloc repeated 28 times in the console run of 2026-11-23 before the run aborted).
        --g_insideNew;
        char line[96];
        const int len = std::snprintf(line, sizeof(line), "[alloc] new(%lu) FAILED (caller %p)\n",
                                      static_cast<unsigned long>(size), __builtin_return_address(0));
        if (len > 0) write(2, line, static_cast<size_t>(len));
        throw;
    }
    --g_insideNew;
    Insert(p, size, __builtin_return_address(0));
    return p;
}

void* __wrap__Znaj(size_t size)
{
    ++g_insideNew;
    void* p = __real__Znaj(size);
    --g_insideNew;
    Insert(p, size, __builtin_return_address(0));
    return p;
}

} // extern "C"
