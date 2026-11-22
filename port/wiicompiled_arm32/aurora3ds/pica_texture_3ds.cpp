// GX textures on the PICA200 (renderer milestone 3): the texture a GX texture map points at is decoded once
// (gx_texture_decode.cpp: any GX format, palettes included, to linear RGBA8 in one persistent scratch buffer),
// halved above 128 texels, packed into the PICA format closest to the source (LA4/LA8 for intensity formats,
// RGB565, RGBA5551, RGBA4, RGBA8) in the PICA's 8x8 Morton-order tiles, and kept in VRAM until a frame that does
// not use it needs the space.

#include "pica_internal.hpp"

#include "gx_texture_decode.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

extern "C" {
#include <3ds/allocator/linear.h>
#include <3ds/allocator/vram.h>
#include <3ds/gpu/gx.h>
#include <3ds/services/gspgpu.h>
#include <3ds/svc.h>
}

namespace brawl3ds_pica {
namespace {
using namespace aurora::gx;

struct TexKey {
    const void* data;
    const void* tlutData;
    uint32_t width, height, format;
    uint32_t tlutFormat, tlutEntries;
    uint32_t dataVersion, tlutVersion;
    bool operator==(const TexKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct TexKeyHash {
    size_t operator()(const TexKey& k) const noexcept
    {
        const auto* w = reinterpret_cast<const uint32_t*>(&k);
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < sizeof(TexKey) / 4; ++i) h = (h ^ w[i]) * 16777619u;
        return h;
    }
};
static_assert(sizeof(TexKey) % 4 == 0);

struct TexEntry {
    C3D_Tex tex;
    uint32_t bytes = 0;
    uint32_t lastUsedFrame = 0;
    float scaleS = 1.0f, scaleT = 1.0f;  // GX size / PICA size
    bool valid = false;                  // false: decoding failed, the unit gets the white texture
};

std::unordered_map<TexKey, TexEntry, TexKeyHash> g_cache;
uint32_t g_frame = 1;
size_t g_bytes = 0;
size_t g_budget = 0;
C3D_Tex g_white;
bool g_whiteReady = false;
// What each unit has bound (C3D_TexBind re-emits the unit's whole texture state).
const C3D_Tex* g_boundTex[3] = {};
u32 g_boundParam[3] = {};

// Statistics over the match (WriteTextureStats).
uint64_t g_statCreated = 0, g_statEvicted = 0, g_statFailed = 0, g_statUploadBytes = 0, g_statDecodeTicks = 0;
uint64_t g_statBinds = 0, g_statWhite = 0;
uint32_t g_statFormats[16] = {};
size_t g_statPeakBytes = 0;

uint32_t NextPow2(uint32_t v)
{
    uint32_t p = 8;
    while (p < v && p < 1024) p <<= 1;
    return p;
}

inline uint32_t Morton(uint32_t x, uint32_t y)
{
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

// Packs linear RGBA8 (w x h) into a PICA texture of W x H in `fmt`, padding with the edge texels.
void PackTiled(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t W, uint32_t H, GPU_TEXCOLOR fmt, uint8_t* out)
{
    const uint32_t bpp = fmt == GPU_RGBA8 ? 4 : (fmt == GPU_LA4 ? 1 : 2);
    // The PICA samples t = 0 from the last row in memory and t = 1 from the first, GX t = 0 from its first row:
    // memory row y holds GX row H - 1 - y (rows past the GX height repeat its last row). Unflipped, every texture
    // of the safety screen showed upside down.
    for (uint32_t y = 0; y < H; ++y) {
        const uint32_t gxRow = H - 1 - y;
        const uint8_t* row = rgba + static_cast<size_t>(std::min(gxRow, h - 1)) * w * 4;
        const uint32_t tileRow = (y >> 3) * (W >> 3);
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* p = row + std::min(x, w - 1) * 4;
            const uint32_t at = ((tileRow + (x >> 3)) * 64 + Morton(x & 7, y & 7)) * bpp;
            const uint32_t r = p[0], g = p[1], b = p[2], a = p[3];
            switch (fmt) {
            case GPU_RGBA8: {
                const uint32_t v = r << 24 | g << 16 | b << 8 | a;
                std::memcpy(out + at, &v, 4);
                break;
            }
            case GPU_RGB565: {
                const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                std::memcpy(out + at, &v, 2);
                break;
            }
            case GPU_RGBA5551: {
                const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128));
                std::memcpy(out + at, &v, 2);
                break;
            }
            case GPU_RGBA4: {
                const uint16_t v = static_cast<uint16_t>((r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | a >> 4);
                std::memcpy(out + at, &v, 2);
                break;
            }
            case GPU_LA8: {
                const uint16_t v = static_cast<uint16_t>(r << 8 | a);
                std::memcpy(out + at, &v, 2);
                break;
            }
            default:  // GPU_LA4
                out[at] = static_cast<uint8_t>((r >> 4) << 4 | a >> 4);
                break;
            }
        }
    }
}

bool HasAlpha(const uint8_t* rgba, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i)
        if (rgba[i * 4 + 3] != 255) return true;
    return false;
}

// The PICA format for a GX format (and palette format for C4/C8/C14X2).
GPU_TEXCOLOR PicaFormat(uint32_t gxFormat, uint32_t tlutFormat, const uint8_t* rgba, size_t pixels)
{
    switch (gxFormat) {
    case GX_TF_I4: case GX_TF_IA4: return GPU_LA4;
    case GX_TF_I8: case GX_TF_IA8: return GPU_LA8;
    case GX_TF_RGB565: return GPU_RGB565;
    case GX_TF_RGBA8: return GPU_RGBA8;
    case GX_TF_CMPR: return HasAlpha(rgba, pixels) ? GPU_RGBA5551 : GPU_RGB565;
    case GX_TF_C4: case GX_TF_C8: case GX_TF_C14X2:
        if (tlutFormat == GX_TL_IA8) return GPU_LA8;
        if (tlutFormat == GX_TL_RGB565) return GPU_RGB565;
        return HasAlpha(rgba, pixels) ? GPU_RGBA4 : GPU_RGB565;
    default:  // RGB5A3 and anything else
        return HasAlpha(rgba, pixels) ? GPU_RGBA4 : GPU_RGB565;
    }
}

size_t PicaBytes(GPU_TEXCOLOR fmt, uint32_t W, uint32_t H)
{
    const size_t px = static_cast<size_t>(W) * H;
    return fmt == GPU_RGBA8 ? px * 4 : fmt == GPU_LA4 ? px : px * 2;
}

void EnsureWhite()
{
    if (g_whiteReady) return;
    if (!C3D_TexInit(&g_white, 8, 8, GPU_RGBA8)) return;
    std::memset(g_white.data, 0xFF, 8 * 8 * 4);
    C3D_TexFlush(&g_white);
    C3D_TexSetFilter(&g_white, GPU_NEAREST, GPU_NEAREST);
    g_whiteReady = true;
}

void EvictFor(size_t needed)
{
    if (g_bytes + needed <= g_budget) return;
    std::vector<std::pair<uint32_t, TexKey>> old;
    for (auto& [key, e] : g_cache)
        if (e.lastUsedFrame < g_frame) old.emplace_back(e.lastUsedFrame, key);
    std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [frame, key] : old) {
        if (g_bytes + needed <= g_budget * 9 / 10) break;
        auto it = g_cache.find(key);
        if (it == g_cache.end()) continue;
        if (it->second.valid) C3D_TexDelete(&it->second.tex);
        for (auto& b : g_boundTex)
            if (b == &it->second.tex) b = nullptr;
        g_bytes -= it->second.bytes;
        g_cache.erase(it);
        ++g_statEvicted;
    }
}

