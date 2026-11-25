// Code packs: a fighter's or stage's translated code, loaded from sdmc:/3ds/brawl3ds/packs/ the first time one of
// its functions is called (tools/build_packs.py makes them and describes the format).
//
// The executable keeps a stub for every function it left out; the stub ends in Brawl3ds_NotLinked (main_3ds.cpp),
// which asks here before it falls back to the interpreter. A pack is read into an arena set aside early (the heap
// has no megabyte in one piece later), adjusted to where it landed and made executable (svcControlProcessMemory:
// the probe in main_3ds.cpp showed the console allows it). Arena memory is used once: nothing here is unloaded
// yet, so no page that ran code is ever rewritten and the instruction cache cannot hold stale code. When the
// arena is full the remaining modules run interpreted, as before.
#include <3ds.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unwind.h>

#include "ppc_runtime.h"
#include "abi_bridge.h"

extern "C" void Brawl3ds_NotLinked(uint32_t guestAddress, CpuContext* ctx);
extern "C" const char __exidx_start[];
extern "C" const char __exidx_end[];
extern "C" u8* g_brawl3dsPackArena;
extern "C" u32 g_brawl3dsPackArenaSize;

namespace {

struct PackHeader {
    char magic[4];
    uint32_t version, rxSize, fileSize, memSize, absCount, relCount, exportCount, exidxStart, exidxCount;
};
struct Export {
    uint32_t guest, offset;
};
struct Pack {
    uint32_t start, end;  // guest addresses of its first and last function
    char name[24];
    uint8_t state;        // 0 not tried, 1 loaded, 2 not available
    uint32_t calls;       // while not loaded: calls that ran interpreted (kLoadAfterCalls)
    uint8_t* base;
    Export* exports;
    uint32_t exportCount, rxSize, exidxStart, exidxCount;
};
using Entry = void (*)(CpuContext*);
struct Slot {
    uint32_t guest;
    Entry entry;  // null: known not to be in a pack
};

constexpr uint32_t kMaxPacks = 96;
// A module's pack is loaded once the module has been called this often. The character select screen calls a
// function or two of many fighters: those run interpreted and cost no arena. A fighter or stage in a match
// passes this within its first frames.
constexpr uint32_t kLoadAfterCalls = 300;
constexpr uint32_t kSlots = 4096;
Pack g_packs[kMaxPacks];
uint32_t g_packCount = 0;
Slot g_slots[kSlots];
uint8_t* g_arena = nullptr;
uint32_t g_arenaSize = 0, g_arenaUsed = 0;
Handle g_process = CUR_PROCESS_HANDLE;
uint32_t g_loaded = 0, g_loadedBytes = 0;
bool g_arenaFullLogged = false;
bool g_wantReset = false;   // a pack did not fit: empty the arena at the next scene change, if that is safe
bool g_canReset = false;    // the instruction cache can be invalidated (Luma3DS), or packs/force_reset.txt
bool g_lumaIcache = false;
uint32_t g_resets = 0;

bool ReadAll(std::FILE* f, void* to, uint32_t bytes) { return std::fread(to, 1, bytes, f) == bytes; }

bool Load(Pack& pack)
{
    const uint64_t startTick = svcGetSystemTick();
    char path[80];
    std::snprintf(path, sizeof(path), "sdmc:/3ds/brawl3ds/packs/%s.b3p", pack.name);
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    PackHeader h{};
    bool ok = ReadAll(f, &h, sizeof(h)) && std::memcmp(h.magic, "B3PK", 4) == 0 && h.version == 1;
    const uint32_t block = ok ? (h.memSize + 0xFFFu) & ~0xFFFu : 0;
    if (ok && g_arenaUsed + block > g_arenaSize) {
        g_wantReset = true;
        if (!g_arenaFullLogged) {
            g_arenaFullLogged = true;
            std::fprintf(stderr, "[packs] the arena is full (%lu of %lu KiB): %s and later modules run interpreted\n",
                         static_cast<unsigned long>(g_arenaUsed >> 10), static_cast<unsigned long>(g_arenaSize >> 10),
                         pack.name);
        }
        ok = false;
    }
    uint32_t* fixups = nullptr;
    Export* exports = nullptr;
    uint8_t* base = g_arena + g_arenaUsed;
    if (ok) {
        fixups = static_cast<uint32_t*>(std::malloc((h.absCount + h.relCount) * 4 + 4));
        exports = static_cast<Export*>(std::malloc(h.exportCount * sizeof(Export) + 4));
        ok = fixups && exports && ReadAll(f, fixups, (h.absCount + h.relCount) * 4) &&
             ReadAll(f, exports, h.exportCount * sizeof(Export)) && ReadAll(f, base, h.fileSize);
    }
    std::fclose(f);
    if (ok) {
        std::memset(base + h.fileSize, 0, block - h.fileSize);
        const uint32_t where = reinterpret_cast<uint32_t>(base);
        for (uint32_t i = 0; i < h.absCount; ++i) {  // addresses inside the pack
            uint32_t word;
            std::memcpy(&word, base + fixups[i], 4);
            word += where;
            std::memcpy(base + fixups[i], &word, 4);
        }
        for (uint32_t i = 0; i < h.relCount; ++i) {  // offsets from the pack to the executable
            const uint32_t at = fixups[h.absCount + i] & 0x7FFFFFFFu;
            uint32_t word;
            std::memcpy(&word, base + at, 4);
            if (fixups[h.absCount + i] & 0x80000000u) word = (word & 0x80000000u) | ((word - where) & 0x7FFFFFFFu);
            else word -= where;
            std::memcpy(base + at, &word, 4);
        }
        svcFlushProcessDataCache(g_process, where, block);
        const Result protect = svcControlProcessMemory(g_process, where, 0, h.rxSize, MEMOP_PROT,
                                                       MEMPERM_READ | MEMPERM_EXECUTE);
        if (R_FAILED(protect)) {
            std::fprintf(stderr, "[packs] %s: could not make its code executable (%08lX)\n", pack.name,
                         static_cast<unsigned long>(protect));
            ok = false;
        }
    }
    std::free(fixups);
    if (!ok) {
        std::free(exports);
        return false;
    }
    g_arenaUsed += block;
    pack.base = base;
    pack.exports = exports;
    pack.exportCount = h.exportCount;
    pack.rxSize = h.rxSize;
    pack.exidxStart = h.exidxStart;
    pack.exidxCount = h.exidxCount;
    ++g_loaded;
    g_loadedBytes += block;
    std::fprintf(stderr, "[packs] %s loaded: %lu functions, %lu KiB, %.0f ms (arena %lu of %lu KiB, %lu packs)\n",
                 pack.name, static_cast<unsigned long>(h.exportCount), static_cast<unsigned long>(block >> 10),
                 static_cast<double>(svcGetSystemTick() - startTick) * 1000.0 / SYSCLOCK_ARM11,
                 static_cast<unsigned long>(g_arenaUsed >> 10), static_cast<unsigned long>(g_arenaSize >> 10),
                 static_cast<unsigned long>(g_loaded));
    return true;
}

// `cache`: the answer holds until the arena is emptied (false while a pack is still counting calls).
Entry Lookup(uint32_t guest, bool& cache)
{
    cache = true;
    for (uint32_t i = 0; i < g_packCount; ++i) {
        Pack& pack = g_packs[i];
        if (guest < pack.start || guest > pack.end) continue;
        if (pack.state == 0) {
            if (++pack.calls < kLoadAfterCalls) {
                cache = false;
                return nullptr;
            }
            pack.state = Load(pack) ? 1 : 2;
        }
        if (pack.state != 1) return nullptr;
        uint32_t lo = 0, hi = pack.exportCount;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (pack.exports[mid].guest < guest) lo = mid + 1;
            else hi = mid;
        }
        if (lo < pack.exportCount && pack.exports[lo].guest == guest)
            return reinterpret_cast<Entry>(pack.base + pack.exports[lo].offset);
        return nullptr;
    }
    return nullptr;
}

}  // namespace

