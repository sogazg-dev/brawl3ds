// PICA200 draw path for Aurora's GX front end on the 3DS (docs/RENDERER_3DS.md). Milestone 3: textures, TEV
// (pica_tev_3ds.cpp), alpha test, blending, depth and culling.
//
// A batch is one full draw from Aurora (backend_3ds.cpp's push_draw_command: a GX state change, with its
// triangle-list indices) plus the unchanged-state draws appended to it (brawl3ds_append_draw). At a batch's start
// the GX state is translated (ApplyMaterial), its textures bound (pica_texture_3ds.cpp) and the GX matrices
// uploaded. Each GX vertex is decoded on the CPU from the current vertex format into position, channel color,
// matrix row and up to two texture-coordinate sets; the vertex shader (gx_m4.v.pica) applies the GX position
// matrix, the projection and the texture-coordinate generators. Batches go to citro3d as indexed triangle lists
// into per-frame linear-memory buffers; the frame begins at its first draw and ends in Brawl3dsPicaEndFrame.
// Milestone 4: GX per-vertex lighting of channel COLOR0A0 in the vertex shader (normals from the GX vertex,
// lights as uniforms, UploadLighting); fog (pica_tev_3ds.cpp); a second texture-coordinate set and coordinates from
// the normal. A batch that reads two sets uses a 28-byte vertex (PicaVertex2) in the same buffer; all others keep
// the 24-byte one, so linear memory is spent only where needed.
// The GX viewport is folded into the projection (a pass drawn into a corner of the EFB lands in the same corner of
// the screen, and its depth range maps like GX's); an EFB texture copy that clears clears its rectangle with a quad
// (Brawl3dsPicaClearRect). The copy itself is not made yet (milestone 6): its texture reads what guest memory holds.
// Not yet: channel COLOR1A1 (drawn as COLOR0A0), indirect textures, the GX scissor, EFB copies.
//
// Milestone 5: the GPU side on core 2. The game thread (core 0) still reads the GX state: it translates materials,
// works out the uniforms and plans how to read each draw's vertices, but it no longer calls citro3d. It records
// what changed (combiner stages, fragment state, fog, uniform vectors, texture objects) and the draws into a
// command ring. A worker thread on the New 3DS's core 2 owns citro3d: it decodes and uploads textures, decodes the
// vertices into the frame's buffers and builds and submits the GPU commands, while the game thread goes on with
// the frame. At the frame's end the game thread waits for the worker (the GX draw-done the game expects; Aurora's
// staging arena, which full draws' vertices live in, is reset right after). Appended draws' vertices can live in
// the runtime's FIFO buffer, which is reused as soon as the call returns: they are copied into the ring. Without
// core 2 (or with debug_flags.txt bit 32) the same commands run on the game thread as they are recorded.

#include "pica_internal.hpp"
#include "brawl3ds_cp_hooks.h"
#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"

#include <3ds/synchronization.h>
#include <3ds/thread.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

extern "C" const unsigned char brawl3ds_gx_m4_shbin[];
extern "C" const unsigned int brawl3ds_gx_m4_shbin_size;

// What the renderer is doing, for main_3ds.cpp's stall report (heartbeat thread).
extern "C" const char* volatile g_brawl3dsRenderPhase = "not started";

// Vertex state writes on the worker (Aurora's handle_cp, the GX calls run there): see g_recPlanGen.
extern "C" unsigned g_brawl3dsVtxGen = 0;
// Set by the runtime around a GXCallDisplayList of a list in guest memory (gx_dl.cpp, brawl3ds-dl-ref).
extern "C" unsigned char g_brawl3dsListInGuestRam = 0;