// Textures live in VRAM (the ~5 MiB the framebuffers leave; linear memory is full of guest RAM). A texture is
// packed into a linear staging buffer and copied to VRAM by the GPU's transfer engine, queued on citro3d's GX queue
// ahead of the frame's draws. The staging buffer is reused each frame (the previous frame's queue has finished by
// then), so a frame uploads at most kStagingBytes in kMaxUploadsPerFrame copies; textures past that wait a frame.
constexpr size_t kStagingBytes = 256u * 1024u;
// Each upload is one entry on citro3d's GX command queue, which only starts running at the frame's end: with 16
// a frame (plus citro3d's own entries) the safety screen's burst of textures stalled the game in Azahar twice.
constexpr uint32_t kMaxUploadsPerFrame = 8;
uint8_t* g_staging = nullptr;
size_t g_stagingUsed = 0;
uint32_t g_uploadsThisFrame = 0;
uint64_t g_statDeferred = 0;

enum class CreateResult { kOk, kFailed, kDeferred };

// 2x2 box filter of linear RGBA8, in place (each output texel lies at or before the texels it is made of).
void HalveInPlace(uint8_t* rgba, uint32_t w, uint32_t h, uint32_t& outW, uint32_t& outH)
{
    outW = std::max(1u, w / 2), outH = std::max(1u, h / 2);
    for (uint32_t y = 0; y < outH; ++y) {
        const uint8_t* r0 = rgba + static_cast<size_t>(std::min(2 * y, h - 1)) * w * 4;
        const uint8_t* r1 = rgba + static_cast<size_t>(std::min(2 * y + 1, h - 1)) * w * 4;
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t x0 = std::min(2 * x, w - 1) * 4, x1 = std::min(2 * x + 1, w - 1) * 4;
            uint8_t px[4];
            for (int c = 0; c < 4; ++c)
                px[c] = static_cast<uint8_t>((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
            std::memcpy(rgba + (static_cast<size_t>(y) * outW + x) * 4, px, 4);
        }
    }
}

// The decode buffer: 1 MiB (512 x 512 RGBA8), allocated once. Larger textures are decoded at half size by
// sampling (see Create).
constexpr size_t kScratchBytes = 512u * 512u * 4u;
uint8_t* g_scratch = nullptr;

// Decodes one GX texture and queues its upload.
CreateResult Create(const GXTexObj_& obj, const GXTlutObj_* tlut, TexEntry& e)
{
    const uint32_t w = obj.width(), h = obj.height(), fmt = obj.format();
    if (!obj.data || w == 0 || h == 0 || w > 1024 || h > 1024) return CreateResult::kFailed;
    const bool paletted = fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2;
    if (paletted && (!tlut || !tlut->data || tlut->numEntries == 0)) return CreateResult::kFailed;
    if (!g_staging || g_uploadsThisFrame >= kMaxUploadsPerFrame) return CreateResult::kDeferred;

    // Above 128 texels the top screen (400x240) cannot show the full resolution: keep half.
    const bool half = (w > 128 || h > 128) && w >= 16 && h >= 16;
    const uint32_t W = NextPow2(w), H = NextPow2(h);
    const uint32_t picaW = half ? std::max(8u, W / 2) : W, picaH = half ? std::max(8u, H / 2) : H;
    // The PICA format depends on the texels (alpha or not); this check uses the largest (4 bytes a texel).
    if (g_stagingUsed > 0 && g_stagingUsed + static_cast<size_t>(picaW) * picaH * 4 > kStagingBytes)
        return CreateResult::kDeferred;

    if (!g_scratch || static_cast<size_t>(w) * h * 4 > kScratchBytes) return CreateResult::kFailed;  // over 512x512
    const uint64_t t0 = svcGetSystemTick();
    if (!DecodeGxTexture(fmt, static_cast<const uint8_t*>(obj.data), w, h,
                         tlut ? static_cast<const uint8_t*>(tlut->data) : nullptr, tlut ? tlut->format : 0,
                         tlut ? tlut->numEntries : 0, g_scratch))
        return CreateResult::kFailed;
    const uint8_t* rgba = g_scratch;
    uint32_t tw = w, th = h;
    if (half) HalveInPlace(g_scratch, w, h, tw, th);

    const GPU_TEXCOLOR pf = PicaFormat(fmt, paletted ? tlut->format : 0, rgba, static_cast<size_t>(tw) * th);
    const size_t bytes = PicaBytes(pf, picaW, picaH);
    if (g_stagingUsed + bytes > kStagingBytes) return CreateResult::kDeferred;
    EvictFor(bytes);
    if (!C3D_TexInitVRAM(&e.tex, static_cast<u16>(picaW), static_cast<u16>(picaH), pf)) {
        // VRAM full or fragmented: drop everything this frame does not use and try once more.
        const size_t keep = g_budget;
        g_budget = 0;
        EvictFor(bytes);
        g_budget = keep;
        if (!C3D_TexInitVRAM(&e.tex, static_cast<u16>(picaW), static_cast<u16>(picaH), pf))
            return CreateResult::kFailed;
    }
    uint8_t* staged = g_staging + g_stagingUsed;
    PackTiled(rgba, tw, th, picaW, picaH, pf, staged);
    GSPGPU_FlushDataCache(staged, bytes);
    GX_TextureCopy(reinterpret_cast<u32*>(staged), 0, static_cast<u32*>(e.tex.data), 0, bytes, 8);  // raw copy
    g_stagingUsed = (g_stagingUsed + bytes + 127) & ~static_cast<size_t>(127);
    ++g_uploadsThisFrame;

    e.bytes = static_cast<uint32_t>(bytes);
    // GX size over the padded size; halving keeps the ratio.
    e.scaleS = static_cast<float>(w) / static_cast<float>(W);
    e.scaleT = static_cast<float>(h) / static_cast<float>(H);
    g_bytes += bytes;
    g_statPeakBytes = std::max(g_statPeakBytes, g_bytes);
    g_statUploadBytes += bytes;
    g_statDecodeTicks += svcGetSystemTick() - t0;
    ++g_statCreated;
    ++g_statFormats[fmt & 15];
    return CreateResult::kOk;
}


void Bind(int unit, C3D_Tex* tex)
{
    if (unit < 0 || unit > 2) return;
    if (g_boundTex[unit] == tex && g_boundParam[unit] == tex->param) return;
    C3D_TexBind(unit, tex);
    g_boundTex[unit] = tex;
    g_boundParam[unit] = tex->param;
}

GPU_TEXTURE_WRAP_PARAM Wrap(GXTexWrapMode m)
{
    return m == GX_REPEAT ? GPU_REPEAT : m == GX_MIRROR ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE;
}
} // namespace