// The function of a pack for this guest address (its pack is loaded if need be), or null: not in a pack.
extern "C" void (*Brawl3dsPackFunction(uint32_t guest))(CpuContext*)
{
    if (g_packCount == 0) return nullptr;
    Slot& slot = g_slots[(guest >> 2) & (kSlots - 1)];
    if (slot.guest == guest) return slot.entry;
    bool cache = false;
    const Entry entry = Lookup(guest, cache);
    if (cache) {
        slot.guest = guest;
        slot.entry = entry;
    }
    return entry;
}

// Luma3DS's own system call 0x94: the whole instruction cache invalidated. Memory that ran one pack's code can
// then hold another's.
static void LumaInvalidateInstructionCache()
{
    __asm__ volatile("svc 0x94" ::: "r0", "r1", "r2", "r3", "r12", "lr", "memory");
}

// At a scene change (main_3ds.cpp): no fighter or stage code is running. If a pack did not fit since the last
// one, or the arena is more than half used on the way out of a match, every pack is dropped and the arena
// starts over; the next match loads what it uses.
extern "C" void Brawl3dsPacksSceneChange(int leavingMatch)
{
    if (g_packCount == 0 || !g_canReset || g_arenaUsed == 0) return;
    if (!g_wantReset && !(leavingMatch && g_arenaUsed > g_arenaSize / 2)) return;
    svcControlProcessMemory(g_process, reinterpret_cast<uint32_t>(g_arena), 0, g_arenaSize, MEMOP_PROT,
                            MEMPERM_READ | MEMPERM_WRITE);
    for (uint32_t i = 0; i < g_packCount; ++i) {
        Pack& pack = g_packs[i];
        if (pack.state == 1) std::free(pack.exports);
        pack.exports = nullptr;
        pack.base = nullptr;
        pack.state = 0;
        pack.calls = 0;
    }
    std::memset(g_slots, 0, sizeof(g_slots));
    if (g_lumaIcache) LumaInvalidateInstructionCache();
    std::fprintf(stderr, "[packs] arena emptied at a scene change (%lu KiB were in use, %lu packs)\n",
                 static_cast<unsigned long>(g_arenaUsed >> 10), static_cast<unsigned long>(g_loaded));
    g_arenaUsed = 0;
    g_loaded = 0;
    g_wantReset = false;
    g_arenaFullLogged = false;
    ++g_resets;
}