namespace {
using namespace aurora::gx;
using namespace brawl3ds_pica;

struct PicaVertex {
    float x, y, z;
    uint8_t r, g, b, a;   // the material color of GX channel COLOR0A0 (the shader applies the lighting)
    int8_t row;           // GX matrix row of the position matrix (0, 3, ..., 27)
    int8_t n[3];          // object-space normal x 127 (lit batches only)
    int16_t uv[2];        // one texture-coordinate set in 1/512 steps (+-64; MaterialPlan::uvAttr[0])
};
static_assert(sizeof(PicaVertex) == 24);
// The layout of batches that read two texture-coordinate sets: set B after the rest.
struct PicaVertex2 : PicaVertex {
    int16_t uv2[2];       // MaterialPlan::uvAttr[1], 1/512 steps
};
static_assert(sizeof(PicaVertex2) == 28);
constexpr float kUvScale = 512.0f;  // gx_m4.v.pica multiplies by 1/512
// The EFB the game draws into (aurora3ds's configured_fb_size): it maps onto the whole top screen.
constexpr float kEfbWidth = 640.0f, kEfbHeight = 456.0f;
constexpr uint32_t kMaxLights = 8;   // gx_m4.v.pica's lights[40]: 5 vectors each

// Linear memory is nearly all guest RAM: these buffers, citro3d's command buffer and the texture staging buffer
// have to fit in the ~3.5 MiB it leaves (main_3ds.cpp's kLinearHeap).
constexpr uint32_t kMaxVertices = 78 * 1024;   // per frame at 24 bytes; Brawl's match draws up to ~74,400
constexpr uint32_t kVertexBytes = kMaxVertices * sizeof(PicaVertex);
constexpr uint32_t kMaxIndices = 160 * 1024;
constexpr uint32_t kDisplayFlags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

// ============================================================================================================
// Shared by both sides: the shader (parsed on the game thread, bound by the worker), vertex reading plans and the
// vertex decoders (pure functions of a plan).
// ============================================================================================================

DVLB_s* g_dvlb = nullptr;
shaderProgram_s g_program;
int g_projLoc = -1, g_mtxLoc = -1, g_texMtxLoc = -1, g_texSrcLoc = -1;
int g_ambLoc = -1, g_lselLoc = -1, g_lightsLoc = -1, g_lightLoopLoc = -1, g_litLoc = -1;
int g_fogpLoc = -1, g_fogvLoc = -1, g_twosetLoc = -1;

// Unaligned loads: the build passes -munaligned-access, and memcpy of 2 or 4 bytes becomes one ldrh/ldr (never the
// ldm that faults on the console).
inline uint16_t Load16(const uint8_t* p, bool be)
{
    uint16_t v;
    std::memcpy(&v, p, 2);
    return be ? __builtin_bswap16(v) : v;
}
inline uint32_t Load32(const uint8_t* p, bool be)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return be ? __builtin_bswap32(v) : v;
}
inline float LoadComponent(const uint8_t* p, GXCompType type, float scale, bool be)
{
    switch (type) {
    case GX_U8:
        return p[0] * scale;
    case GX_S8:
        return static_cast<int8_t>(p[0]) * scale;
    case GX_U16:
        return Load16(p, be) * scale;
    case GX_S16:
        return static_cast<int16_t>(Load16(p, be)) * scale;
    default: {
        const uint32_t bits = Load32(p, be);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    }
}
inline void LoadColor(const uint8_t* p, GXCompType type, bool be, uint8_t out[4])
{
    switch (type) {
    case GX_RGB565: {
        const uint16_t c = Load16(p, be);
        out[0] = static_cast<uint8_t>((c >> 11) * 255 / 31);
        out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
        out[3] = 255;
        break;
    }
    case GX_RGB8:
    case GX_RGBX8:
        out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255;
        break;
    case GX_RGBA4: {
        const uint16_t c = Load16(p, be);
        out[0] = static_cast<uint8_t>((c >> 12) * 17), out[1] = static_cast<uint8_t>(((c >> 8) & 15) * 17);
        out[2] = static_cast<uint8_t>(((c >> 4) & 15) * 17), out[3] = static_cast<uint8_t>((c & 15) * 17);
        break;
    }
    case GX_RGBA6: {
        const uint32_t c = uint32_t{p[0]} << 16 | uint32_t{p[1]} << 8 | p[2];
        out[0] = static_cast<uint8_t>(((c >> 18) & 63) * 255 / 63), out[1] = static_cast<uint8_t>(((c >> 12) & 63) * 255 / 63);
        out[2] = static_cast<uint8_t>(((c >> 6) & 63) * 255 / 63), out[3] = static_cast<uint8_t>((c & 63) * 255 / 63);
        break;
    }
    default:  // GX_RGBA8
        out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = p[3];
        break;
    }
}

// Bytes one attribute takes in the vertex stream.
inline uint32_t DirectSize(int attr, const VtxAttrFmt& f)
{
    const auto a = static_cast<GXAttr>(attr);
    return comp_type_size(a, f.type) * comp_cnt_count(a, f.cnt);
}

// How to read the vertices of one draw, worked out from the GX vertex descriptor and format on the game thread.
struct AttrPlan {
    const uint8_t* base;  // indexed: the array
    uint32_t stride;
    uint32_t offset;      // where the value, or its index, starts in the GX vertex
    uint32_t index;       // 0 = direct, 1 = 8-bit index, 2 = 16-bit index (in the stream's byte order)
    bool be;              // the values' byte order
    GXCompType type;
    float scale;          // fixed-point components: 1 / 2^frac
    bool two;             // texture coordinates: s and t (else s only); position: x y z (else x y)
    uint8_t shift;        // fixed-point components: frac (scale = 1 / 2^shift); 0 for floats
};
struct VertexPlan {
    uint32_t size;        // bytes per GX vertex
    int pnMtxOffset;      // -1 = none
    int8_t defaultRow;    // the current position matrix's row, for vertices without PNMTXIDX
    bool streamBe;        // the draw's byte order (display lists big-endian, the runtime's packed draws not)
    bool hasPos, hasClr;
    AttrPlan pos, clr;
    bool hasUv, hasUv2;
    AttrPlan uv, uv2;
    bool hasNrm;
    AttrPlan nrm;
};

// The GX channel COLOR0A0 for this batch: its material color goes into the vertices (from the vertex color or the
// material register); its lighting, when enabled, into the shader's uniforms.
struct ChannelPlan {
    bool rgbFromVertex, alphaFromVertex;
    uint8_t rgb[3], alpha;  // material color (register source)
    bool lit;               // the shader lights the batch: the vertices need normals
};

inline const uint8_t* AttrValues(const AttrPlan& a, const uint8_t* vtx, bool streamBe)
{
    const uint8_t* p = vtx + a.offset;
    if (a.index == 0) return p;
    return a.base + (a.index == 2 ? Load16(p, streamBe) : *p) * a.stride;
}

// The positions of one draw, in a loop specialized for their component type.
template <GXCompType Type, class V>
void DecodePositions(const VertexPlan& plan, const uint8_t* src, uint32_t count, V* out)
{
    const AttrPlan& a = plan.pos;
    constexpr uint32_t step = Type == GX_F32 ? 4 : (Type == GX_U16 || Type == GX_S16) ? 2 : 1;
    const float scale = a.scale;
    const bool be = a.be;
    for (uint32_t n = 0; n < count; ++n, src += plan.size) {
        const uint8_t* values = AttrValues(a, src, plan.streamBe);
        V& v = out[n];
        v.x = LoadComponent(values, Type, scale, be);
        v.y = LoadComponent(values + step, Type, scale, be);
        v.z = a.two ? LoadComponent(values + 2 * step, Type, scale, be) : 0.0f;
    }
}

// A fixed-point component as the integer it is.
inline int32_t LoadInt(const uint8_t* p, GXCompType type, bool be)
{
    switch (type) {
    case GX_U8: return p[0];
    case GX_S8: return static_cast<int8_t>(p[0]);
    case GX_U16: return Load16(p, be);
    default: return static_cast<int16_t>(Load16(p, be));
    }
}
// value * 2^up / 2^shift, toward zero: what the float path's exact product gives when it is cut to an integer
// (a 16-bit value times a power of two, and times 127 for normals, is exact in a float).
inline int32_t ScaleFixed(int32_t value, uint32_t up, uint32_t shift)
{
    return shift <= up ? value * (1 << (up - shift)) : value / (1 << (shift - up));
}

// One texture-coordinate set of a draw into `field` (uv or uv2) of each vertex.
template <class V>
void DecodeUvs(const AttrPlan& a, const uint8_t* src, bool streamBe, uint32_t count, uint32_t size, V* out,
               int16_t (V::*field)[2])
{
    const uint32_t step = a.type == GX_F32 ? 4 : (a.type == GX_U16 || a.type == GX_S16) ? 2 : 1;
    if (a.type != GX_F32) {  // fixed point: integers all the way (kUvScale = 2^9), same results as below
        const GXCompType type = a.type;
        const bool be = a.be, two = a.two;
        const uint32_t shift = a.shift;
        for (uint32_t n = 0; n < count; ++n, src += size) {
            const uint8_t* values = AttrValues(a, src, streamBe);
            const int32_t u = ScaleFixed(LoadInt(values, type, be), 9, shift);
            const int32_t v = two ? ScaleFixed(LoadInt(values + step, type, be), 9, shift) : 0;
            (out[n].*field)[0] = static_cast<int16_t>(std::clamp(u, -32768, 32767));
            (out[n].*field)[1] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }
        return;
    }
    for (uint32_t n = 0; n < count; ++n, src += size) {
        const uint8_t* values = AttrValues(a, src, streamBe);
        const float u = LoadComponent(values, a.type, a.scale, a.be) * kUvScale;
        const float v = a.two ? LoadComponent(values + step, a.type, a.scale, a.be) * kUvScale : 0.0f;
        (out[n].*field)[0] = static_cast<int16_t>(std::clamp(u, -32768.0f, 32767.0f));
        (out[n].*field)[1] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }
}

inline int8_t NormalByte(float v) { return static_cast<int8_t>(std::clamp(v * 127.0f, -127.0f, 127.0f)); }

// The normals of one draw, x 127 (their GX fixed-point scale applied).
template <class V>
void DecodeNormals(const AttrPlan& a, const uint8_t* src, bool streamBe, uint32_t count, uint32_t size, V* out)
{
    const uint32_t step = a.type == GX_F32 ? 4 : (a.type == GX_U16 || a.type == GX_S16) ? 2 : 1;
    if (a.type != GX_F32) {  // fixed point: (value * 127) / 2^shift in integers, same results as below
        const GXCompType type = a.type;
        const bool be = a.be;
        const int32_t divisor = 1 << a.shift;
        for (uint32_t n = 0; n < count; ++n, src += size) {
            const uint8_t* values = AttrValues(a, src, streamBe);
            for (uint32_t k = 0; k < 3; ++k) {
                const int32_t c = LoadInt(values + k * step, type, be) * 127 / divisor;
                out[n].n[k] = static_cast<int8_t>(std::clamp(c, -127, 127));
            }
        }
        return;
    }
    for (uint32_t n = 0; n < count; ++n, src += size) {
        const uint8_t* values = AttrValues(a, src, streamBe);
        out[n].n[0] = NormalByte(LoadComponent(values, a.type, a.scale, a.be));
        out[n].n[1] = NormalByte(LoadComponent(values + step, a.type, a.scale, a.be));
        out[n].n[2] = NormalByte(LoadComponent(values + 2 * step, a.type, a.scale, a.be));
    }
}

// Decodes `count` GX vertices read with `plan` into `out` (V = PicaVertex or PicaVertex2). `normals`: the batch is
// lit or generates coordinates from normals.
template <class V>
void DecodeInto(const VertexPlan& plan, const ChannelPlan& ch, bool normals, const uint8_t* src, uint32_t count, V* out)
{
    const bool be = plan.streamBe;
    // Channel color.
    if ((ch.rgbFromVertex || ch.alphaFromVertex) && plan.hasClr) {
        const uint8_t* vtx = src;
        for (uint32_t n = 0; n < count; ++n, vtx += plan.size) {
            uint8_t c[4];
            LoadColor(AttrValues(plan.clr, vtx, be), plan.clr.type, plan.clr.be, c);
            V& v = out[n];
            v.r = ch.rgbFromVertex ? c[0] : ch.rgb[0];
            v.g = ch.rgbFromVertex ? c[1] : ch.rgb[1];
            v.b = ch.rgbFromVertex ? c[2] : ch.rgb[2];
            v.a = ch.alphaFromVertex ? c[3] : ch.alpha;
        }
    } else {
        // A vertex color source without CLR0 in the vertex: GX reads white.
        const uint8_t r = ch.rgbFromVertex ? 255 : ch.rgb[0], g = ch.rgbFromVertex ? 255 : ch.rgb[1],
                      b = ch.rgbFromVertex ? 255 : ch.rgb[2], a = ch.alphaFromVertex ? 255 : ch.alpha;
        for (uint32_t n = 0; n < count; ++n) out[n].r = r, out[n].g = g, out[n].b = b, out[n].a = a;
    }
    // Normals, for a lit batch or coordinates from the normal (GX's default without NRM in the vertex: (1, 0, 0)).
    if (normals) {
        if (plan.hasNrm) {
            DecodeNormals(plan.nrm, src, be, count, plan.size, out);
        } else {
            for (uint32_t n = 0; n < count; ++n) out[n].n[0] = 127, out[n].n[1] = out[n].n[2] = 0;
        }
    }
    // Matrix row.
    if (plan.pnMtxOffset >= 0) {
        const uint8_t* vtx = src + plan.pnMtxOffset;
        for (uint32_t n = 0; n < count; ++n, vtx += plan.size) out[n].row = static_cast<int8_t>(*vtx);
    } else {
        for (uint32_t n = 0; n < count; ++n) out[n].row = plan.defaultRow;
    }
    // Texture coordinates.
    if (plan.hasUv) {
        DecodeUvs<V>(plan.uv, src, be, count, plan.size, out, &V::uv);
    } else {
        for (uint32_t n = 0; n < count; ++n) out[n].uv[0] = out[n].uv[1] = 0;
    }
    if constexpr (sizeof(V) == sizeof(PicaVertex2)) {
        if (plan.hasUv2) {
            DecodeUvs<V>(plan.uv2, src, be, count, plan.size, out, &V::uv2);
        } else {
            for (uint32_t n = 0; n < count; ++n) out[n].uv2[0] = out[n].uv2[1] = 0;
        }
    }
    // Positions.
    if (plan.hasPos) {
        switch (plan.pos.type) {
        case GX_U8: DecodePositions<GX_U8>(plan, src, count, out); break;
        case GX_S8: DecodePositions<GX_S8>(plan, src, count, out); break;
        case GX_U16: DecodePositions<GX_U16>(plan, src, count, out); break;
        case GX_S16: DecodePositions<GX_S16>(plan, src, count, out); break;
        default: DecodePositions<GX_F32>(plan, src, count, out); break;
        }
    } else {
        for (uint32_t n = 0; n < count; ++n) out[n].x = out[n].y = out[n].z = 0.0f;
    }
}

// ============================================================================================================
// The command ring between the game thread (producer) and the GPU worker (consumer).
// ============================================================================================================

enum CmdType : uint16_t {
    kCmdWrap = 0,  // the rest of the ring is unused: continue at its start
    kCmdBegin, kCmdFlush, kCmdTexEnv, kCmdTexEnvBuf, kCmdAlphaTest, kCmdBlend, kCmdDepth, kCmdCull, kCmdFogMode,
    kCmdFogColor, kCmdFogLut, kCmdFVUnif, kCmdBoolUnif, kCmdIVUnif, kCmdBindTex, kCmdBindRamp, kCmdDraw, kCmdPlan,
    kCmdAppend, kCmdClear, kCmdEndFrame,
    // Step 2: GX commands for Aurora's command processor, which runs on the worker (brawl3ds_cp_hooks.h).
    kCmdCpData, kCmdCpRawDraw, kCmdCpCall, kCmdCpEndFrame,
};
struct CmdHeader {
    uint16_t type;
    uint16_t pad;
    uint32_t size;  // the whole command, header included, a multiple of 8 (GX data can pass 64 KiB)
};
static_assert(sizeof(CmdHeader) == 8);
struct CmdBegin { uint32_t clearColor; };
struct CmdTexEnv { int32_t stage; C3D_TexEnv env; };
struct CmdTexEnvBuf { int32_t rgb, alpha; };
struct CmdAlphaTest { int32_t on, func, ref; };
struct CmdBlend { int32_t kind, src, dst, logicOp; };
struct CmdDepth { int32_t func, mask; };
struct CmdCull { int32_t mode; };
struct CmdFogMode { int32_t on; };
struct CmdFogColor { uint32_t color; };
struct CmdFogLut { C3D_FogLut lut; };
struct CmdFVUnif { int32_t id; C3D_FVec v; };
struct CmdBoolUnif { int32_t id, on; };
struct CmdIVUnif { int32_t id, x, y, z, w; };
struct CmdBindTex { int32_t unit, hasTlut; GXTexObj_ obj; GXTlutObj_ tlut; };
struct CmdBindRamp { int32_t unit; };
struct CmdDraw {
    VertexPlan plan;
    ChannelPlan ch;
    uint8_t normals, layout, skip, pad;
    const uint8_t* vertices;  // Aurora's staging arena: valid until the frame-end drain
    uint32_t vtxCount;
    const uint16_t* indices;
    uint32_t indexCount;
};
struct CmdPlan { VertexPlan plan; };
struct CmdAppend { int32_t prim; uint32_t vtxCount, bytes; };  // followed by the vertices' raw bytes
struct CmdClear { float x0, x1, y0, y1, z; uint8_t rgba[4]; };
struct CmdCpData { uint32_t bytes; uint8_t kind, bigEndian, pad[2]; };  // kind 0 FIFO, 1 display list; then the bytes
// kind 2: a display list left where it is, in guest memory (then its address). The runtime marks the lists it
// passes straight from the game's model data (g_brawl3dsListInGuestRam): geometry, ~670 KiB a match frame that
// was copied into the ring. They stand still until the worker has read them: the game thread waits for the
// worker at the end of every frame, and model data is not rewritten between its draw and that.
struct CmdCpListRef { const uint8_t* data; uint32_t pad; };
static_assert(sizeof(CmdCpListRef) == 8);
struct CmdCpRawDraw { int32_t prim, fmt; uint32_t vtxCount, bytes; };   // then the vertices
struct CmdCpCall { void (*fn)(void*); uint32_t bytes; };                 // then the call's captured values
static_assert(sizeof(CmdCpData) % 8 == 0 && sizeof(CmdCpRawDraw) % 8 == 0 && sizeof(CmdCpCall) % 8 == 0);

// A power of two (positions are taken modulo it with a mask: the ARM11 has no divide instruction). 2 MiB if the heap
// has it (a match frame records ~1 MiB, most of it appended vertices), else 1 MiB or 512 KiB: the game thread then
// waits for ring space more often instead of the run aborting.
uint32_t g_ringBytes = 2u << 20;
uint8_t* g_ring = nullptr;
uint32_t g_head = 0;  // bytes recorded so far (the producer's; the worker reads it)
uint32_t g_tail = 0;  // bytes executed so far (the worker's; the producer reads it)
bool g_workerOn = false;  // a worker thread runs the commands (else the game thread does, as it records them)
Thread g_worker = nullptr;
LightEvent g_workAvailable, g_spaceAvailable, g_drained;
int g_workerSleeping = 0, g_producerWaitingSpace = 0, g_producerWaitingDrain = 0;
// Statistics (WriteStats).
uint64_t g_workerBusyTicks = 0, g_drainWaitTicks = 0, g_spaceWaitTicks = 0;
uint64_t g_ringBytesRecorded = 0;
uint32_t g_ringPeak = 0;
uint32_t g_wakeSignals = 0;  // times the game thread had to wake a sleeping worker (a syscall each)
// Step 2: the worker's time in Aurora's command processor (with the recording it does now), the game thread's
// catch-up waits (inline scopes) and the GX bytes handed over.
bool g_cpOnWorker = false;  // Aurora's command processor runs on the worker (else on the game thread, as in step 1)
bool g_cpWanted = false;    // ... from the end of the first frame the renderer saw (StartRenderer, Brawl3dsPicaEndFrame)
int g_cpInlineDepth = 0;    // game thread: > 0 inside an inline scope (it decodes GX commands itself)
uint64_t g_cpTicks = 0, g_winCpTicks = 0, g_cpBytes = 0;
uint32_t g_cpSyncs = 0, g_winCpSyncs = 0, g_cpFailures = 0;
uint64_t g_cpSyncTicks = 0, g_winCpSyncTicks = 0;  // the game thread's time in those waits
// The GX functions that made the game thread wait (inline scopes), for the match summary.
struct SyncSite {
    const char* name;
    uint32_t count;
    uint64_t ticks;
};
SyncSite g_syncSites[8];
const char* g_syncSite = nullptr;  // the site of the wait in progress
// The same, for the periodic frame lines of headless.log (Brawl3dsPicaTakeWindowStats): a run that stops early
// still says how the worker did.
uint64_t g_winBusyTicks = 0, g_winDrainTicks = 0;
uint32_t g_winWakeSignals = 0;

void ExecuteRecorded();  // the worker's command loop body (below)

// Room for one command of `size` bytes at the ring's head; waits while the worker has not caught up.
uint8_t* Reserve(uint32_t size)
{
    for (;;) {
        const uint32_t at = g_head & (g_ringBytes - 1);
        const uint32_t contiguous = g_ringBytes - at;
        const uint32_t need = size <= contiguous ? size : contiguous + size;  // wrapping skips the rest of the ring
        const uint32_t tail = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
        const uint32_t used = g_head - tail;
        if (used + need <= g_ringBytes) {
            if (size > contiguous) {
                auto* wrap = reinterpret_cast<CmdHeader*>(g_ring + at);
                wrap->type = kCmdWrap;
                wrap->pad = 0;
                wrap->size = 0;
                g_head += contiguous;  // published with the command after it
            }
            g_ringPeak = std::max(g_ringPeak, used + need);
            return g_ring + (g_head & (g_ringBytes - 1));
        }
        if (!g_workerOn) {  // cannot happen inline (everything runs as it is recorded)
            std::fprintf(stderr, "[pica] command ring full without a worker\n");
            std::abort();
        }
        const uint64_t start = svcGetSystemTick();
        __atomic_store_n(&g_producerWaitingSpace, 1, __ATOMIC_SEQ_CST);
        if (g_head - __atomic_load_n(&g_tail, __ATOMIC_SEQ_CST) + need > g_ringBytes)
            LightEvent_Wait(&g_spaceAvailable);
        __atomic_store_n(&g_producerWaitingSpace, 0, __ATOMIC_SEQ_CST);
        g_spaceWaitTicks += svcGetSystemTick() - start;
    }
}

// Publishes the command just written at the head; inline, runs it.
void Commit(uint32_t size)
{
    g_ringBytesRecorded += size;
    __atomic_store_n(&g_head, g_head + size, __ATOMIC_SEQ_CST);
    if (!g_workerOn) {
        ExecuteRecorded();
        return;
    }
    if (__atomic_load_n(&g_workerSleeping, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&g_workerSleeping, 0, __ATOMIC_SEQ_CST);
        LightEvent_Signal(&g_workAvailable);
        ++g_wakeSignals;
        ++g_winWakeSignals;
    }
}

constexpr uint32_t CmdSize(uint32_t payload) { return (sizeof(CmdHeader) + payload + 7u) & ~7u; }

void Execute(const CmdHeader* h);  // runs one command (the worker side, below)

// The worker records too since step 2 (Aurora's command processor runs there and calls the recording side): its
// commands run at once, in order, from this scratch buffer instead of going through the ring. An appended draw is
// the largest command (Brawl3dsPicaAppend keeps it under 64 KiB).
alignas(8) uint8_t g_inlineCommand[0x10000];
bool OnWorker() { return g_worker != nullptr && threadGetCurrent() == g_worker; }
uint8_t* ReserveCommand(uint32_t size)
{
    if (!OnWorker()) return Reserve(size);
    if (size > sizeof(g_inlineCommand)) {
        std::fprintf(stderr, "[pica] a %u-byte command recorded on the worker\n", size);
        std::abort();
    }
    return g_inlineCommand;
}
void CommitCommand(CmdHeader* h)
{
    if (OnWorker())
        Execute(h);
    else
        Commit(h->size);
}

template <class T>
T* Record(CmdType type, uint32_t extra = 0)
{
    const uint32_t size = CmdSize(sizeof(T) + extra);
    auto* h = reinterpret_cast<CmdHeader*>(ReserveCommand(size));
    h->type = type;
    h->pad = 0;
    h->size = size;
    return reinterpret_cast<T*>(h + 1);
}
template <class T>
void Publish(T* payload)
{
    CommitCommand(reinterpret_cast<CmdHeader*>(payload) - 1);
}
void RecordBare(CmdType type)
{
    auto* h = reinterpret_cast<CmdHeader*>(ReserveCommand(CmdSize(0)));
    h->type = type;
    h->pad = 0;
    h->size = CmdSize(0);
    CommitCommand(h);
}

// Waits until the worker has run everything recorded (the frame's end).
void Drain()
{
    if (!g_workerOn || OnWorker()) return;
    const uint64_t start = svcGetSystemTick();
    while (__atomic_load_n(&g_tail, __ATOMIC_SEQ_CST) != g_head) {
        __atomic_store_n(&g_producerWaitingDrain, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_tail, __ATOMIC_SEQ_CST) != g_head) LightEvent_Wait(&g_drained);
        __atomic_store_n(&g_producerWaitingDrain, 0, __ATOMIC_SEQ_CST);
    }
    const uint64_t waited = svcGetSystemTick() - start;
    g_drainWaitTicks += waited;
    g_winDrainTicks += waited;
}

