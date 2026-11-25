// Game functions replaced by ARM code (tools/apply_natives.py makes their translated bodies a call to these).
//
// The SDK's matrix functions are written in paired-single instructions, which the translator emulates a lane at a
// time through doubles; here they are plain VFP arithmetic on the same guest memory. Results match the console's
// to the last bit wherever a double-precision multiply-add rounds like its fused one (always, short of a tie).
// A function's volatile registers (r0, r3-r12, f0-f13) are not reproduced: callers cannot use them.
#define BRAWL3DS_INLINE_GUEST_ACCESS 1
#include <cstdint>
#include <cstring>

#include "ppc_runtime.h"
#include "abi_bridge.h"
#include "memory.h"

// The indirect calls' own table (abi_bridge.h, brawl3ds-indirect-slots).
Brawl3dsFastIndirectSlot g_brawl3dsFastIndirect[kBrawl3dsFastIndirectSlots];

namespace {

inline float LoadF(uint32_t address)
{
    const uint32_t bits = MemoryInline::Brawl3dsLoadInline<uint32_t>(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline void StoreF(uint32_t address, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    MemoryInline::Brawl3dsStoreInline<uint32_t>(address, bits);
}
// A 3x4 matrix is 48 bytes: one page lookup for all of it when it does not straddle a (1 MiB) page.
inline void LoadMtx(uint32_t address, float m[12])
{
    const uintptr_t bias = MemoryInline::g_fullReadablePageBias[address >> MemoryInline::kPageShift];
    if (bias != 0 && (address & MemoryInline::kPageMask) <= MemoryInline::kPageSize - 48u) [[likely]] {
        const uint8_t* p = reinterpret_cast<const uint8_t*>((bias - 1u) + address);
        for (int i = 0; i < 12; ++i) {
            uint32_t bits;
            std::memcpy(&bits, p + 4 * i, 4);
            bits = __builtin_bswap32(bits);
            std::memcpy(&m[i], &bits, 4);
        }
        return;
    }
    for (int i = 0; i < 12; ++i) m[i] = LoadF(address + 4u * static_cast<uint32_t>(i));
}
inline void StoreMtx(uint32_t address, const float m[12])
{
    const uintptr_t bias = MemoryInline::g_fullWritablePageBias[address >> MemoryInline::kPageShift];
    if (bias != 0 && (address & MemoryInline::kPageMask) <= MemoryInline::kPageSize - 48u) [[likely]] {
        uint8_t* p = reinterpret_cast<uint8_t*>((bias - 1u) + address);
        for (int i = 0; i < 12; ++i) {
            uint32_t bits;
            std::memcpy(&bits, &m[i], 4);
            bits = __builtin_bswap32(bits);
            std::memcpy(p + 4 * i, &bits, 4);
        }
        return;
    }
    for (int i = 0; i < 12; ++i) StoreF(address + 4u * static_cast<uint32_t>(i), m[i]);
}
// ps_madd: a * b + c rounded once.
inline float Madd(float a, float b, float c)
{
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c));
}

// PSMTXConcat's own order: each row of `a` times the columns of `b`, the translation last.
inline void Concat(const float a[12], const float b[12], float out[12])
{
    for (int i = 0; i < 3; ++i) {
        const float a0 = a[4 * i], a1 = a[4 * i + 1], a2 = a[4 * i + 2], a3 = a[4 * i + 3];
        for (int j = 0; j < 4; ++j) {
            float acc = b[j] * a0;
            acc = Madd(b[4 + j], a1, acc);
            acc = Madd(b[8 + j], a2, acc);
            if (j == 3) acc = Madd(1.0f, a3, acc);
            out[4 * i + j] = acc;
        }
    }
}

}  // namespace

// void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab)
extern "C" void Brawl3dsNative_801EC1B8(CpuContext* ctx)
{
    float a[12], b[12], out[12];
    LoadMtx(ctx->gpr[3], a);
    LoadMtx(ctx->gpr[4], b);
    Concat(a, b, out);
    StoreMtx(ctx->gpr[5], out);
}

// void PSMTXConcatArray(const Mtx a, const Mtx* srcBase, Mtx* dstBase, u32 count)
extern "C" void Brawl3dsNative_801EC284(CpuContext* ctx)
{
    float a[12], b[12], out[12];
    LoadMtx(ctx->gpr[3], a);
    uint32_t src = ctx->gpr[4], dst = ctx->gpr[5];
    for (uint32_t n = ctx->gpr[6]; n != 0; --n, src += 48, dst += 48) {
        LoadMtx(src, b);
        Concat(a, b, out);
        StoreMtx(dst, out);
    }
}

// void __fill_mem(void* dst, int value, size_t count): what memset is made of.
extern "C" void Brawl3dsNative_80004388(CpuContext* ctx)
{
    uint32_t address = ctx->gpr[3], count = ctx->gpr[5];
    const uint8_t value = static_cast<uint8_t>(ctx->gpr[4]);
    while (count != 0) {
        const uint32_t inPage = MemoryInline::kPageSize - (address & MemoryInline::kPageMask);
        const uint32_t run = count < inPage ? count : inPage;
        const uintptr_t bias = MemoryInline::g_fullWritablePageBias[address >> MemoryInline::kPageShift];
        if (bias != 0) {
            std::memset(reinterpret_cast<uint8_t*>((bias - 1u) + address), value, run);
        } else {  // code, MMIO or unmapped: the checked path, a byte at a time
            for (uint32_t i = 0; i < run; ++i) Memory::Write8(address + i, value);
        }
        address += run;
        count -= run;
    }
}

// void AXFXReverbHiExpCallback(AXFX_BUFFERUPDATE*, AXFX_REVERBHI_EXP*): the reverb on the game's auxiliary bus,
// left out. Leaving the options menu restarts the sound system, and on the console the callback then ran once more
// over delay lines already given back (a data abort in its translated code, 2026-11-24). Without it the auxiliary
// send comes back dry; it also was 1-2% of the game thread.
extern "C" void Brawl3dsNative_80202A58(CpuContext*) {}