void TexturesBeginFrame()
{
    ++g_frame;
    g_stagingUsed = 0;
    g_uploadsThisFrame = 0;
    if (g_budget == 0) {
        // The VRAM the render target left, minus a little for citro3d.
        const size_t freeBytes = vramSpaceFree();
        const size_t reserve = 256u << 10;
        g_budget = freeBytes > reserve + (1u << 20) ? freeBytes - reserve : (1u << 20);
        g_staging = static_cast<uint8_t*>(linearMemAlign(kStagingBytes, 128));
        g_scratch = static_cast<uint8_t*>(std::malloc(kScratchBytes));
        std::fprintf(stderr,
                     "[pica] texture cache: %u KiB VRAM free, budget %u KiB, staging %s, decode buffer %s, %u KiB "
                     "linear free\n",
                     static_cast<unsigned>(freeBytes >> 10), static_cast<unsigned>(g_budget >> 10),
                     g_staging ? "ok" : "FAILED", g_scratch ? "ok" : "FAILED",
                     static_cast<unsigned>(linearSpaceFree() >> 10));
    }
}

bool BindTexture(int unit, int texMap, float& scaleS, float& scaleT)
{
    scaleS = scaleT = 1.0f;
    EnsureWhite();
    ++g_statBinds;
    if (texMap < 0 || texMap >= static_cast<int>(MaxTextures)) {
        ++g_statWhite;
        if (g_whiteReady) Bind(unit, &g_white);
        return false;
    }
    const GXTexObj_& obj = g_gxState.loadedTextures[texMap];
    const uint32_t fmt = obj.format();
    const bool paletted = fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2;
    const GXTlutObj_* tlut = nullptr;
    if (paletted && static_cast<uint32_t>(obj.tlut) < MaxTluts) tlut = &g_gxState.loadedTluts[obj.tlut];

    TexKey key{};
    key.data = obj.data;
    key.width = obj.width();
    key.height = obj.height();
    key.format = fmt;
    key.dataVersion = obj.texDataVersion;
    if (tlut) {
        key.tlutData = tlut->data;
        key.tlutFormat = tlut->format;
        key.tlutEntries = tlut->numEntries;
        key.tlutVersion = tlut->tlutDataVersion;
    }
    auto [it, inserted] = g_cache.try_emplace(key);
    TexEntry& e = it->second;
    if (inserted) {
        const CreateResult r = Create(obj, tlut, e);
        if (r == CreateResult::kDeferred) {  // try again next frame
            g_cache.erase(it);
            ++g_statDeferred;
            ++g_statWhite;
            if (g_whiteReady) Bind(unit, &g_white);
            return false;
        }
        e.valid = r == CreateResult::kOk;
        if (!e.valid) ++g_statFailed;
    }
    e.lastUsedFrame = g_frame;
    if (!e.valid) {
        ++g_statWhite;
        if (g_whiteReady) Bind(unit, &g_white);
        return false;
    }
    C3D_TexSetFilter(&e.tex, obj.mag_filter() == GX_LINEAR ? GPU_LINEAR : GPU_NEAREST,
                     obj.min_filter() == GX_NEAR || obj.min_filter() == GX_NEAR_MIP_NEAR ||
                             obj.min_filter() == GX_NEAR_MIP_LIN
                         ? GPU_NEAREST
                         : GPU_LINEAR);
    C3D_TexSetWrap(&e.tex, Wrap(obj.wrap_s()), Wrap(obj.wrap_t()));
    Bind(unit, &e.tex);
    scaleS = e.scaleS;
    scaleT = e.scaleT;
    return true;
}