// Whether the arena may be emptied. On Luma3DS, system call 0x94 is tried once: packs/icache_probe.txt keeps the
// outcome, and "calling" left there means the call ended the process (it is not tried again).
static void ProbeReset()
{
    if (std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/packs/force_reset.txt", "r")) {  // emulator tests
        std::fclose(f);
        g_canReset = true;
        return;
    }
    // Off unless asked for (packs/allow_reset.txt): in the emulator, emptying the arena after a match and
    // loading a fighter again for the results screen hung the game (2026-11-24; the emulator has no way to
    // drop its translations of the old code, so that test says nothing about the console either way).
    if (std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/packs/allow_reset.txt", "r")) {
        std::fclose(f);
    } else {
        return;
    }
    s64 luma = 0;
    if (R_FAILED(svcGetSystemInfo(&luma, 0x10000, 0))) {
        std::fprintf(stderr, "[packs] not on Luma3DS: the arena is filled once\n");
        return;
    }
    const char* path = "sdmc:/3ds/brawl3ds/packs/icache_probe.txt";
    char line[40] = "";
    if (std::FILE* f = std::fopen(path, "r")) {
        std::fgets(line, sizeof(line), f);
        std::fclose(f);
    } else {
        if (std::FILE* w = std::fopen(path, "w")) {
            std::fputs("calling\n", w);
            std::fclose(w);
        }
        LumaInvalidateInstructionCache();
        std::strcpy(line, "ok");
        if (std::FILE* w = std::fopen(path, "w")) {
            std::fputs("ok\n", w);
            std::fclose(w);
        }
    }
    g_lumaIcache = g_canReset = std::strncmp(line, "ok", 2) == 0;
    std::fprintf(stderr, "[packs] instruction cache invalidation (Luma3DS svc 0x94): %s\n",
                 g_canReset ? "available, the arena is emptied between matches" : "not available, the arena is filled once");
}