// ============================================================================================================
// The worker side: everything that touches citro3d, the GPU buffers and the texture cache.
// ============================================================================================================

bool g_ready = false;
bool g_failed = false;
bool g_frameOpen = false;
C3D_RenderTarget* g_target = nullptr;
uint8_t* g_vtxBytes = nullptr;  // the frame's vertices, each batch in its layout (PicaVertex or PicaVertex2)
uint16_t* g_indices = nullptr;
uint32_t g_vtxBytesUsed = 0, g_idxUsed = 0;
// The open batch: where its vertices start in the vertex buffer, its first index, its vertex count and layout
// (0: PicaVertex, 1: PicaVertex2), how its vertices are read, and whether it is drawn at all (GX_CULL_ALL).
uint32_t g_batchVtxByte = 0, g_batchIdx = 0, g_batchVerts = 0;
int g_batchLayout = 0;
bool g_batchSkip = false;
VertexPlan g_batchPlan;
ChannelPlan g_batchChannel;
bool g_batchNormals = false;
int g_curLayout = -1;  // the attribute layout citro3d has
bool g_batchOpen = false;
uint32_t g_droppedDraws = 0;
uint32_t g_statClears = 0;
bool g_skipFrame = false;       // OpenFrame gave up on a busy GPU: no drawing until the frame ends
uint32_t g_gpuStalls = 0;
const uint8_t* g_lastTopFramebuffer = nullptr;  // for the screenshots
C3D_FogLut g_workerFogLut;
// citro3d's command buffer: a match frame has ~555 batches, each uploading ~46 uniform vectors, its combiners,
// textures and fragment state, plus its draw (~1 KiB a batch).
constexpr size_t kCommandBufferSize = 768 * 1024;

bool InitGpu()
{
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeded
    auto fail = [](const char* what) {
        std::fprintf(stderr, "[pica] renderer NOT started: %s failed (%u KiB linear, %u KiB VRAM free)\n", what,
                     static_cast<unsigned>(linearSpaceFree() >> 10), static_cast<unsigned>(vramSpaceFree() >> 10));
        return false;
    };
    if (!g_dvlb) return fail("the shader");
    // The top screen still has the start-up text console's format (libctru's consoleInit: RGB565, single buffer),
    // but the render target's display transfer writes 24-bit pixels: the screen showed the frame as two to three
    // smeared copies while the (24-bit) screenshots were right. 24-bit again, before the buffers below take the
    // linear memory it needs; single-buffered, as double buffering needs ~280 KiB more than is left.
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    if (!C3D_Init(kCommandBufferSize)) return fail("C3D_Init");
    g_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!g_target) return fail("the render target");
    C3D_RenderTargetSetOutput(g_target, GFX_TOP, GFX_LEFT, kDisplayFlags);
    g_vtxBytes = static_cast<uint8_t*>(linearAlloc(kVertexBytes));
    if (!g_vtxBytes) return fail("the vertex buffer");
    g_indices = static_cast<uint16_t*>(linearAlloc(sizeof(uint16_t) * kMaxIndices));
    if (!g_indices) return fail("the index buffer");
    std::fprintf(stderr,
                 "[pica] renderer ready: %u vertices, %u indices per frame (%u KiB linear); %u KiB linear, "
                 "%u KiB VRAM free\n",
                 kMaxVertices, kMaxIndices,
                 static_cast<unsigned>((kVertexBytes + 2 * kMaxIndices) >> 10),
                 static_cast<unsigned>(linearSpaceFree() >> 10), static_cast<unsigned>(vramSpaceFree() >> 10));
    g_failed = false;
    g_ready = true;
    return true;
}

// The vertex attribute layout: 0 = PicaVertex (four inputs), 1 = PicaVertex2 (set B as a fifth). No fixed attribute:
// the console's GPU hung on the first draws (2 s every frame) in a build that gave layout 0 set B as a fixed 0 and
// whose shader wrote texcoord2 twice (one of the two; per-vertex fog was cleared on the console later). The shader
// reads set B only under its twoset flag. citro3d re-sends the attribute state whenever it is fetched for writing,
// so it changes only when the layout does.
void SetLayout(int layout)
{
    if (layout == g_curLayout) return;
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);          // position
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);  // channel color
    AttrInfo_AddLoader(attr, 2, GPU_BYTE, 4);           // matrix row, normal x 127
    AttrInfo_AddLoader(attr, 3, GPU_SHORT, 2);          // texture coordinates set A x 512 (the shader sees u, v, 0, 1)
    if (layout) AttrInfo_AddLoader(attr, 4, GPU_SHORT, 2);  // set B
    g_curLayout = layout;
}

// The state that does not change between batches; set at the start of every frame.
void ApplyFixedState()
{
    C3D_BindProgram(&g_program);
    g_curLayout = -1;
    SetLayout(0);
    // GX clip-space z runs from -w (near) to 0 (far), which is also the PICA's clip range. citro3d's default depth
    // map (depth = -z/w) makes near 1 and far 0; GX's depth buffer has near 0 and far 1, so the game's compare
    // functions and clear value apply unchanged with depth = z/w + 1.
    C3D_DepthMap(true, 1.0f, 1.0f);
}

void OpenFrame(uint32_t clearColor)
{
    if (g_frameOpen || g_skipFrame || !InitGpu()) return;
    // The previous frame's GPU work must be done: this frame reuses its buffers. Not C3D_FRAME_SYNCDRAW, which also
    // waits for citro3d's vblank-paced frame slot (the game paces itself): with it, Azahar runs stalled for good on
    // the safety screen. Poll instead, say so when the GPU stays busy, and drop the frame's drawing after 2 s.
    const uint64_t start = svcGetSystemTick();
    bool logged = false;
    g_brawl3dsRenderPhase = "waiting for the GPU (frame start)";
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        const uint64_t waited = svcGetSystemTick() - start;
        if (!logged && waited > SYSCLOCK_ARM11) {
            std::fprintf(stderr, "[pica] GPU still busy with the previous frame after 1 s\n");
            logged = true;
        }
        if (waited > 2ull * SYSCLOCK_ARM11) {
            std::fprintf(stderr, "[pica] GPU busy for 2 s: this frame is not drawn\n");
            ++g_gpuStalls;
            g_skipFrame = true;
            return;
        }
        svcSleepThread(500000);  // 0.5 ms
    }
    C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, clearColor, 0x00FFFFFF);
    C3D_FrameDrawOn(g_target);
    ApplyFixedState();
    TexturesBeginFrame();
    g_vtxBytesUsed = g_idxUsed = 0;
    g_batchOpen = false;
    g_frameOpen = true;
    g_brawl3dsRenderPhase = "drawing";
}

void FlushBatch()
{
    if (!g_batchOpen) return;
    g_batchOpen = false;
    const uint32_t count = g_idxUsed - g_batchIdx;
    if (count == 0 || g_batchSkip) return;
    SetLayout(g_batchLayout);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    if (g_batchLayout)
        BufInfo_Add(buf, g_vtxBytes + g_batchVtxByte, sizeof(PicaVertex2), 5, 0x43210);
    else
        BufInfo_Add(buf, g_vtxBytes + g_batchVtxByte, sizeof(PicaVertex), 4, 0x3210);
    C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, g_indices + g_batchIdx);
}

// Decodes `count` vertices into the open batch; false when the vertex buffer is full.
bool DecodeIntoBatch(const uint8_t* src, uint32_t count)
{
    const uint32_t stride = g_batchLayout ? sizeof(PicaVertex2) : sizeof(PicaVertex);
    if (g_vtxBytesUsed + count * stride > kVertexBytes) return false;
    if (g_batchLayout)
        DecodeInto(g_batchPlan, g_batchChannel, g_batchNormals, src, count,
                   reinterpret_cast<PicaVertex2*>(g_vtxBytes + g_vtxBytesUsed));
    else
        DecodeInto(g_batchPlan, g_batchChannel, g_batchNormals, src, count,
                   reinterpret_cast<PicaVertex*>(g_vtxBytes + g_vtxBytesUsed));
    g_vtxBytesUsed += count * stride;
    g_batchVerts += count;
    return true;
}

// Triangle-list indices for `count` vertices of `prim`, relative to `first` (in the batch).
bool AppendIndices(GXPrimitive prim, uint32_t first, uint32_t count)
{
    const uint32_t needed = count * 3;
    if (g_idxUsed + needed > kMaxIndices) return false;
    uint16_t* out = g_indices + g_idxUsed;
    uint32_t n = 0;
    switch (prim) {
    case GX_QUADS:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            const uint32_t q = first + i;
            out[n++] = q, out[n++] = q + 1, out[n++] = q + 2;
            out[n++] = q, out[n++] = q + 2, out[n++] = q + 3;
        }
        break;
    case GX_TRIANGLES:
        for (uint32_t i = 0; i + 2 < count; i += 3) out[n++] = first + i, out[n++] = first + i + 1, out[n++] = first + i + 2;
        break;
    case GX_TRIANGLESTRIP:
        for (uint32_t i = 0; i + 2 < count; ++i) {
            out[n++] = first + i;
            out[n++] = first + ((i & 1) ? i + 2 : i + 1);
            out[n++] = first + ((i & 1) ? i + 1 : i + 2);
        }
        break;
    case GX_TRIANGLEFAN:
        for (uint32_t i = 1; i + 1 < count; ++i) out[n++] = first, out[n++] = first + i, out[n++] = first + i + 1;
        break;
    default:  // lines and points: not drawn yet
        break;
    }
    g_idxUsed += n;
    return true;
}

void ExecDraw(const CmdDraw& d)
{
    if (!g_frameOpen) return;
    FlushBatch();
    if (g_vtxBytesUsed + 8 + d.vtxCount * sizeof(PicaVertex2) > kVertexBytes || g_idxUsed + d.indexCount > kMaxIndices) {
        ++g_droppedDraws;
        return;
    }
    g_batchPlan = d.plan;
    g_batchChannel = d.ch;
    g_batchNormals = d.normals != 0;
    g_batchLayout = d.layout;
    g_batchSkip = d.skip != 0;
    // Every batch starts 8-byte aligned, as it always did with 24-byte vertices (28-byte ones break that).
    g_vtxBytesUsed = (g_vtxBytesUsed + 7) & ~7u;
    g_batchVtxByte = g_vtxBytesUsed;
    g_batchIdx = g_idxUsed;
    g_batchVerts = 0;
    if (!DecodeIntoBatch(d.vertices, d.vtxCount)) {
        ++g_droppedDraws;
        return;
    }
    std::memcpy(g_indices + g_idxUsed, d.indices, d.indexCount * sizeof(uint16_t));
    g_idxUsed += d.indexCount;
    g_batchOpen = true;
}

void ExecAppend(const CmdAppend& a)
{
    if (!g_frameOpen || !g_batchOpen) return;
    const uint32_t first = g_batchVerts;
    const uint32_t stride = g_batchLayout ? sizeof(PicaVertex2) : sizeof(PicaVertex);
    if (g_vtxBytesUsed + a.vtxCount * stride > kVertexBytes || g_idxUsed + a.vtxCount * 3 > kMaxIndices) {
        ++g_droppedDraws;
        return;
    }
    if (!DecodeIntoBatch(reinterpret_cast<const uint8_t*>(&a + 1), a.vtxCount)) return;
    AppendIndices(static_cast<GXPrimitive>(a.prim), first, a.vtxCount);
}

