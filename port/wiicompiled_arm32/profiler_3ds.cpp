// Sampling profiler for the headless timing build (New 3DS).
//
// A thread above the main thread's priority, on the same core, wakes every ~1 ms. Because it preempts the main
// thread, each wake-up sees the guest state exactly where the game was: the guest link register (the return address
// of the latest guest call, so it lands inside the function running now, or in its caller when that function is a
// leaf) and the guest stack's back chain (the saved return addresses of every caller). Samples are counted per
// address in fixed tables: no malloc and no locks in the sampler. Brawl3dsProfilerWrite folds addresses into guest
// functions and writes the "self" ranking (the function itself) and the "inclusive" ranking (it and everything it
// calls), with the game's symbol names.

#include <3ds.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "abi_bridge.h"
#include "isa/ppc_isa_context.h"
#include "memory.h"

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

namespace {

constexpr uint32_t kTableSize = 1u << 16;  // distinct guest addresses; 768 KiB
constexpr int kMaxFrames = 32;

struct Slot {
    uint32_t address;
    uint32_t self;
    uint32_t inclusive;
};
Slot g_table[kTableSize];

CpuContext* volatile* g_slot = nullptr;
volatile bool g_recording = false;
uint32_t g_samples = 0, g_noContext = 0, g_tableFull = 0, g_depthSum = 0;

Slot* Find(uint32_t address)
{
    uint32_t h = (address >> 2) * 2654435761u;
    for (uint32_t i = 0; i < kTableSize; ++i) {
        Slot& s = g_table[(h + i) & (kTableSize - 1)];
        if (s.address == address) return &s;
        if (s.address == 0) {
            s.address = address;
            return &s;
        }
    }
    return nullptr;
}

bool ReadGuest32(uint32_t address, uint32_t& value)
{
    return MemoryInline::TryReadMappedScalar(address, value);
}

void TakeSample()
{
    const CpuContext* ctx = g_slot ? *g_slot : nullptr;
    if (!ctx) {
        ++g_noContext;
        return;
    }
    ++g_samples;
    uint32_t chain[kMaxFrames + 1];
    int n = 0;
    chain[n++] = ctx->lr;
    uint32_t sp = ctx->gpr[1];
    for (int frame = 0; frame < kMaxFrames; ++frame) {
        uint32_t prev = 0, saved = 0;
        if (!ReadGuest32(sp, prev) || prev <= sp || prev - sp > 0x20000u) break;
        if (!ReadGuest32(prev + 4, saved) || saved == 0) break;
        chain[n++] = saved;
        sp = prev;
    }
    g_depthSum += n;
    if (Slot* s = Find(chain[0])) ++s->self; else ++g_tableFull;
    for (int i = 0; i < n; ++i) {
        bool seen = false;
        for (int j = 0; j < i && !seen; ++j) seen = chain[j] == chain[i];
        if (seen) continue;
        if (Slot* s = Find(chain[i])) ++s->inclusive; else ++g_tableFull;
    }
}

void SamplerThread(void*)
{
    for (;;) {
        svcSleepThread(1000000LL);  // 1 ms
        if (g_recording) TakeSample();
    }
}

// The guest function an address belongs to: the translated-function registry's entry, or the game's symbol map
// when its floor is closer (the same preference as the runtime's crash dumps).
std::pair<uint32_t, std::string> FunctionOf(uint32_t address)
{
    uint32_t start = 0;
    std::string name;
    if (auto info = TranslatedFunctionRegistry::FindNearestByAddress(address)) {
        if (address - info->address < 0x10000u) {
            start = info->address;
            name = info->name ? info->name : "";
        }
    }
    if (kGuestMapSymbolCount) {
        const uint32_t* end = kGuestMapSymbolAddresses + kGuestMapSymbolCount;
        const uint32_t* it = std::upper_bound(kGuestMapSymbolAddresses, end, address);
        if (it != kGuestMapSymbolAddresses) {
            const uint32_t idx = static_cast<uint32_t>(it - kGuestMapSymbolAddresses) - 1;
            const uint32_t mapStart = kGuestMapSymbolAddresses[idx];
            if (address - mapStart < 0x10000u && (start == 0 || mapStart >= start)) {
                start = mapStart;
                name = kGuestMapSymbolNames[idx];
            }
        }
    }
    if (start == 0) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "?%08lX", static_cast<unsigned long>(address & ~0xFFu));
        return {address & ~0xFFu, buf};
    }
    return {start, name};
}

}  // namespace

void Brawl3dsProfilerStart(CpuContext* volatile* mainContextSlot)
{
    g_slot = mainContextSlot;
    // Priority 0x19: above the main thread (0x30), just below the heartbeat (0x18). Same core as the main thread
    // (-2 = the process's default core), so a wake-up preempts the game where it is.
    threadCreate(SamplerThread, nullptr, 16 * 1024, 0x19, -2, true);
}

// Call counts of the hot shards' functions (abi_bridge.h's MKW_GUEST_CODE_GUARD, patched by
// tools/patch_wiicompiled_arm32.py), indexed by the translator's function-table index (~105,500 in Brawl).
struct Brawl3dsCallCount {
    uint32_t count;
    uint32_t address;
};
constexpr uint32_t kCallCountSlots = 1u << 17;
Brawl3dsCallCount g_brawl3dsCallCounts[kCallCountSlots];

void Brawl3dsProfilerSetRecording(bool on)
{
    if (on) {
        for (auto& c : g_brawl3dsCallCounts) c.count = 0;  // count the match only
    }
    g_recording = on;
}