// Reads the index and sets the arena aside: at frame 60, after the renderer took its own buffers (its command ring
// and texture decode buffer are megabytes too), while the heap still has megabytes in one piece.
extern "C" void Brawl3dsPacksInit()
{
    std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/packs/index.bin", "rb");
    if (!f) {
        std::fprintf(stderr, "[packs] no sdmc:/3ds/brawl3ds/packs/index.bin: no code packs\n");
        return;
    }
    struct {
        char magic[4];
        uint32_t version, probes[4], count;
    } head{};
    const uint32_t mine[4] = {reinterpret_cast<uint32_t>(&Brawl3ds_NotLinked),
                              reinterpret_cast<uint32_t>(static_cast<void (*)(uint32_t, CpuContext*)>(&InvokeIndirectCpu)),
                              reinterpret_cast<uint32_t>(static_cast<void* (*)(void*, const void*, size_t)>(&std::memcpy)),
                              reinterpret_cast<uint32_t>(&g_mkwGuestCodePatched[0])};
    bool ok = ReadAll(f, &head, sizeof(head)) && std::memcmp(head.magic, "B3PI", 4) == 0 && head.version == 1 &&
              head.count <= kMaxPacks;
    if (ok && std::memcmp(head.probes, mine, sizeof(mine)) != 0) {
        std::fprintf(stderr, "[packs] the packs on the SD card were built for another executable: not used\n");
        ok = false;
    }
    for (uint32_t i = 0; ok && i < head.count; ++i) {
        struct {
            uint32_t start, end;
            char name[24];
        } e{};
        ok = ReadAll(f, &e, sizeof(e));
        g_packs[i].start = e.start;
        g_packs[i].end = e.end;
        std::memcpy(g_packs[i].name, e.name, sizeof(e.name));
        g_packs[i].name[sizeof(e.name) - 1] = 0;
    }
    std::fclose(f);
    if (!ok) return;
    if (R_FAILED(svcDuplicateHandle(&g_process, CUR_PROCESS_HANDLE))) g_process = CUR_PROCESS_HANDLE;
    // The arena is the top of the heap mapping, kept out of malloc's reach (main_3ds.cpp, __system_allocateHeaps).
    g_arena = g_brawl3dsPackArena;
    g_arenaSize = g_brawl3dsPackArenaSize;
    if (!g_arena || g_arenaSize == 0) {
        std::fprintf(stderr, "[packs] no arena in this memory layout: no code packs\n");
        return;
    }
    g_packCount = head.count;
    ProbeReset();
    std::fprintf(stderr, "[packs] %lu packs indexed, arena %lu KiB at %08lX\n", static_cast<unsigned long>(g_packCount),
                 static_cast<unsigned long>(g_arenaSize >> 10),
                 static_cast<unsigned long>(reinterpret_cast<uint32_t>(g_arena)));
}

extern "C" void Brawl3dsPacksStats(unsigned* loaded, unsigned* usedKiB, unsigned* arenaKiB)
{
    *loaded = g_loaded;
    *usedKiB = g_arenaUsed >> 10;
    *arenaKiB = g_arenaSize >> 10;
}

// The unwinder's table for a program counter: a pack's own when it is in one, else the executable's (libgcc
// calls this when it is defined; C++ exceptions and thread exits unwind through translated frames).
extern "C" _Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int* count)
{
    for (uint32_t i = 0; i < g_packCount; ++i) {
        const Pack& pack = g_packs[i];
        if (pack.state == 1 && pc >= reinterpret_cast<_Unwind_Ptr>(pack.base) &&
            pc < reinterpret_cast<_Unwind_Ptr>(pack.base) + pack.rxSize) {
            *count = static_cast<int>(pack.exidxCount);
            return reinterpret_cast<_Unwind_Ptr>(pack.base + pack.exidxStart);
        }
    }
    *count = static_cast<int>((__exidx_end - __exidx_start) / 8);
    return reinterpret_cast<_Unwind_Ptr>(__exidx_start);
}