void ExecClear(const CmdClear& c)
{
    if (!g_frameOpen) return;
    FlushBatch();
    g_vtxBytesUsed = (g_vtxBytesUsed + 7) & ~7u;
    if (g_vtxBytesUsed + 4 * sizeof(PicaVertex) > kVertexBytes || g_idxUsed + 6 > kMaxIndices) {
        ++g_droppedDraws;
        return;
    }
    ++g_statClears;
    PicaVertex* v = reinterpret_cast<PicaVertex*>(g_vtxBytes + g_vtxBytesUsed);
    const float corners[4][2] = {{c.x0, c.y0}, {c.x1, c.y0}, {c.x1, c.y1}, {c.x0, c.y1}};
    for (int i = 0; i < 4; ++i) {
        v[i] = {};
        v[i].x = corners[i][0], v[i].y = corners[i][1], v[i].z = c.z;
        v[i].r = c.rgba[0], v[i].g = c.rgba[1], v[i].b = c.rgba[2], v[i].a = c.rgba[3];
    }
    uint16_t* idx = g_indices + g_idxUsed;
    idx[0] = 0, idx[1] = 1, idx[2] = 2, idx[3] = 0, idx[4] = 2, idx[5] = 3;
    SetLayout(0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, v, sizeof(PicaVertex), 4, 0x3210);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, idx);
    g_vtxBytesUsed += 4 * sizeof(PicaVertex);
    g_idxUsed += 6;
}

// In-game captures: Brawl3dsPicaCaptureNextFrame keeps the copy of the frame that just ended, taken at the next
// frame's end (no waiting), and Brawl3dsPicaWriteCaptures writes them at the end of the run. Writing one at once
// stalled the game ~3 s (a GPU wait and a slow SD write), and in two console runs one such stall was followed by a
// four-minute one in VIWaitForRetrace.
constexpr uint32_t kShotW = 240, kShotH = 400, kShotBytes = kShotW * kShotH * 3;
struct Capture {
    char path[64];
    uint8_t* rgb;
};
Capture g_captures[8];
uint32_t g_captureCount = 0;
const uint8_t* g_pendingCaptureSource = nullptr;
char g_pendingCapturePath[64];

void TakePendingCapture()
{
    if (!g_pendingCaptureSource) return;
    const uint8_t* src = g_pendingCaptureSource;
    g_pendingCaptureSource = nullptr;
    if (g_captureCount >= sizeof(g_captures) / sizeof(g_captures[0])) return;
    auto* rgb = static_cast<uint8_t*>(std::malloc(kShotBytes));
    if (!rgb) return;  // no memory left for it: the capture is skipped, the run goes on
    GSPGPU_InvalidateDataCache(src, kShotBytes);
    std::memcpy(rgb, src, kShotBytes);
    Capture& c = g_captures[g_captureCount++];
    std::snprintf(c.path, sizeof(c.path), "%s", g_pendingCapturePath);
    c.rgb = rgb;
}

void ExecEndFrame()
{
    g_skipFrame = false;
    if (!g_frameOpen) return;
    FlushBatch();
    GSPGPU_FlushDataCache(g_vtxBytes, g_vtxBytesUsed);
    GSPGPU_FlushDataCache(g_indices, g_idxUsed * sizeof(uint16_t));
    // A capture asked for after the previous frame: that frame's copy is complete now (C3D_FrameBegin waited).
    TakePendingCapture();
    // The framebuffer citro3d copies this frame into (its back buffer until the copy is done and it swaps).
    g_lastTopFramebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    g_brawl3dsRenderPhase = "C3D_FrameEnd";
    C3D_FrameEnd(0);
    g_brawl3dsRenderPhase = "idle (game code)";
    g_frameOpen = false;
}

void ExecBlend(const CmdBlend& b)
{
    switch (b.kind) {
    case 1:
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, static_cast<GPU_BLENDFACTOR>(b.src),
                       static_cast<GPU_BLENDFACTOR>(b.dst), static_cast<GPU_BLENDFACTOR>(b.src),
                       static_cast<GPU_BLENDFACTOR>(b.dst));
        break;
    case 2:
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        break;
    case 3: C3D_ColorLogicOp(static_cast<GPU_LOGICOP>(b.logicOp)); break;
    default: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); break;
    }
}

void ExecCp(const CmdHeader* h);  // step 2: Aurora's command processor on the worker (below)

// Runs one recorded command.
void Execute(const CmdHeader* h)
{
    // GX commands for the command processor: always (GXInit's come before the GPU is up).
    if (h->type >= kCmdCpData) {
        ExecCp(h);
        return;
    }
    const void* p = h + 1;
    // Nothing touches citro3d before it is up (InitGpu runs at the first frame's start). State still applies to a
    // frame the GPU did not let start (the game thread's record of it stays right); textures and draws do not.
    if (!g_ready && h->type != kCmdBegin) return;
    if (!g_frameOpen && (h->type == kCmdBindTex || h->type == kCmdBindRamp)) return;
    switch (h->type) {
    case kCmdBegin: OpenFrame(static_cast<const CmdBegin*>(p)->clearColor); break;
    case kCmdFlush: FlushBatch(); break;
    case kCmdTexEnv: {
        const auto* c = static_cast<const CmdTexEnv*>(p);
        C3D_TexEnv env = c->env;
        C3D_SetTexEnv(c->stage, &env);
        break;
    }
    case kCmdTexEnvBuf: {
        const auto* c = static_cast<const CmdTexEnvBuf*>(p);
        C3D_TexEnvBufUpdate(C3D_RGB, c->rgb);
        C3D_TexEnvBufUpdate(C3D_Alpha, c->alpha);
        break;
    }
    case kCmdAlphaTest: {
        const auto* c = static_cast<const CmdAlphaTest*>(p);
        C3D_AlphaTest(c->on != 0, static_cast<GPU_TESTFUNC>(c->func), c->ref);
        break;
    }
    case kCmdBlend: ExecBlend(*static_cast<const CmdBlend*>(p)); break;
    case kCmdDepth: {
        const auto* c = static_cast<const CmdDepth*>(p);
        C3D_DepthTest(true, static_cast<GPU_TESTFUNC>(c->func), static_cast<GPU_WRITEMASK>(c->mask));
        break;
    }
    case kCmdCull: C3D_CullFace(static_cast<GPU_CULLMODE>(static_cast<const CmdCull*>(p)->mode)); break;
    case kCmdFogMode:
        C3D_FogGasMode(static_cast<const CmdFogMode*>(p)->on ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        break;
    case kCmdFogColor: C3D_FogColor(static_cast<const CmdFogColor*>(p)->color); break;
    case kCmdFogLut:
        g_workerFogLut = static_cast<const CmdFogLut*>(p)->lut;
        C3D_FogLutBind(&g_workerFogLut);
        break;
    case kCmdFVUnif: {
        const auto* c = static_cast<const CmdFVUnif*>(p);
        C3D_FVUnif[GPU_VERTEX_SHADER][c->id] = c->v;
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][c->id] = true;
        break;
    }
    case kCmdBoolUnif: {
        const auto* c = static_cast<const CmdBoolUnif*>(p);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, c->id, c->on != 0);
        break;
    }
    case kCmdIVUnif: {
        const auto* c = static_cast<const CmdIVUnif*>(p);
        C3D_IVUnifSet(GPU_VERTEX_SHADER, c->id, c->x, c->y, c->z, c->w);
        break;
    }
    case kCmdBindTex: {
        const auto* c = static_cast<const CmdBindTex*>(p);
        BindTextureObj(c->unit, c->obj, c->hasTlut ? &c->tlut : nullptr);
        break;
    }
    case kCmdBindRamp: BindFogRamp(static_cast<const CmdBindRamp*>(p)->unit); break;
    case kCmdDraw: ExecDraw(*static_cast<const CmdDraw*>(p)); break;
    case kCmdPlan: g_batchPlan = static_cast<const CmdPlan*>(p)->plan; break;
    case kCmdAppend: ExecAppend(*static_cast<const CmdAppend*>(p)); break;
    case kCmdClear: ExecClear(*static_cast<const CmdClear*>(p)); break;
    case kCmdEndFrame: ExecEndFrame(); break;
    case kCmdCpData: case kCmdCpRawDraw: case kCmdCpCall: case kCmdCpEndFrame: ExecCp(h); break;
    default: break;
    }
}

// Runs every command recorded and not yet run.
void ExecuteRecorded()
{
    uint32_t tail = g_tail;
    const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        const auto* h = reinterpret_cast<const CmdHeader*>(g_ring + (tail & (g_ringBytes - 1)));
        if (h->type == kCmdWrap) {
            tail += g_ringBytes - (tail & (g_ringBytes - 1));
            continue;
        }
        Execute(h);
        tail += h->size;
        // Free the room as it goes: a frame records more than the producer would otherwise wait for.
        if ((tail & 0xFFFF) < h->size) __atomic_store_n(&g_tail, tail, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_tail, tail, __ATOMIC_SEQ_CST);
}

// The GPU worker on core 2: runs commands as they come. The core is its own (any thread put there later needs a
// higher priority to preempt it), so while a frame is being recorded it stays awake up to kAwakeTicks between commands:
// every sleep costs the game thread a wake-up syscall at its next command, and a frame records thousands of
// commands in bursts. Between frames (game logic) it sleeps after a short spin.
constexpr uint64_t kAwakeTicks = SYSCLOCK_ARM11 / 125;  // 8 ms
void WorkerMain(void*)
{
    for (;;) {
        uint32_t spins = 0;
        uint64_t idleSince = 0;
        while (__atomic_load_n(&g_head, __ATOMIC_ACQUIRE) == g_tail) {
            if (++spins < 4000) continue;
            if (g_frameOpen) {
                const uint64_t now = svcGetSystemTick();
                if (!idleSince) idleSince = now;
                if (now - idleSince < kAwakeTicks) {
                    spins = 0;
                    continue;
                }
            }
            idleSince = 0;
            __atomic_store_n(&g_workerSleeping, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_head, __ATOMIC_SEQ_CST) == g_tail) LightEvent_Wait(&g_workAvailable);
            __atomic_store_n(&g_workerSleeping, 0, __ATOMIC_SEQ_CST);
            spins = 0;
        }
        const uint64_t start = svcGetSystemTick();
        ExecuteRecorded();
        const uint64_t busy = svcGetSystemTick() - start;
        g_workerBusyTicks += busy;
        g_winBusyTicks += busy;
        if (__atomic_load_n(&g_producerWaitingSpace, __ATOMIC_SEQ_CST)) LightEvent_Signal(&g_spaceAvailable);
        if (__atomic_load_n(&g_producerWaitingDrain, __ATOMIC_SEQ_CST) &&
            __atomic_load_n(&g_head, __ATOMIC_SEQ_CST) == g_tail)
            LightEvent_Signal(&g_drained);
    }
}

// ============================================================================================================
// The game-thread side: reads the GX state and records.
// ============================================================================================================

bool g_started = false;
bool g_recFrameOpen = false;
bool g_recBatchOpen = false;
uint32_t g_recBatchVerts = 0;
VertexPlan g_recPlan;
// g_recPlan is still what BuildPlan would give for (g_recPlanFmt, g_recPlanBe): nothing has written the vertex
// state since (g_brawl3dsVtxGen counts the CP register writes that changed one and the GX calls run on the
// worker). A match frame appends ~11,000 draws to its ~550 batches; building and comparing a plan for each was
// a third of the worker's time.
unsigned g_recPlanGen = 0;
int g_recPlanFmt = -1;
bool g_recPlanBe = false, g_recPlanMemo = false;
uint32_t g_recDropped = 0;
MaterialPlan g_plan;
ChannelPlan g_channel;
uint32_t g_statClearsSkipped = 0, g_statPartViewport = 0, g_statPostMtx = 0, g_statPostNormalize = 0;
bool g_debugFrame = false;  // Brawl3dsPicaDebugNextFrame: log the next frame's first batches
uint32_t g_debugBatches = 0;
uint32_t g_debugSerial = 0;  // batches of the debug frame so far
// What the worker's citro3d uniforms hold (as recorded): only changes are recorded. citro3d uploads every uniform
// vector marked dirty before the next draw; ~555 batches a frame rewriting all 46 overflowed its command buffer.
C3D_FVec g_fvShadow[96];
uint16_t g_boolShadow = 0;
C3D_IVec g_ivShadow[4];