void Brawl3dsProfilerWrite(const char* path, unsigned frames, double averageFrameMs)
{
    g_recording = false;
    struct Agg {
        std::string name;
        uint32_t self = 0, inclusive = 0;
    };
    std::map<uint32_t, Agg> byFunction;
    std::map<uint32_t, uint32_t> selfByAddress;
    for (const Slot& s : g_table) {
        if (!s.address) continue;
        auto [start, name] = FunctionOf(s.address);
        Agg& a = byFunction[start];
        if (a.name.empty()) a.name = name;
        a.self += s.self;
        a.inclusive += s.inclusive;
        if (s.self) selfByAddress[s.address] += s.self;
    }
    std::vector<std::pair<uint32_t, const Agg*>> rows;
    for (const auto& [start, a] : byFunction) rows.emplace_back(start, &a);

    std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    const double total = g_samples ? g_samples : 1;
    std::fprintf(f,
                 "brawl3ds sampling profile of the VS demo match\n"
                 "%u samples over %u frames (%.2f ms per frame), %u with no guest context, average stack depth %.1f,"
                 " %u lost (table full)\n"
                 "ms/frame = the share of the samples times the average frame time.\n"
                 "self = the sampled return address was inside the function (its own code, plus runtime code it calls"
                 " directly, e.g. HLE GX/OS functions; a leaf's time lands in its caller).\n"
                 "inclusive = the function was anywhere on the guest call stack.\n",
                 g_samples, frames, averageFrameMs, g_noContext, g_samples ? double(g_depthSum) / g_samples : 0.0,
                 g_tableFull);
    const auto print = [&](const char* title, bool self, size_t count) {
        std::sort(rows.begin(), rows.end(), [self](const auto& x, const auto& y) {
            return (self ? x.second->self : x.second->inclusive) > (self ? y.second->self : y.second->inclusive);
        });
        std::fprintf(f, "\n== %s ==\n    %%   ms/frame  address   function\n", title);
        for (size_t i = 0; i < rows.size() && i < count; ++i) {
            const uint32_t v = self ? rows[i].second->self : rows[i].second->inclusive;
            if (!v) break;
            std::fprintf(f, "%6.2f %8.2f  %08lX  %s\n", 100.0 * v / total, v / total * averageFrameMs,
                         static_cast<unsigned long>(rows[i].first), rows[i].second->name.c_str());
        }
    };
    print("self, top 80", true, 80);
    print("inclusive, top 120", false, 120);

    std::vector<std::pair<uint32_t, uint32_t>> hot(selfByAddress.begin(), selfByAddress.end());
    std::sort(hot.begin(), hot.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    std::fprintf(f, "\n== hottest return addresses, top 60 ==\n");
    for (size_t i = 0; i < hot.size() && i < 60; ++i) {
        auto [start, name] = FunctionOf(hot[i].first);
        std::fprintf(f, "%6.2f  %08lX  %s+0x%lX\n", 100.0 * hot[i].second / total,
                     static_cast<unsigned long>(hot[i].first), name.c_str(),
                     static_cast<unsigned long>(hot[i].first - start));
    }

    // Call counts (hot shards only). calls/frame says how often a function runs; for the self ranking's functions,
    // us/call = self time per call (a function whose own time is mostly call overhead shows up as a tiny us/call
    // with a huge call count).
    std::map<uint32_t, uint64_t> callsByAddress;
    for (const auto& c : g_brawl3dsCallCounts) {
        if (c.count) callsByAddress[c.address] += c.count;
    }
    std::vector<std::pair<uint32_t, uint64_t>> calls(callsByAddress.begin(), callsByAddress.end());
    std::sort(calls.begin(), calls.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    uint64_t allCalls = 0;
    for (const auto& c : calls) allCalls += c.second;
    std::fprintf(f, "\n== most called functions (hot shards only: %zu functions, %.0f calls per frame), top 80 ==\n"
                    "calls/frame  self ms/frame  us/call  address   function\n",
                 calls.size(), frames ? double(allCalls) / frames : 0.0);
    for (size_t i = 0; i < calls.size() && i < 80; ++i) {
        const auto it = byFunction.find(calls[i].first);
        const double selfMs = it != byFunction.end() ? it->second.self / total * averageFrameMs : 0.0;
        const double perFrame = frames ? double(calls[i].second) / frames : 0.0;
        std::fprintf(f, "%11.0f  %13.2f  %7.2f  %08lX  %s\n", perFrame, selfMs,
                     perFrame > 0 ? selfMs * 1000.0 / perFrame : 0.0, static_cast<unsigned long>(calls[i].first),
                     FunctionOf(calls[i].first).second.c_str());
    }
    std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.second->self > y.second->self; });
    std::fprintf(f, "\n== self ranking with call counts, top 40 (0 calls = not in a hot shard) ==\n"
                    "self ms/frame  calls/frame  us/call  address   function\n");
    for (size_t i = 0; i < rows.size() && i < 40; ++i) {
        const double selfMs = rows[i].second->self / total * averageFrameMs;
        const auto it = callsByAddress.find(rows[i].first);
        const double perFrame = it != callsByAddress.end() && frames ? double(it->second) / frames : 0.0;
        std::fprintf(f, "%13.2f  %11.0f  %7.2f  %08lX  %s\n", selfMs, perFrame,
                     perFrame > 0 ? selfMs * 1000.0 / perFrame : 0.0, static_cast<unsigned long>(rows[i].first),
                     rows[i].second->name.c_str());
    }
    std::fclose(f);
}