void WriteTextureStats(std::FILE* f, unsigned frames)
{
    const double div = frames ? frames : 1;
    if (f) {
    std::fprintf(f,
                 "textures: %llu created (%.1f per frame, %.2f ms decode per frame), %llu evicted, %llu failed, %llu deferred; "
                 "%zu cached (%zu KiB, peak %zu KiB, budget %zu KiB); %.0f binds per frame, %.0f white\n",
                 static_cast<unsigned long long>(g_statCreated), g_statCreated / div,
                 g_statDecodeTicks / div / (SYSCLOCK_ARM11 / 1000.0), static_cast<unsigned long long>(g_statEvicted),
                 static_cast<unsigned long long>(g_statFailed), static_cast<unsigned long long>(g_statDeferred), g_cache.size(), g_bytes >> 10, g_statPeakBytes >> 10,
                 g_budget >> 10, g_statBinds / div, g_statWhite / div);
    std::fprintf(f, "texture formats created:");
    static const char* kNames[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "7",
                                     "C4", "C8", "C14X2", "11", "12", "13", "CMPR", "15"};
    for (int i = 0; i < 16; ++i)
        if (g_statFormats[i]) std::fprintf(f, " %s %u", kNames[i], g_statFormats[i]);
    std::fprintf(f, "\n");
    }
    g_statCreated = g_statEvicted = g_statFailed = g_statUploadBytes = g_statDecodeTicks = g_statBinds = g_statWhite = g_statDeferred = 0;
}

} // namespace brawl3ds_pica