// The shader is parsed here (pure CPU work): the uniform locations are needed to record. The worker binds it.
// It starts at the first draw, as before step 2: started by GXInit's commands, its 2 MiB ring took the memory the
// runtime's disc index needs a moment later (console, 2026-11-24: malloc(48) failed, "DVD file table is
// unavailable"). The switches that choose the threads are read here (main_3ds.cpp reads them a little later).
void StartRenderer()
{
    if (g_started) return;
    g_started = true;
    if (g_debugFlags == 0) {
        if (std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/debug_flags.txt", "r")) {
            unsigned flags = 0;
            if (std::fscanf(f, "%u", &flags) == 1) g_debugFlags = flags;
            std::fclose(f);
        }
    }
    g_dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(brawl3ds_gx_m4_shbin)),
                            brawl3ds_gx_m4_shbin_size);
    if (g_dvlb) {
        shaderProgramInit(&g_program);
        shaderProgramSetVsh(&g_program, &g_dvlb->DVLE[0]);
        g_projLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "proj");
        g_mtxLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "posmtx");
        g_texMtxLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "texmtx");
        g_texSrcLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "texsrc");
        g_ambLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "amb");
        g_lselLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "lsel");
        g_lightsLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "lights");
        g_lightLoopLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "lightloop");
        g_litLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "lit");
        g_fogpLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "fogp");
        g_fogvLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "fogv");
        g_twosetLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "twoset");
        if (g_projLoc < 0 || g_mtxLoc < 0 || g_texMtxLoc < 0 || g_texSrcLoc < 0 || g_ambLoc < 0 || g_lselLoc < 0 ||
            g_lightsLoc < 0 || g_lightLoopLoc < 0 || g_litLoc < 0 || g_fogpLoc < 0 || g_fogvLoc < 0 ||
            g_twosetLoc < 0) {
            std::fprintf(stderr, "[pica] the shader's uniforms are missing\n");
            g_dvlb = nullptr;  // InitGpu reports it
        }
    }
    for (g_ringBytes = 2u << 20; g_ringBytes >= (512u << 10); g_ringBytes >>= 1) {
        g_ring = static_cast<uint8_t*>(std::malloc(g_ringBytes));
        if (g_ring) break;
    }
    if (!g_ring) {
        std::fprintf(stderr, "[pica] no memory for the command ring (not even 512 KiB)\n");
        std::abort();
    }
    if (g_ringBytes != (2u << 20))
        std::fprintf(stderr, "[pica] command ring %u KiB (no 2 MiB block in the heap)\n", g_ringBytes >> 10);
    LightEvent_Init(&g_workAvailable, RESET_ONESHOT);
    LightEvent_Init(&g_spaceAvailable, RESET_ONESHOT);
    LightEvent_Init(&g_drained, RESET_ONESHOT);
    // The worker on core 2 (the New 3DS's second application core; melee-3ds and smash64-3ds run their renderers
    // there with the same exheader), at the game thread's priority.
    if (!(g_debugFlags & 32)) {
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        g_worker = threadCreate(WorkerMain, nullptr, 64 * 1024, priority, 2, true);
    }
    g_workerOn = g_worker != nullptr;
    // The command processor moves to the worker at the end of this frame (Brawl3dsPicaEndFrame), with nothing in
    // flight: this first draw is already being decoded here.
    g_cpWanted = g_workerOn && !(g_debugFlags & 64);
    std::fprintf(stderr, "[pica] GPU work %s; GX command processor on %s\n",
                 g_workerOn ? "on a worker thread on core 2" : "on the game thread (no core 2 worker)",
                 g_cpWanted ? "the worker from the end of this frame" : "the game thread");
}

// ============================================================================================================
// Step 2: Aurora's command processor on the worker (brawl3ds_cp_hooks.h). The game thread hands over GX bytes;
// the worker decodes them, which records (at once, on the worker) and draws.
// ============================================================================================================

void Sync();
void CpEndFrame();  // the frame's end on the worker (below, with the recorder)

// Queues GX bytes (copied). Too large for the ring: decoded here once the worker has caught up.
bool DeferBytes(uint8_t kind, bool bigEndian, const uint8_t* data, uint32_t size)
{
    const uint32_t cmd = CmdSize(sizeof(CmdCpData) + size);
    if (cmd > g_ringBytes / 2) {
        Sync();
        ++g_cpInlineDepth;
        if (kind == 0) {
            uint32_t consumed = 0;
            while (consumed < size) {
                const uint32_t n = aurora::gx::fifo::process(data + consumed, size - consumed, bigEndian);
                if (n == 0) break;
                consumed += n;
            }
        } else {
            brawl3ds_cp_process_display_list(data, size, bigEndian);
        }
        --g_cpInlineDepth;
        return true;
    }
    auto* h = reinterpret_cast<CmdHeader*>(Reserve(cmd));
    h->type = kCmdCpData;
    h->pad = 0;
    h->size = cmd;
    auto* c = reinterpret_cast<CmdCpData*>(h + 1);
    c->bytes = size;
    c->kind = kind;
    c->bigEndian = bigEndian ? 1 : 0;
    c->pad[0] = c->pad[1] = 0;
    std::memcpy(c + 1, data, size);
    g_cpBytes += size;
    Commit(cmd);
    return true;
}

// The game thread waits until the worker has decoded (and run) everything queued.
void Sync()
{
    if (!g_cpOnWorker || OnWorker()) return;
    const uint64_t start = svcGetSystemTick();
    if (g_cpInlineDepth == 0) aurora::gx::fifo::drain();  // what is still in the FIFO goes first
    Drain();
    const uint64_t ticks = svcGetSystemTick() - start;
    ++g_cpSyncs;
    ++g_winCpSyncs;
    g_cpSyncTicks += ticks;
    g_winCpSyncTicks += ticks;
    const char* name = g_syncSite ? g_syncSite : "(large GX data)";
    for (SyncSite& site : g_syncSites) {
        if (site.name == nullptr || site.name == name || std::strcmp(site.name, name) == 0) {
            site.name = name;
            ++site.count;
            site.ticks += ticks;
            break;
        }
    }
}

void ProcessCpCommand(const CmdHeader* h)
{
    const void* p = h + 1;
    switch (h->type) {
    case kCmdCpData: {
        const auto* c = static_cast<const CmdCpData*>(p);
        const auto* bytes = reinterpret_cast<const uint8_t*>(c + 1);
        if (c->kind == 0) {
            uint32_t consumed = 0;
            while (consumed < c->bytes) {
                const uint32_t n = aurora::gx::fifo::process(bytes + consumed, c->bytes - consumed, c->bigEndian != 0);
                if (n == 0) break;
                consumed += n;
            }
        } else if (c->kind == 2) {
            brawl3ds_cp_process_display_list(reinterpret_cast<const CmdCpListRef*>(c + 1)->data, c->bytes,
                                             c->bigEndian != 0);
        } else {
            brawl3ds_cp_process_display_list(bytes, c->bytes, c->bigEndian != 0);
        }
        break;
    }
    case kCmdCpRawDraw: {
        const auto* c = static_cast<const CmdCpRawDraw*>(p);
        aurora::gx::fifo::submit_raw_draw(static_cast<GXPrimitive>(c->prim), static_cast<GXVtxFmt>(c->fmt),
                                          reinterpret_cast<const uint8_t*>(c + 1), static_cast<uint16_t>(c->vtxCount),
                                          c->bytes);
        break;
    }
    case kCmdCpCall: {
        const auto* c = static_cast<const CmdCpCall*>(p);
        ++g_brawl3dsVtxGen;  // a GX call: it may set an array or a vertex format directly
        c->fn(const_cast<CmdCpCall*>(c) + 1);
        aurora::gx::fifo::drain();  // what the call wrote into the worker's own FIFO
        break;
    }
    case kCmdCpEndFrame: CpEndFrame(); break;
    default: break;
    }
}

void ExecCp(const CmdHeader* h)
{
    const uint64_t start = svcGetSystemTick();
    try {
        ProcessCpCommand(h);
    } catch (const std::exception& e) {
        // On the game thread the runtime's GX entry points swallowed these (GX__CallDisplayList_80172f64).
        if (g_cpFailures++ < 8) std::fprintf(stderr, "[pica] GX command processor: %s\n", e.what());
    } catch (...) {
        if (g_cpFailures++ < 8) std::fprintf(stderr, "[pica] GX command processor: unknown exception\n");
    }
    const uint64_t ticks = svcGetSystemTick() - start;
    g_cpTicks += ticks;
    g_winCpTicks += ticks;
}

inline C3D_FVec Row(const aurora::Vec4<float>& r) { return FVec4_New(r[0], r[1], r[2], r[3]); }

inline void SetVec(int id, const C3D_FVec& v)
{
    C3D_FVec& cur = g_fvShadow[id];
    uint32_t a[4], b[4];  // four words: newlib's memcmp goes byte by byte, ~25,000 times a frame
    std::memcpy(a, &cur, sizeof(a));
    std::memcpy(b, &v, sizeof(b));
    if (((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) != 0) {
        cur = v;
        auto* c = Record<CmdFVUnif>(kCmdFVUnif);
        c->id = id;
        c->v = v;
        Publish(c);
    }
}

inline void SetBoolUniform(int loc, bool on)
{
    const uint16_t bit = static_cast<uint16_t>(BIT(loc - 0x68));
    if (((g_boolShadow & bit) != 0) == on) return;
    g_boolShadow = static_cast<uint16_t>(on ? g_boolShadow | bit : g_boolShadow & ~bit);
    auto* c = Record<CmdBoolUnif>(kCmdBoolUnif);
    c->id = loc;
    c->on = on;
    Publish(c);
}

inline void SetIntUniform(int loc, int x, int y, int z, int w)
{
    const C3D_IVec v = IVec_Pack(x, y, z, w);
    if (g_ivShadow[loc - 0x60] == v) return;
    g_ivShadow[loc - 0x60] = v;
    auto* c = Record<CmdIVUnif>(kCmdIVUnif);
    c->id = loc, c->x = x, c->y = y, c->z = z, c->w = w;
    Publish(c);
}

void BeginFrameIfNeeded()
{
    if (g_recFrameOpen) return;
    g_recFrameOpen = true;
    const auto& cc = g_gxState.clearColor;
    auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    auto* c = Record<CmdBegin>(kCmdBegin);
    c->clearColor = byte(cc[0]) << 24 | byte(cc[1]) << 16 | byte(cc[2]) << 8 | 0xFF;
    Publish(c);
}

void RecordFlush()
{
    if (!g_recBatchOpen) return;
    g_recBatchOpen = false;
    RecordBare(kCmdFlush);
}

// Uniforms for a batch: the GX projection, position matrices and the texture-coordinate generators of its units.
void UploadUniforms()
{
    // The GX viewport, folded into the projection: GX maps clip space onto the viewport's rectangle of the EFB and
    // its depth range; the PICA maps it onto the whole screen and depth = z/w + 1 (ApplyFixedState). So x and y are
    // scaled and moved to where the viewport sits in the 640x456 EFB, and z so that z/w + 1 is GX's screen depth
    // zfar + (zfar - znear) * z/w. A viewport covering the EFB (any height from 448 to 528) is drawn as the whole
    // screen.
    const auto& vp = g_gxState.logicalViewport;
    float sx = 1, ox = 0, sy = 1, oy = 0;
    const bool whole = std::fabs(vp.left) < 0.5f && std::fabs(vp.top) < 0.5f && std::fabs(vp.width - kEfbWidth) < 0.5f &&
                       vp.height >= 448.0f && vp.height <= 528.0f;
    if (!whole && vp.width > 0 && vp.height > 0) {
        sx = vp.width / kEfbWidth;
        ox = (2.0f * vp.left + vp.width) / kEfbWidth - 1.0f;
        sy = vp.height / kEfbHeight;
        oy = 1.0f - (2.0f * vp.top + vp.height) / kEfbHeight;
        ++g_statPartViewport;
    }
    const float zn = std::clamp(vp.znear, 0.0f, 1.0f), zf = std::clamp(vp.zfar, 0.0f, 1.0f);
    const float sz = zf - zn, oz = zf - 1.0f;
    // The GX projection with the top screen's quarter turn folded in: clip (x, y) -> (y, -x), i.e. rows 1 and -0.
    const auto& p = g_gxState.proj;
    auto mix = [](const aurora::Vec4<float>& a, float ka, const aurora::Vec4<float>& b, float kb) {
        return FVec4_New(a[0] * ka + b[0] * kb, a[1] * ka + b[1] * kb, a[2] * ka + b[2] * kb, a[3] * ka + b[3] * kb);
    };
    SetVec(g_projLoc + 0, mix(p.m1, sy, p.m3, oy));
    SetVec(g_projLoc + 1, mix(p.m0, -sx, p.m3, -ox));
    SetVec(g_projLoc + 2, mix(p.m2, sz, p.m3, oz));
    SetVec(g_projLoc + 3, FVec4_New(p.m3[0], p.m3[1], p.m3[2], p.m3[3]));
    for (u32 i = 0; i < 10 && i < MaxPnMtx; ++i) {
        const auto& m = g_gxState.pnMtx[i].pos;
        SetVec(g_mtxLoc + i * 3 + 0, Row(m.m0));
        SetVec(g_mtxLoc + i * 3 + 1, Row(m.m1));
        SetVec(g_mtxLoc + i * 3 + 2, Row(m.m2));
    }

    // Per unit: the source vector's weights (uv set A, uv set B, object position, object normal) and the texture
    // matrix, rows s, t and q (q = 1 for a 2x4 matrix). Textures are stretched to their PICA size, so GX
    // coordinates apply unchanged.
    for (int u = 0; u < kMaxUnits; ++u) {
        C3D_FVec s = FVec4_New(1, 0, 0, 0), m0 = FVec4_New(1, 0, 0, 0), m1 = FVec4_New(0, 1, 0, 0),
                 m2 = FVec4_New(0, 0, 0, 1);
        if (u < g_plan.unitCount) {
            const auto& tcg = g_gxState.tcgs[g_plan.units[u].texCoord & 7];
            const int src = static_cast<int>(tcg.src);
            if (src == GX_TG_POS) {
                s = FVec4_New(0, 0, 1, 0);
            } else if (src == GX_TG_NRM && !(g_debugFlags & 4)) {
                s = FVec4_New(0, 0, 0, 1);
            } else if (src >= GX_TG_TEX0 && src <= GX_TG_TEX0 + 7 && g_plan.unitSet[u] == 1) {
                s = FVec4_New(0, 1, 0, 0);  // set B
            }
            const bool threeRows = tcg.type == GX_TG_MTX3x4;
            if (tcg.mtx != GX_IDENTITY && tcg.mtx >= GX_TEXMTX0) {
                const auto& m = g_gxState.texMtxs[std::min<u32>((tcg.mtx - GX_TEXMTX0) / 3, MaxTexMtx - 1)];
                m0 = Row(m.m0);
                m1 = Row(m.m1);
                if (threeRows) m2 = Row(m.m2);
            } else if (threeRows) {
                m2 = FVec4_New(0, 0, 1, 0);
            }
            // GX's dual texture transform: with it on, the post-transform matrix takes (s, t, q of the first matrix,
            // 1) (q = 1 for a 2x4 matrix): folded into the rows here. A 2x4 generator keeps q = 1 (Aurora outputs its
            // s, t undivided). Normalizing in between is not done (counted).
            if ((g_gxState.dualTex & 1u) && tcg.postMtx != GX_PTIDENTITY && tcg.postMtx >= GX_PTTEXMTX0) {
                const auto& pt = g_gxState.ptTexMtxs[std::min<u32>((tcg.postMtx - GX_PTTEXMTX0) / 3, MaxPTTexMtx - 1)];
                const C3D_FVec a = m0, b = m1, c = m2;
                auto fold = [&](const aurora::Vec4<float>& r) {
                    return FVec4_New(r[0] * a.x + r[1] * b.x + r[2] * c.x, r[0] * a.y + r[1] * b.y + r[2] * c.y,
                                     r[0] * a.z + r[1] * b.z + r[2] * c.z,
                                     r[0] * a.w + r[1] * b.w + r[2] * c.w + r[3]);
                };
                m0 = fold(pt.m0);
                m1 = fold(pt.m1);
                if (threeRows) m2 = fold(pt.m2);
                ++g_statPostMtx;
                if (tcg.normalize) ++g_statPostNormalize;
            }
        }
        SetVec(g_texSrcLoc + u, s);
        SetVec(g_texMtxLoc + u * 3 + 0, m0);
        SetVec(g_texMtxLoc + u * 3 + 1, m1);
        SetVec(g_texMtxLoc + u * 3 + 2, m2);
    }
}

// Lighting statistics for the match summary.
uint32_t g_statBatches = 0, g_statLit = 0, g_statLitApprox = 0, g_statLights = 0, g_statColor1 = 0;

void BuildChannel(ChannelPlan& ch)
{
    auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const auto& cc = g_gxState.colorChannelConfig[0];  // GX_COLOR0
    const auto& cs = g_gxState.colorChannelState[0];
    const auto& ac = g_gxState.colorChannelConfig[2];  // GX_ALPHA0
    const auto& as = g_gxState.colorChannelState[2];
    const bool none = g_gxState.numChans == 0;
    ch.rgbFromVertex = !none && cc.matSrc == GX_SRC_VTX;
    ch.alphaFromVertex = !none && ac.matSrc == GX_SRC_VTX;
    for (int c = 0; c < 3; ++c) ch.rgb[c] = none ? 255 : byte(cs.matColor[c]);
    ch.alpha = none ? 255 : byte(as.matColor[3]);
    const bool litRgb = !none && cc.lightingEnabled, litAlpha = !none && ac.lightingEnabled;
    ch.lit = litRgb || litAlpha;
    ++g_statBatches;
    for (u32 s = 0; s < g_gxState.numTevStages && s < MaxTevStages; ++s)
        if (g_gxState.tevStages[s].channelId == GX_COLOR1A1) {
            ++g_statColor1;
            break;
        }
    SetBoolUniform(g_litLoc, ch.lit);
    if (!ch.lit) return;
    ++g_statLit;

    // The light model: COLOR0's, or ALPHA0's when only the alpha is lit. Both lit with different models (or an
    // ambient from the vertex color) is drawn approximately.
    const ColorChannelConfig& cfg = litRgb ? cc : ac;
    if ((litRgb && litAlpha && (cc.diffFn != ac.diffFn || cc.attnFn != ac.attnFn)) ||
        (litRgb && cc.ambSrc == GX_SRC_VTX) || (litAlpha && ac.ambSrc == GX_SRC_VTX))
        ++g_statLitApprox;
    const bool spec = cfg.attnFn == GX_AF_SPEC;
    const bool useDot = cfg.diffFn != GX_DF_NONE;
    SetVec(g_lselLoc, FVec4_New(spec ? 1.0f : 0.0f, useDot ? 1.0f : 0.0f, cfg.diffFn == GX_DF_SIGN ? -2.0f : 0.0f, 0));
    // Ambient, 1 for a part that is not lit (with its lights' color 0 there, lacc = 1: the material alone).
    SetVec(g_ambLoc, FVec4_New(litRgb ? cs.ambColor[0] : 1.0f, litRgb ? cs.ambColor[1] : 1.0f,
                               litRgb ? cs.ambColor[2] : 1.0f, litAlpha ? as.ambColor[3] : 1.0f));
    uint32_t n = 0;
    for (u32 i = 0; i < GX::MaxLights && n < kMaxLights; ++i) {
        const bool inRgb = litRgb && cs.lightMask.test(i), inAlpha = litAlpha && as.lightMask.test(i);
        if (!inRgb && !inAlpha) continue;
        const Light& l = g_gxState.lights[i];
        const int at = g_lightsLoc + static_cast<int>(n) * 5;
        // A light placed "at infinity" (directional lights sit ~1e18 away) overflows the shader's 24-bit floats
        // (max ~9e18) in the squared distance: brought in to 1e6, which keeps its direction from the scene.
        float px = l.pos[0], py = l.pos[1], pz = l.pos[2];
        const float far = std::max({std::fabs(px), std::fabs(py), std::fabs(pz)});
        if (far > 1e6f) {
            const float k = 1e6f / far;
            px *= k, py *= k, pz *= k;
        }
        SetVec(at + 0, FVec4_New(px, py, pz, 0));
        SetVec(at + 1, FVec4_New(l.dir[0], l.dir[1], l.dir[2], 0));
        SetVec(at + 2, FVec4_New(inRgb ? l.color[0] : 0.0f, inRgb ? l.color[1] : 0.0f, inRgb ? l.color[2] : 0.0f,
                                 inAlpha ? l.color[3] : 0.0f));
        if (cfg.attnFn == GX_AF_NONE) {
            SetVec(at + 3, FVec4_New(1, 0, 0, 0));
            SetVec(at + 4, FVec4_New(1, 0, 0, 0));
        } else {
            SetVec(at + 3, FVec4_New(l.cosAtt[0], l.cosAtt[1], l.cosAtt[2], 0));
            float k0 = l.distAtt[0], k1 = l.distAtt[1], k2 = l.distAtt[2];
            if (spec && useDot) {  // GX normalizes the specular distance coefficients for a lit diffuse
                const float len = std::sqrt(k0 * k0 + k1 * k1 + k2 * k2);
                if (len > 0) k0 /= len, k1 /= len, k2 /= len;
            }
            SetVec(at + 4, FVec4_New(k0, k1, k2, 0));
        }
        ++n;
    }
    g_statLights += n;
    if (n == 0) {  // the loop runs at least once: one light that adds nothing
        SetVec(g_lightsLoc + 0, FVec4_New(0, 0, 0, 0));
        SetVec(g_lightsLoc + 2, FVec4_New(0, 0, 0, 0));
        SetVec(g_lightsLoc + 3, FVec4_New(1, 0, 0, 0));
        SetVec(g_lightsLoc + 4, FVec4_New(1, 0, 0, 0));
        n = 1;
    }
    SetIntUniform(g_lightLoopLoc, static_cast<int>(n) - 1, 0, 5, 0);
}

// Per-vertex fog (gx_m4.v.pica): the GX fog parameters as uniforms, the ramp on unit 2.
void UploadVertexFog()
{
    const bool on = g_plan.vertexFog;
    if (on) {
        auto* r = Record<CmdBindRamp>(kCmdBindRamp);
        r->unit = 2;
        Publish(r);
        const auto& f = g_gxState.fog;
        const int type = static_cast<int>(f.type);
        const bool persp = (type & 8) == 0;
        const int fn = type & 7;
        SetVec(g_fogpLoc + 0,
               FVec4_New(f.aRaw * 16777216.0f, static_cast<float>(f.bMagnitude),
                         std::ldexp(1.0f, 24 - static_cast<int>(std::min<uint32_t>(f.bShift, 31))), f.c));
        SetVec(g_fogpLoc + 1, FVec4_New(persp ? 1.0f : 0.0f, persp ? 0.0f : 1.0f, f.aRaw, 0.0f));
        SetVec(g_fogpLoc + 2, FVec4_New(fn == 6 || fn == 7 ? 1.0f : 0.0f, fn == 5 || fn == 7 ? 1.0f : 0.0f,
                                        fn <= 3 ? 1.0f : 0.0f, 0.0f));
    }
    SetBoolUniform(g_fogvLoc, on);
}

// The batch's textures: the GX texture objects (and palettes) as they are now; the worker decodes and binds them.
void RecordTextures()
{
    for (int u = 0; u < g_plan.unitCount; ++u) {
        const int texMap = g_plan.units[u].texMap;
        auto* c = Record<CmdBindTex>(kCmdBindTex);
        c->unit = u;
        c->hasTlut = 0;
        if (texMap >= 0 && texMap < static_cast<int>(MaxTextures)) {
            c->obj = g_gxState.loadedTextures[texMap];
            const uint32_t fmt = c->obj.format();
            const bool paletted = fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2;
            if (paletted && static_cast<uint32_t>(c->obj.tlut) < MaxTluts) {
                c->tlut = g_gxState.loadedTluts[c->obj.tlut];
                c->hasTlut = 1;
            }
        } else {
            c->obj = GXTexObj_{};  // no data: the worker binds its white texture
        }
        Publish(c);
    }
}

void BuildPlan(GXVtxFmt fmt, bool be, VertexPlan& plan)
{
    const auto& vf = g_gxState.vtxFmts[fmt];
    const auto& desc = g_gxState.vtxDesc;
    std::memset(&plan, 0, sizeof(plan));  // whole bytes: plans are compared with memcmp
    plan.pnMtxOffset = -1;
    plan.defaultRow = static_cast<int8_t>(g_gxState.currentPnMtx * 3);
    plan.streamBe = be;
    uint32_t at = 0;
    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr) {
        const GXAttrType type = desc[attr];
        if (type == GX_NONE) continue;
        if (attr <= GX_VA_TEX7MTXIDX) {  // matrix indices: one byte, always direct
            if (attr == GX_VA_PNMTXIDX) plan.pnMtxOffset = static_cast<int>(at);
            ++at;
            continue;
        }
        const auto& af = vf.attrs[attr];
        AttrPlan a;
        std::memset(&a, 0, sizeof(a));
        a.offset = at;
        a.type = af.type;
        a.be = be;
        a.scale = af.type == GX_F32 ? 1.0f : 1.0f / static_cast<float>(1u << af.frac);
        a.shift = af.type == GX_F32 ? 0 : static_cast<uint8_t>(af.frac);
        if (type == GX_DIRECT) {
            at += DirectSize(attr, af);
        } else {
            const uint32_t idxBytes = type == GX_INDEX16 ? 2 : 1;
            a.index = idxBytes;
            at += (attr == GX_VA_NRM && af.cnt == GX_NRM_NBT3) ? idxBytes * 3 : idxBytes;
            const auto& array = g_gxState.arrays[attr];
            a.base = static_cast<const uint8_t*>(array.data);
            a.stride = array.stride;
            a.be = !array.le;
            if (!a.base) continue;  // no array set: nothing to read
        }
        if (attr == GX_VA_POS) {
            a.two = af.cnt == GX_POS_XYZ;
            plan.hasPos = true;
            plan.pos = a;
        } else if (attr == GX_VA_CLR0) {
            plan.hasClr = true;
            plan.clr = a;
        } else if (attr == GX_VA_NRM) {  // the normal is the first three components (NBT: of the first index)
            plan.hasNrm = true;
            plan.nrm = a;
        } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
            a.two = af.cnt == GX_TEX_ST;
            if (g_plan.uvAttr[0] == attr) plan.hasUv = true, plan.uv = a;
            if (g_plan.uvAttr[1] == attr) plan.hasUv2 = true, plan.uv2 = a;
        }
    }
    plan.size = at;
}

void DebugDumpBatch(GXVtxFmt fmt, const uint8_t* vertices, uint32_t vtxCount, uint32_t indexCount,
                    const VertexPlan& vplan);
} // namespace

// The recorders pica_tev_3ds.cpp uses (pica_internal.hpp).
namespace brawl3ds_pica {
void RecTexEnv(int stage, const C3D_TexEnv& env)
{
    auto* c = Record<CmdTexEnv>(kCmdTexEnv);
    c->stage = stage;
    c->env = env;
    Publish(c);
}
void RecTexEnvBufUpdate(int rgbMask, int alphaMask)
{
    auto* c = Record<CmdTexEnvBuf>(kCmdTexEnvBuf);
    c->rgb = rgbMask, c->alpha = alphaMask;
    Publish(c);
}
void RecAlphaTest(bool on, GPU_TESTFUNC func, int ref)
{
    auto* c = Record<CmdAlphaTest>(kCmdAlphaTest);
    c->on = on, c->func = func, c->ref = ref;
    Publish(c);
}
void RecBlend(int kind, int src, int dst, int logicOp)
{
    auto* c = Record<CmdBlend>(kCmdBlend);
    c->kind = kind, c->src = src, c->dst = dst, c->logicOp = logicOp;
    Publish(c);
}
void RecDepthTest(GPU_TESTFUNC func, int writeMask)
{
    auto* c = Record<CmdDepth>(kCmdDepth);
    c->func = func, c->mask = writeMask;
    Publish(c);
}
void RecCull(GPU_CULLMODE mode)
{
    auto* c = Record<CmdCull>(kCmdCull);
    c->mode = mode;
    Publish(c);
}
void RecFogMode(bool on)
{
    auto* c = Record<CmdFogMode>(kCmdFogMode);
    c->on = on;
    Publish(c);
}
void RecFogColor(uint32_t color)
{
    auto* c = Record<CmdFogColor>(kCmdFogColor);
    c->color = color;
    Publish(c);
}
void RecFogLut(const C3D_FogLut& lut)
{
    auto* c = Record<CmdFogLut>(kCmdFogLut);
    c->lut = lut;
    Publish(c);
}
} // namespace brawl3ds_pica

// A full draw from Aurora: `vertices` are its raw GX vertices, `indices` its triangle list (relative to them).
void Brawl3dsPicaDraw(GXVtxFmt fmt, const uint8_t* vertices, uint32_t vtxCount, const uint16_t* indices,
                      uint32_t indexCount, bool bigEndian)
{
    StartRenderer();
    BeginFrameIfNeeded();
    RecordFlush();  // the open batch is drawn with its own state, before this batch's is recorded
    // The batch's material: combiners and fragment state, its textures, the uniforms and the channel color.
    ApplyMaterial(g_plan);
    RecordTextures();
    UploadUniforms();
    BuildChannel(g_channel);
    UploadVertexFog();
    const int layout = g_plan.uvAttr[1] >= 0 ? 1 : 0;
    SetBoolUniform(g_twosetLoc, layout == 1);

    auto* c = Record<CmdDraw>(kCmdDraw);
    BuildPlan(fmt, bigEndian, c->plan);
    c->ch = g_channel;
    c->normals = g_channel.lit || g_plan.needsNormals;
    c->layout = static_cast<uint8_t>(layout);
    c->skip = g_plan.skip;
    c->pad = 0;
    c->vertices = vertices;
    c->vtxCount = vtxCount;
    c->indices = indices;
    c->indexCount = indexCount;
    g_recPlan = c->plan;
    g_recPlanGen = g_brawl3dsVtxGen;
    g_recPlanFmt = static_cast<int>(fmt);
    g_recPlanBe = bigEndian;
    g_recPlanMemo = OnWorker();  // only the worker's own state is counted
    const VertexPlan dumpPlan = c->plan;
    Publish(c);
    g_recBatchOpen = true;
    g_recBatchVerts = vtxCount;

    ++g_debugSerial;
    if (g_debugFrame) DebugDumpBatch(fmt, vertices, vtxCount, indexCount, dumpPlan);
}

extern "C" void Brawl3dsPicaDebugNextFrame()
{
    g_debugFrame = true;
    g_debugBatches = 0;
    g_debugSerial = 0;
}

// An unchanged-state draw appended to the open batch; false when there is none (the caller draws it in full). Its
// vertices are copied into the ring: they can sit in the runtime's FIFO buffer, reused when this returns.
bool Brawl3dsPicaAppend(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint32_t vtxCount, bool bigEndian)
{
    if (!g_recBatchOpen) return false;
    if (g_recBatchVerts + vtxCount > 0xFFFF) {
        ++g_recDropped;
        return true;  // dropped, but handled
    }
    const bool memo = g_recPlanMemo && g_recPlanGen == g_brawl3dsVtxGen && g_recPlanFmt == static_cast<int>(fmt) &&
                      g_recPlanBe == bigEndian && OnWorker();
    if (!memo) {
        VertexPlan plan;
        BuildPlan(fmt, bigEndian, plan);
        if (std::memcmp(&plan, &g_recPlan, sizeof(plan)) != 0) {  // arrays or byte order changed under the batch
            g_recPlan = plan;
            auto* p = Record<CmdPlan>(kCmdPlan);
            p->plan = plan;
            Publish(p);
        }
        g_recPlanGen = g_brawl3dsVtxGen;
        g_recPlanFmt = static_cast<int>(fmt);
        g_recPlanBe = bigEndian;
        g_recPlanMemo = OnWorker();
    }
    const VertexPlan& plan = g_recPlan;
    const uint32_t bytes = vtxCount * plan.size;
    if (CmdSize(sizeof(CmdAppend) + bytes) > 0xFFF8) {  // larger than a command can be: drawn in full instead
        return false;
    }
    auto* a = Record<CmdAppend>(kCmdAppend, bytes);
    a->prim = prim;
    a->vtxCount = vtxCount;
    a->bytes = bytes;
    std::memcpy(a + 1, vertices, bytes);
    Publish(a);
    g_recBatchVerts += vtxCount;
    return true;
}

// The clear of an EFB texture copy (GXCopyTex with clear): the copy's rectangle (EFB pixels) takes the copy clear
// color and depth, as on GX. Brawl's shadow pass draws the fighters in one color into a corner of the EFB, copies
// that into a texture and clears it; without the clear the mask stayed on screen. A copy of (nearly) the whole
// screen is not cleared: the copy itself is not made yet, so that would only erase the frame.
void Brawl3dsPicaClearRect(int x, int y, int w, int h, bool color, bool alpha, bool depth)
{
    if (!color && !alpha && !depth) return;
    if (w <= 0 || h <= 0) return;
    if (w >= kEfbWidth * 0.9f && h >= kEfbHeight * 0.9f) {
        ++g_statClearsSkipped;
        return;
    }
    StartRenderer();
    BeginFrameIfNeeded();
    RecordFlush();
    // Uniforms: position matrix 0 = identity, the projection only turns the screen (the next batch's
    // UploadUniforms sets its own again), no lighting, fog or set B; then the clear's fixed-function state.
    SetVec(g_projLoc + 0, FVec4_New(0, 1, 0, 0));
    SetVec(g_projLoc + 1, FVec4_New(-1, 0, 0, 0));
    SetVec(g_projLoc + 2, FVec4_New(0, 0, 1, 0));
    SetVec(g_projLoc + 3, FVec4_New(0, 0, 0, 1));
    SetVec(g_mtxLoc + 0, FVec4_New(1, 0, 0, 0));
    SetVec(g_mtxLoc + 1, FVec4_New(0, 1, 0, 0));
    SetVec(g_mtxLoc + 2, FVec4_New(0, 0, 1, 0));
    SetBoolUniform(g_litLoc, false);
    SetBoolUniform(g_fogvLoc, false);
    SetBoolUniform(g_twosetLoc, false);
    ApplyClearState(color, alpha, depth);
    // The quad in GX clip space (y up), at the clear depth: depth = z + 1 with w = 1, kept off the far plane.
    auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const auto& cc = g_gxState.clearColor;
    auto* c = Record<CmdClear>(kCmdClear);
    c->x0 = 2.0f * static_cast<float>(x) / kEfbWidth - 1.0f;
    c->x1 = 2.0f * static_cast<float>(x + w) / kEfbWidth - 1.0f;
    c->y0 = 1.0f - 2.0f * static_cast<float>(y) / kEfbHeight;
    c->y1 = 1.0f - 2.0f * static_cast<float>(y + h) / kEfbHeight;
    c->z = std::min(static_cast<float>(g_gxState.clearDepth & 0xFFFFFF) / 16777215.0f - 1.0f, -1.0f / 65536.0f);
    c->rgba[0] = byte(cc[0]), c->rgba[1] = byte(cc[1]), c->rgba[2] = byte(cc[2]), c->rgba[3] = byte(cc[3]);
    Publish(c);
}

namespace {
// One frame's debug dump (Brawl3dsPicaDebugNextFrame): the first batches, then the ones whose material may be
// drawn wrong (approximated, three or more TEV stages, two texture-coordinate sets, coordinates from normals), with
// their TEV stages. Kept to 40 batches: each line goes to the SD card, and a 60-batch dump with every fogged batch
// made the title's dump frame take 6.4 s on the console. The first vertex is decoded here for it.
void DebugDumpBatch(GXVtxFmt fmt, const uint8_t* vertices, uint32_t vtxCount, uint32_t indexCount,
                    const VertexPlan& vplan)
{
    const bool suspect = g_plan.approximated || g_gxState.numTevStages >= 3 || g_plan.uvAttr[1] >= 0 ||
                         g_plan.needsNormals;
    if (!(g_debugSerial <= 6 || (suspect && g_debugBatches < 40))) return;
    ++g_debugBatches;
    PicaVertex2 v{};
    if (vtxCount) DecodeInto(vplan, g_channel, true, vertices, 1, &v);
    std::fprintf(stderr, "[pica] -- batch #%u of the frame: %s%s%d PICA stages\n", g_debugSerial,
                 g_plan.approximated ? "approximated, " : "", suspect ? "suspect, " : "", g_plan.picaStages);
    for (u32 s = 0; s < g_gxState.numTevStages && s < MaxTevStages; ++s) {
        const TevStage& st = g_gxState.tevStages[s];
        std::fprintf(stderr,
                     "[pica]    tev %u: color %d %d %d %d op %d bias %d scale %d out %d | alpha %d %d %d %d op %d "
                     "bias %d scale %d out %d | map %d coord %d chan %d kc %d ka %d swap %d/%d ind %d\n",
                     s, st.colorPass.a, st.colorPass.b, st.colorPass.c, st.colorPass.d, st.colorOp.op,
                     st.colorOp.bias, st.colorOp.scale, st.colorOp.outReg, st.alphaPass.a, st.alphaPass.b,
                     st.alphaPass.c, st.alphaPass.d, st.alphaOp.op, st.alphaOp.bias, st.alphaOp.scale,
                     st.alphaOp.outReg, static_cast<int>(st.texMapId), static_cast<int>(st.texCoordId),
                     static_cast<int>(st.channelId), static_cast<int>(st.kcSel), static_cast<int>(st.kaSel),
                     static_cast<int>(st.tevSwapRas), static_cast<int>(st.tevSwapTex),
                     static_cast<int>(st.indTexMtxId));
    }
    // Blending, alpha test and the TEV constants (the look of layout effects depends on them).
    {
        const auto& ac = g_gxState.alphaCompare;
        const auto& k = g_gxState.kcolors;
        const auto& c = g_gxState.colorRegs;
        std::fprintf(stderr,
                     "[pica]   blend %d src %d dst %d, alpha test %d %u %d %d %u, color/alpha update %d/%d; "
                     "K0-3 %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f; "
                     "C0-2 %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f\n",
                     static_cast<int>(g_gxState.blendMode), static_cast<int>(g_gxState.blendFacSrc),
                     static_cast<int>(g_gxState.blendFacDst), static_cast<int>(ac.comp0), ac.ref0,
                     static_cast<int>(ac.op), static_cast<int>(ac.comp1), ac.ref1, g_gxState.colorUpdate ? 1 : 0,
                     g_gxState.alphaUpdate ? 1 : 0, k[0][0], k[0][1], k[0][2], k[0][3], k[1][0], k[1][1],
                     k[1][2], k[1][3], k[2][0], k[2][1], k[2][2], k[2][3], k[3][0], k[3][1], k[3][2], k[3][3],
                     c[1][0], c[1][1], c[1][2], c[1][3], c[2][0], c[2][1], c[2][2], c[2][3], c[3][0], c[3][1],
                     c[3][2], c[3][3]);
    }
    // The first vertex's GX screen depth (position matrix, projection, viewport depth range) and the fog there.
    if (g_plan.fog) {
        const auto& m = g_gxState.pnMtx[std::min<u32>(static_cast<u32>(v.row) / 3, MaxPnMtx - 1)].pos;
        const float vx = m.m0[0] * v.x + m.m0[1] * v.y + m.m0[2] * v.z + m.m0[3];
        const float vy = m.m1[0] * v.x + m.m1[1] * v.y + m.m1[2] * v.z + m.m1[3];
        const float vz = m.m2[0] * v.x + m.m2[1] * v.y + m.m2[2] * v.z + m.m2[3];
        const auto& pr = g_gxState.proj;
        const float cz = pr.m2[0] * vx + pr.m2[1] * vy + pr.m2[2] * vz + pr.m2[3];
        const float cw = pr.m3[0] * vx + pr.m3[1] * vy + pr.m3[2] * vz + pr.m3[3];
        const auto& vpz = g_gxState.logicalViewport;
        const float d = cw != 0.0f ? vpz.zfar + (vpz.zfar - vpz.znear) * cz / cw : -1.0f;
        const auto& fg = g_gxState.fog;
        std::fprintf(stderr,
                     "[pica]   depth %.6f; fog type %d a %g b %u >> %u c %g color %.2f %.2f %.2f: amount %.3f "
                     "(at 0 %.3f, 0.5 %.3f, 0.99 %.3f, 1 %.3f)",
                     d, static_cast<int>(fg.type), fg.aRaw, fg.bMagnitude, fg.bShift, fg.c, fg.color[0],
                     fg.color[1], fg.color[2], FogAmountAt(d), FogAmountAt(0.0f), FogAmountAt(0.5f),
                     FogAmountAt(0.99f), FogAmountAt(1.0f));
        if (g_plan.uvAttr[1] >= 0) std::fprintf(stderr, ", uv2 (%.3f %.3f)", v.uv2[0] / kUvScale, v.uv2[1] / kUvScale);
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr,
                 "[pica] batch %u: fmt %d, %u verts, %u indices, tev %u stages, %d units (", g_debugBatches,
                 static_cast<int>(fmt), vtxCount, indexCount, g_gxState.numTevStages, g_plan.unitCount);
    for (int u = 0; u < g_plan.unitCount; ++u) {
        const auto& t = g_gxState.loadedTextures[g_plan.units[u].texMap];
        const auto& tcg = g_gxState.tcgs[g_plan.units[u].texCoord & 7];
        std::fprintf(stderr,
                     "%smap %u coord %u (src %d mtx %d post %d%s%s type %d set %u) %ux%u fmt %u wrap %d/%d",
                     u ? ", " : "", g_plan.units[u].texMap, g_plan.units[u].texCoord, static_cast<int>(tcg.src),
                     static_cast<int>(tcg.mtx), static_cast<int>(tcg.postMtx),
                     (g_gxState.dualTex & 1u) ? " dual" : "", tcg.normalize ? " norm" : "",
                     static_cast<int>(tcg.type), g_plan.unitSet[u], t.width(), t.height(), t.format(),
                     static_cast<int>(t.wrap_s()), static_cast<int>(t.wrap_t()));
    }
    const auto& vp = g_gxState.logicalViewport;
    std::fprintf(stderr, "), viewport %.0f %.0f %.0fx%.0f z %.3f-%.3f, fog %d", vp.left, vp.top, vp.width,
                 vp.height, vp.znear, vp.zfar, static_cast<int>(g_gxState.fog.type));
    std::fprintf(stderr,
                 ", uv attrs %d %d, blend %d, z %d/%d/%d, cull %d, skip %d\n"
                 "[pica]   vertex (%.3f %.3f %.3f) rgba %02x%02x%02x%02x row %u uv (%.3f %.3f)\n",
                 g_plan.uvAttr[0], g_plan.uvAttr[1], static_cast<int>(g_gxState.blendMode),
                 g_gxState.depthCompare ? 1 : 0, static_cast<int>(g_gxState.depthFunc),
                 g_gxState.depthUpdate ? 1 : 0, static_cast<int>(g_gxState.cullMode), g_plan.skip ? 1 : 0, v.x,
                 v.y, v.z, v.r, v.g, v.b, v.a, v.row, v.uv[0] / kUvScale, v.uv[1] / kUvScale);
}

// A 240x400 RGB8 top-screen framebuffer copy as a 400x240 24-bit BMP.
void WriteBmp(const char* path, const uint8_t* rgb)
{
    constexpr uint32_t w = kShotW;
    if (std::FILE* f = std::fopen(path, "wb")) {
        // BMP: 400 wide, 240 tall, bottom-up rows of BGR. Framebuffer pixel (x, y) (x across 240, y down 400, BGR)
        // shows at screen (y, 239 - x).
        constexpr uint32_t sw = 400, sh = 240, rowBytes = sw * 3, size = 54 + rowBytes * sh;
        uint8_t header[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) {
            header[at] = v & 0xFF, header[at + 1] = (v >> 8) & 0xFF, header[at + 2] = (v >> 16) & 0xFF,
            header[at + 3] = v >> 24;
        };
        put32(2, size), put32(10, 54), put32(14, 40), put32(18, sw), put32(22, sh);
        header[26] = 1, header[28] = 24;
        put32(34, rowBytes * sh);
        std::fwrite(header, 1, sizeof(header), f);
        static uint8_t row[sw * 3];
        for (uint32_t sy = 0; sy < sh; ++sy) {  // bottom-up
            const uint32_t screenY = sh - 1 - sy;
            for (uint32_t sx = 0; sx < sw; ++sx) {
                const uint32_t fx = sh - 1 - screenY, fy = sx;
                const uint8_t* px = rgb + (fy * w + fx) * 3;
                row[sx * 3 + 0] = px[0], row[sx * 3 + 1] = px[1], row[sx * 3 + 2] = px[2];
            }
            std::fwrite(row, 1, sizeof(row), f);
        }
        std::fclose(f);
    }
}
} // namespace

unsigned brawl3ds_pica::g_debugFlags = 0;
extern "C" void Brawl3dsPicaSetDebugFlags(unsigned flags) { brawl3ds_pica::g_debugFlags = flags; }

// Called between frames (the worker is idle then).
extern "C" void Brawl3dsPicaCaptureNextFrame(const char* path)
{
    if (!g_ready || !g_lastTopFramebuffer) return;
    g_pendingCaptureSource = g_lastTopFramebuffer;
    std::snprintf(g_pendingCapturePath, sizeof(g_pendingCapturePath), "%s", path);
}

extern "C" void Brawl3dsPicaWriteCaptures()
{
    if (g_started) Drain();
    for (uint32_t i = 0; i < g_captureCount; ++i) {
        WriteBmp(g_captures[i].path, g_captures[i].rgb);
        std::free(g_captures[i].rgb);
    }
    g_captureCount = 0;
}

// Material and texture statistics for the match summary (headless.log). Called between frames.
extern "C" void Brawl3dsPicaWriteStats(std::FILE* f, unsigned frames)
{
    if (g_started) Drain();
    if (f) {
        const double div = frames ? frames : 1;
        auto ms = [&](uint64_t ticks) { return static_cast<double>(ticks) * 1000.0 / SYSCLOCK_ARM11 / div; };
        std::fprintf(f, "GPU stalls (frames not drawn after 2 s of waiting): %u\n", g_gpuStalls);
        std::fprintf(f,
                     "GPU work: %s; worker busy %.2f ms per frame; game thread waited %.2f ms per frame for it at frame "
                     "ends and %.2f ms for ring space; %.0f KiB recorded per frame (ring %u KiB, peak %u KiB); %.1f "
                     "worker wake-ups per frame\n",
                     g_workerOn ? "worker thread on core 2" : "on the game thread", ms(g_workerBusyTicks),
                     ms(g_drainWaitTicks), ms(g_spaceWaitTicks), static_cast<double>(g_ringBytesRecorded) / 1024.0 / div,
                     g_ringBytes >> 10, g_ringPeak >> 10, g_wakeSignals / div);
        std::fprintf(f,
                     "GX command processor: %s; %.2f ms per frame on the worker (with the recording it calls), %.0f "
                     "KiB of GX commands handed over per frame, %.1f catch-up waits per frame, %u failures\n",
                     g_cpOnWorker ? "on the worker" : "on the game thread", ms(g_cpTicks),
                     static_cast<double>(g_cpBytes) / 1024.0 / div, g_cpSyncs / div, g_cpFailures);
        std::fprintf(f, "catch-up waits: %.2f ms per frame on the game thread;", ms(g_cpSyncTicks));
        for (const SyncSite& site : g_syncSites) {
            if (site.name)
                std::fprintf(f, " %s %.1f/frame %.2f ms;", site.name, site.count / div, ms(site.ticks));
        }
        std::fprintf(f, "\n");
        std::fprintf(f,
                     "lighting: %u of %u batches lit (%.1f lights each), %u lit approximately, %u read channel "
                     "COLOR1A1 (drawn as COLOR0A0)\n",
                     g_statLit, g_statBatches, g_statLit ? static_cast<double>(g_statLights) / g_statLit : 0.0,
                     g_statLitApprox, g_statColor1);
        std::fprintf(f,
                     "viewport and copies: %u batches in a part of the screen; %u EFB copy clears drawn, %u skipped "
                     "(whole screen); %u texture units with a post-transform matrix (%u normalized, not applied)\n",
                     g_statPartViewport, g_statClears, g_statClearsSkipped, g_statPostMtx, g_statPostNormalize);
    }
    g_statBatches = g_statLit = g_statLitApprox = g_statLights = g_statColor1 = 0;
    g_statPartViewport = g_statClears = g_statClearsSkipped = g_statPostMtx = g_statPostNormalize = 0;
    g_workerBusyTicks = g_drainWaitTicks = g_spaceWaitTicks = g_ringBytesRecorded = 0;
    g_ringPeak = 0;
    g_wakeSignals = 0;
    g_cpTicks = g_cpBytes = g_cpSyncTicks = 0;
    g_cpSyncs = 0;
    for (SyncSite& site : g_syncSites) site = SyncSite{};
    WriteTevStats(f, frames);
    WriteTextureStats(f, frames);
}

// backend_3ds.cpp: the frame is done with Aurora's staging (reset the arena, the arrays, the current draw).
void Brawl3dsRendererStagingEndFrame();

namespace {
// The recorder's frame end, on the thread that runs the command processor.
void RecorderEndFrame()
{
    RecordFlush();
    if (g_recFrameOpen) RecordBare(kCmdEndFrame);
    g_recFrameOpen = false;
    g_recBatchOpen = false;
    g_debugFrame = false;  // a debug dump covers one frame
}

// Step 2: the frame's end, queued by the game thread and run by the worker in order.
void CpEndFrame()
{
    RecorderEndFrame();
    Brawl3dsRendererStagingEndFrame();
}
} // namespace

// The frame's end: the worker submits it; the game thread waits until it has (Aurora's staging arena, which the
// frame's draws point into, is reset after). Step 2: the worker also decodes the frame's last GX commands and
// resets the staging itself; true when it did.
bool Brawl3dsPicaEndFrame()
{
    if (g_started && g_cpOnWorker) {
        aurora::gx::fifo::drain();  // the FIFO's last commands, then the frame's end
        RecordBare(kCmdCpEndFrame);
        Drain();
        return true;
    }
    if (g_started) {
        RecordFlush();
        if (g_recFrameOpen) RecordBare(kCmdEndFrame);
        Drain();
    }
    g_recFrameOpen = false;
    g_recBatchOpen = false;
    g_debugFrame = false;
    if (g_cpWanted && !g_cpOnWorker) {
        // Between frames, the worker idle: GX commands from now on are decoded there. The staging of this frame is
        // reset by the caller on this thread, as before.
        g_cpOnWorker = true;
        std::fprintf(stderr, "[pica] GX command processor now on the worker\n");
    }
    return false;
}

// ---- brawl3ds_cp_hooks.h ----
extern "C" bool brawl3ds_cp_on_worker(void) { return OnWorker(); }

extern "C" bool brawl3ds_cp_deferred(void)
{
    if (OnWorker()) return false;
    return g_cpOnWorker && g_cpInlineDepth == 0;
}

extern "C" bool brawl3ds_cp_defer_fifo(const uint8_t* data, uint32_t size)
{
    if (!brawl3ds_cp_deferred()) return false;
    return DeferBytes(0, true, data, size);
}

extern "C" bool brawl3ds_cp_defer_list(const uint8_t* data, uint32_t size, bool bigEndian)
{
    if (!brawl3ds_cp_deferred()) return false;
    if (size == 0) return true;
    if (g_brawl3dsListInGuestRam && size >= 128) {  // by reference (CmdCpListRef)
        const uint32_t cmd = CmdSize(sizeof(CmdCpData) + sizeof(CmdCpListRef));
        auto* h = reinterpret_cast<CmdHeader*>(Reserve(cmd));
        h->type = kCmdCpData;
        h->pad = 0;
        h->size = cmd;
        auto* c = reinterpret_cast<CmdCpData*>(h + 1);
        c->bytes = size;
        c->kind = 2;
        c->bigEndian = bigEndian ? 1 : 0;
        c->pad[0] = c->pad[1] = 0;
        auto* ref = reinterpret_cast<CmdCpListRef*>(c + 1);
        ref->data = data;
        ref->pad = 0;
        Commit(cmd);
        return true;
    }
    return DeferBytes(1, bigEndian, data, size);
}

extern "C" void brawl3ds_cp_defer_raw_draw(int prim, int fmt, const uint8_t* vertices, uint16_t vtxCount,
                                           uint32_t bytes)
{
    aurora::gx::fifo::drain();  // the commands before the draw go first
    const uint32_t size = CmdSize(sizeof(CmdCpRawDraw) + bytes);
    auto* h = reinterpret_cast<CmdHeader*>(Reserve(size));
    h->type = kCmdCpRawDraw;
    h->pad = 0;
    h->size = size;
    auto* c = reinterpret_cast<CmdCpRawDraw*>(h + 1);
    c->prim = prim;
    c->fmt = fmt;
    c->vtxCount = vtxCount;
    c->bytes = bytes;
    std::memcpy(c + 1, vertices, bytes);
    g_cpBytes += bytes;
    Commit(size);
}

extern "C" void* brawl3ds_cp_begin_call(void (*fn)(void*), uint32_t bytes)
{
    aurora::gx::fifo::drain();  // the commands before the call go first
    const uint32_t size = CmdSize(sizeof(CmdCpCall) + bytes);
    auto* h = reinterpret_cast<CmdHeader*>(Reserve(size));
    h->type = kCmdCpCall;
    h->pad = 0;
    h->size = size;
    auto* c = reinterpret_cast<CmdCpCall*>(h + 1);
    c->fn = fn;
    c->bytes = bytes;
    return c + 1;
}

extern "C" void brawl3ds_cp_end_call(void* payload)
{
    auto* h = reinterpret_cast<CmdHeader*>(static_cast<uint8_t*>(payload) - sizeof(CmdCpCall)) - 1;
    Commit(h->size);
}

extern "C" void brawl3ds_cp_inline_begin(const char* site)
{
    if (!g_cpOnWorker || OnWorker()) return;
    if (g_cpInlineDepth == 0) {
        g_syncSite = site ? site : "(unnamed)";
        Sync();
        g_syncSite = nullptr;
    }
    ++g_cpInlineDepth;
}

extern "C" void brawl3ds_cp_inline_end(void)
{
    if (!g_cpOnWorker || OnWorker() || g_cpInlineDepth == 0) return;
    --g_cpInlineDepth;
}

// For headless.log's periodic frame lines (main_3ds.cpp): the worker's time in the command processor and the
// game thread's catch-up waits since the last call.
extern "C" void Brawl3dsPicaTakeCpStats(double* cpMs, uint32_t* syncs, double* syncMs)
{
    *cpMs = static_cast<double>(g_winCpTicks) * 1000.0 / SYSCLOCK_ARM11;
    *syncs = g_winCpSyncs;
    *syncMs = static_cast<double>(g_winCpSyncTicks) * 1000.0 / SYSCLOCK_ARM11;
    g_winCpTicks = g_winCpSyncTicks = 0;
    g_winCpSyncs = 0;
}

extern "C" bool Brawl3dsPicaActive() { return g_ready; }

// For headless.log's periodic frame lines: the worker's busy time, the game thread's frame-end wait (ms, summed)
// and the wake-ups since the last call. Called between frames.
extern "C" void Brawl3dsPicaTakeWindowStats(double* busyMs, double* waitMs, uint32_t* wakeups)
{
    *busyMs = static_cast<double>(g_winBusyTicks) * 1000.0 / SYSCLOCK_ARM11;
    *waitMs = static_cast<double>(g_winDrainTicks) * 1000.0 / SYSCLOCK_ARM11;
    *wakeups = g_winWakeSignals;
    g_winBusyTicks = g_winDrainTicks = 0;
    g_winWakeSignals = 0;
}
extern "C" uint32_t Brawl3dsPicaTakeDropped()
{
    const uint32_t n = g_droppedDraws + g_recDropped;
    g_droppedDraws = g_recDropped = 0;
    return n;
}
