// PICA200 draw path for Aurora's GX front end on the 3DS (renderer milestone 2, docs/RENDERER_3DS.md): GX draws as
// untextured geometry with vertex colors on the top screen.
//
// A batch is one full draw from Aurora (backend_3ds.cpp's push_draw_command: a GX state change, with its
// triangle-list indices) plus the unchanged-state draws appended to it (brawl3ds_append_draw). Each GX vertex is
// decoded on the CPU from the current vertex format (direct or indexed attributes, either byte order) into
// position / color / matrix row; the vertex shader (gx_m2.v.pica) applies the vertex's GX position matrix and the
// GX projection, uploaded as uniforms when the batch starts. Batches go to citro3d as indexed triangle lists into
// per-frame linear-memory buffers; the frame begins at its first draw and ends in Brawl3dsPicaEndFrame.
// Not yet: textures, TEV, lighting, blending, depth, culling, viewport/scissor (whole screen), EFB copies.

#include "gx/gx.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <citro3d.h>

extern "C" const unsigned char brawl3ds_gx_m2_shbin[];
extern "C" const unsigned int brawl3ds_gx_m2_shbin_size;
extern "C" const unsigned char brawl3ds_selftest_shbin[];
extern "C" const unsigned int brawl3ds_selftest_shbin_size;

namespace {
using namespace aurora::gx;

struct PicaVertex {
    float x, y, z;
    uint8_t r, g, b, a;
    uint8_t row;  // GX matrix row of the position matrix (0, 3, ..., 27)
    uint8_t pad[3];
};
static_assert(sizeof(PicaVertex) == 20);

constexpr uint32_t kMaxVertices = 80 * 1024;   // per frame; Brawl's match draws ~69,000
constexpr uint32_t kMaxIndices = 160 * 1024;
constexpr uint32_t kDisplayFlags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

bool g_ready = false;
bool g_failed = false;
bool g_frameOpen = false;
C3D_RenderTarget* g_target = nullptr;
DVLB_s* g_dvlb = nullptr;
shaderProgram_s g_program;
int g_projLoc = -1, g_mtxLoc = -1;
PicaVertex* g_vertices = nullptr;
uint16_t* g_indices = nullptr;
uint32_t g_vtxUsed = 0, g_idxUsed = 0;
// The open batch: its first vertex and index in the frame buffers.
uint32_t g_batchVtx = 0, g_batchIdx = 0;
// Milestone 2 has no textures or TEV yet: a vertex without its own color gets its batch's false color, so the
// shapes stay distinguishable in the screenshots (everything untextured was white).
uint32_t g_batchSerial = 0;
uint8_t g_falseColor[3] = {255, 255, 255};
bool g_batchOpen = false;
uint32_t g_droppedDraws = 0;
const uint8_t* g_lastTopFramebuffer = nullptr;  // for Brawl3dsPicaScreenshot
bool g_debugFrame = false;  // Brawl3dsPicaDebugNextFrame: log the next frame's first batches
uint32_t g_debugBatches = 0;
// citro3d's command buffer: a match frame has ~555 batches, each uploading 34 uniform vectors plus its draw, over
// the 256 KiB default (svcBreak PANIC in Azahar at the match start).
constexpr size_t kCommandBufferSize = 640 * 1024;

void ApplyFixedState();
void SelfTest();

bool Init()
{
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeded
    if (!C3D_Init(kCommandBufferSize)) return false;
    g_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!g_target) return false;
    C3D_RenderTargetSetOutput(g_target, GFX_TOP, GFX_LEFT, kDisplayFlags);
    g_dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(brawl3ds_gx_m2_shbin)),
                            brawl3ds_gx_m2_shbin_size);
    if (!g_dvlb) return false;
    shaderProgramInit(&g_program);
    shaderProgramSetVsh(&g_program, &g_dvlb->DVLE[0]);
    g_projLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "proj");
    g_mtxLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "posmtx");
    g_vertices = static_cast<PicaVertex*>(linearAlloc(sizeof(PicaVertex) * kMaxVertices));
    g_indices = static_cast<uint16_t*>(linearAlloc(sizeof(uint16_t) * kMaxIndices));
    if (!g_vertices || !g_indices) return false;
    std::fprintf(stderr, "[pica] renderer ready: %u vertices, %u indices per frame (%u KiB linear)\n", kMaxVertices,
                 kMaxIndices,
                 static_cast<unsigned>((sizeof(PicaVertex) * kMaxVertices + 2 * kMaxIndices) >> 10));
    g_failed = false;
    g_ready = true;
    SelfTest();
    return true;
}

// The fixed state of milestone 2; set at the start of every frame (citro3d keeps it, but other code may not).
void ApplyFixedState()
{
    C3D_BindProgram(&g_program);
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);          // position
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);  // color
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  // matrix row (+ padding)
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
}

bool OpenFrame()
{
    if (g_frameOpen) return true;
    if (!Init()) return false;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // waits for the previous frame: its buffers are reused below
    C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, 0x202020FF, 0);
    C3D_FrameDrawOn(g_target);
    ApplyFixedState();
    g_vtxUsed = g_idxUsed = 0;
    g_batchOpen = false;
    g_frameOpen = true;
    return true;
}

void FlushBatch()
{
    if (!g_batchOpen) return;
    g_batchOpen = false;
    const uint32_t count = g_idxUsed - g_batchIdx;
    if (count == 0) return;
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_vertices + g_batchVtx, sizeof(PicaVertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, g_indices + g_batchIdx);
}

// Uniforms for a batch: the GX projection and position matrices as they are now.
void UploadMatrices()
{
    // The GX projection with the top screen's quarter turn folded in: clip (x, y) -> (y, -x), i.e. rows 1 and -0.
    const auto& p = g_gxState.proj;
    C3D_FVec* proj = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_projLoc, 4);
    proj[0] = FVec4_New(p.m1[0], p.m1[1], p.m1[2], p.m1[3]);
    proj[1] = FVec4_New(-p.m0[0], -p.m0[1], -p.m0[2], -p.m0[3]);
    proj[2] = FVec4_New(p.m2[0], p.m2[1], p.m2[2], p.m2[3]);
    proj[3] = FVec4_New(p.m3[0], p.m3[1], p.m3[2], p.m3[3]);
    C3D_FVec* rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_mtxLoc, 30);
    for (u32 i = 0; i < 10 && i < MaxPnMtx; ++i) {
        const auto& m = g_gxState.pnMtx[i].pos;
        rows[i * 3 + 0] = FVec4_New(m.m0[0], m.m0[1], m.m0[2], m.m0[3]);
        rows[i * 3 + 1] = FVec4_New(m.m1[0], m.m1[1], m.m1[2], m.m1[3]);
        rows[i * 3 + 2] = FVec4_New(m.m2[0], m.m2[1], m.m2[2], m.m2[3]);
    }
}

// --- GX vertex decoding ---
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
inline void LoadColor(const uint8_t* p, GXCompType type, bool be, PicaVertex& v)
{
    switch (type) {
    case GX_RGB565: {
        const uint16_t c = Load16(p, be);
        v.r = static_cast<uint8_t>((c >> 11) * 255 / 31);
        v.g = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        v.b = static_cast<uint8_t>((c & 31) * 255 / 31);
        v.a = 255;
        break;
    }
    case GX_RGB8:
    case GX_RGBX8:
        v.r = p[0], v.g = p[1], v.b = p[2], v.a = 255;
        break;
    case GX_RGBA4: {
        const uint16_t c = Load16(p, be);
        v.r = static_cast<uint8_t>((c >> 12) * 17), v.g = static_cast<uint8_t>(((c >> 8) & 15) * 17);
        v.b = static_cast<uint8_t>(((c >> 4) & 15) * 17), v.a = static_cast<uint8_t>((c & 15) * 17);
        break;
    }
    case GX_RGBA6: {
        const uint32_t c = uint32_t{p[0]} << 16 | uint32_t{p[1]} << 8 | p[2];
        v.r = static_cast<uint8_t>(((c >> 18) & 63) * 255 / 63), v.g = static_cast<uint8_t>(((c >> 12) & 63) * 255 / 63);
        v.b = static_cast<uint8_t>(((c >> 6) & 63) * 255 / 63), v.a = static_cast<uint8_t>((c & 63) * 255 / 63);
        break;
    }
    default:  // GX_RGBA8
        v.r = p[0], v.g = p[1], v.b = p[2], v.a = p[3];
        break;
    }
}

// Bytes one attribute takes in the vertex stream.
inline uint32_t DirectSize(int attr, const VtxAttrFmt& f)
{
    const auto a = static_cast<GXAttr>(attr);
    return comp_type_size(a, f.type) * comp_cnt_count(a, f.cnt);
}

// How to read the vertices of one draw, worked out once per draw from the GX vertex descriptor and format. It used
// to be re-derived for every vertex: ~1.2 us a vertex on the console, ~80 ms of a match frame.
struct AttrPlan {
    const uint8_t* base;  // indexed: the array
    uint32_t stride;
    uint32_t offset;      // where the value, or its index, starts in the GX vertex
    uint32_t index;       // 0 = direct, 1 = 8-bit index, 2 = 16-bit index (in the stream's byte order)
    bool be;              // the values' byte order
    GXCompType type;
};
struct VertexPlan {
    uint32_t size;        // bytes per GX vertex
    int pnMtxOffset;      // -1 = none
    bool hasPos, hasClr, posXYZ;
    float posScale;
    AttrPlan pos, clr;
};

void BuildPlan(GXVtxFmt fmt, bool be, VertexPlan& plan)
{
    const auto& vf = g_gxState.vtxFmts[fmt];
    const auto& desc = g_gxState.vtxDesc;
    plan = {};
    plan.pnMtxOffset = -1;
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
        AttrPlan a{};
        a.offset = at;
        a.type = af.type;
        a.be = be;
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
            plan.hasPos = true;
            plan.pos = a;
            plan.posXYZ = af.cnt == GX_POS_XYZ;
            plan.posScale = af.type == GX_F32 ? 1.0f : 1.0f / static_cast<float>(1u << af.frac);
        } else if (attr == GX_VA_CLR0) {
            plan.hasClr = true;
            plan.clr = a;
        }
    }
    plan.size = at;
}

inline const uint8_t* AttrValues(const AttrPlan& a, const uint8_t* vtx, bool streamBe)
{
    const uint8_t* p = vtx + a.offset;
    if (a.index == 0) return p;
    return a.base + (a.index == 2 ? Load16(p, streamBe) : *p) * a.stride;
}

// The positions of one draw, in a loop specialized for their component type.
template <GXCompType Type>
void DecodePositions(const VertexPlan& plan, const uint8_t* src, bool streamBe, uint32_t count, PicaVertex* out)
{
    const AttrPlan& a = plan.pos;
    constexpr uint32_t step = Type == GX_F32 ? 4 : (Type == GX_U16 || Type == GX_S16) ? 2 : 1;
    const float scale = plan.posScale;
    const bool be = a.be;
    if (plan.posXYZ) {
        for (uint32_t n = 0; n < count; ++n, src += plan.size) {
            const uint8_t* values = AttrValues(a, src, streamBe);
            PicaVertex& v = out[n];
            v.x = LoadComponent(values, Type, scale, be);
            v.y = LoadComponent(values + step, Type, scale, be);
            v.z = LoadComponent(values + 2 * step, Type, scale, be);
        }
    } else {
        for (uint32_t n = 0; n < count; ++n, src += plan.size) {
            const uint8_t* values = AttrValues(a, src, streamBe);
            PicaVertex& v = out[n];
            v.x = LoadComponent(values, Type, scale, be);
            v.y = LoadComponent(values + step, Type, scale, be);
            v.z = 0.0f;
        }
    }
}

// Decode `count` GX vertices of format `fmt` into the frame's vertex buffer; false when it is full.
bool DecodeVertices(GXVtxFmt fmt, const uint8_t* src, uint32_t count, bool be)
{
    if (g_vtxUsed + count > kMaxVertices) return false;
    VertexPlan plan;
    BuildPlan(fmt, be, plan);
    PicaVertex* out = g_vertices + g_vtxUsed;

    // Color (or the batch's false color) and matrix row, then the positions.
    const uint8_t defaultRow = static_cast<uint8_t>(g_gxState.currentPnMtx * 3);
    const uint32_t fallbackColor = uint32_t{g_falseColor[0]} | uint32_t{g_falseColor[1]} << 8 |
                                   uint32_t{g_falseColor[2]} << 16 | 0xFF000000u;
    if (plan.hasClr) {
        const uint8_t* vtx = src;
        for (uint32_t n = 0; n < count; ++n, vtx += plan.size) LoadColor(AttrValues(plan.clr, vtx, be), plan.clr.type, plan.clr.be, out[n]);
    } else {
        for (uint32_t n = 0; n < count; ++n) std::memcpy(&out[n].r, &fallbackColor, 4);
    }
    if (plan.pnMtxOffset >= 0) {
        const uint8_t* vtx = src + plan.pnMtxOffset;
        for (uint32_t n = 0; n < count; ++n, vtx += plan.size) out[n].row = *vtx;
    } else {
        for (uint32_t n = 0; n < count; ++n) out[n].row = defaultRow;
    }
    if (plan.hasPos) {
        switch (plan.pos.type) {
        case GX_U8: DecodePositions<GX_U8>(plan, src, be, count, out); break;
        case GX_S8: DecodePositions<GX_S8>(plan, src, be, count, out); break;
        case GX_U16: DecodePositions<GX_U16>(plan, src, be, count, out); break;
        case GX_S16: DecodePositions<GX_S16>(plan, src, be, count, out); break;
        default: DecodePositions<GX_F32>(plan, src, be, count, out); break;
        }
    } else {
        for (uint32_t n = 0; n < count; ++n) out[n].x = out[n].y = out[n].z = 0.0f;
    }
    g_vtxUsed += count;
    return true;
}

// Triangle-list indices for `count` vertices of `prim`, relative to `base` (the batch's first vertex).
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
} // namespace

// A full draw from Aurora: `vertices` are its raw GX vertices, `indices` its triangle list (relative to them).
void Brawl3dsPicaDraw(GXVtxFmt fmt, const uint8_t* vertices, uint32_t vtxCount, const uint16_t* indices,
                      uint32_t indexCount, bool bigEndian)
{
    if (!OpenFrame()) return;
    FlushBatch();
    if (g_vtxUsed + vtxCount > kMaxVertices || g_idxUsed + indexCount > kMaxIndices) {
        ++g_droppedDraws;
        return;
    }
    UploadMatrices();
    {
        uint32_t h = ++g_batchSerial * 2654435761u;
        g_falseColor[0] = static_cast<uint8_t>(64 + ((h >> 8) & 191));
        g_falseColor[1] = static_cast<uint8_t>(64 + ((h >> 16) & 191));
        g_falseColor[2] = static_cast<uint8_t>(64 + ((h >> 24) & 191));
    }
    g_batchVtx = g_vtxUsed;
    g_batchIdx = g_idxUsed;
    if (!DecodeVertices(fmt, vertices, vtxCount, bigEndian)) {
        ++g_droppedDraws;
        return;
    }
    std::memcpy(g_indices + g_idxUsed, indices, indexCount * sizeof(uint16_t));
    g_idxUsed += indexCount;
    g_batchOpen = true;

    // One frame's debug dump (Brawl3dsPicaDebugNextFrame): the first batches, their first vertex and where the
    // shader's transform puts it, computed the same way on the CPU.
    if (g_debugFrame && g_debugBatches < 8) {
        ++g_debugBatches;
        const PicaVertex& v = g_vertices[g_batchVtx];
        const u32 mi = v.row / 3;
        const auto& m = g_gxState.pnMtx[mi < MaxPnMtx ? mi : 0].pos;
        const float view[4] = {m.m0[0] * v.x + m.m0[1] * v.y + m.m0[2] * v.z + m.m0[3],
                               m.m1[0] * v.x + m.m1[1] * v.y + m.m1[2] * v.z + m.m1[3],
                               m.m2[0] * v.x + m.m2[1] * v.y + m.m2[2] * v.z + m.m2[3], 1.0f};
        const auto& p = g_gxState.proj;
        auto dot = [&](const auto& r) { return r[0] * view[0] + r[1] * view[1] + r[2] * view[2] + r[3] * view[3]; };
        std::fprintf(stderr,
                     "[pica] batch %u: fmt %d, %u verts, %u indices (first %u %u %u), be %d, uniforms proj %d mtx %d\n"
                     "[pica]   vertex (%.3f %.3f %.3f) rgba %02x%02x%02x%02x row %u -> view (%.3f %.3f %.3f)"
                     " -> clip (%.3f %.3f %.3f w %.3f)\n",
                     g_debugBatches, static_cast<int>(fmt), vtxCount, indexCount, indexCount ? indices[0] : 0,
                     indexCount > 1 ? indices[1] : 0, indexCount > 2 ? indices[2] : 0, bigEndian ? 1 : 0, g_projLoc,
                     g_mtxLoc, v.x, v.y, v.z, v.r, v.g, v.b, v.a, v.row, view[0], view[1], view[2], dot(p.m0),
                     dot(p.m1), dot(p.m2), dot(p.m3));
    }
}

extern "C" void Brawl3dsPicaScreenshot(const char* path);

namespace {
// Bring-up self-test, once when the renderer starts: two frames with a red triangle in clip space through identity
// matrices, A drawn with C3D_DrawArrays, B with C3D_DrawElements, each saved to the SD card. Which ones show the
// triangle separates the draw call from the shader and state.
void SelfTest()
{
    const PicaVertex tri[3] = {{-0.5f, -0.5f, -0.5f, 255, 0, 0, 255, 0, {}},
                               {0.5f, -0.5f, -0.5f, 255, 0, 0, 255, 0, {}},
                               {0.0f, 0.5f, -0.5f, 255, 0, 0, 255, 0, {}}};
    std::memcpy(g_vertices, tri, sizeof(tri));
    g_indices[0] = 0, g_indices[1] = 1, g_indices[2] = 2;
    GSPGPU_FlushDataCache(g_vertices, sizeof(tri));
    GSPGPU_FlushDataCache(g_indices, 8);
    for (int variant = 0; variant < 3; ++variant) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, 0x202020FF, 0);
        C3D_FrameDrawOn(g_target);
        ApplyFixedState();
        if (variant == 2) {  // C: position from the buffer, color and matrix row as fixed attributes
            C3D_AttrInfo* attr = C3D_GetAttrInfo();
            AttrInfo_Init(attr);
            AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
            AttrInfo_AddFixed(attr, 1);
            AttrInfo_AddFixed(attr, 2);
            C3D_FixedAttribSet(1, 255.0f, 0.0f, 0.0f, 255.0f);
            C3D_FixedAttribSet(2, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        C3D_FVec* proj = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_projLoc, 4);
        proj[0] = FVec4_New(1, 0, 0, 0), proj[1] = FVec4_New(0, 1, 0, 0);
        proj[2] = FVec4_New(0, 0, 1, 0), proj[3] = FVec4_New(0, 0, 0, 1);
        C3D_FVec* rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_mtxLoc, 3);
        rows[0] = FVec4_New(1, 0, 0, 0), rows[1] = FVec4_New(0, 1, 0, 0), rows[2] = FVec4_New(0, 0, 1, 0);
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        if (variant == 2) {
            BufInfo_Add(buf, g_vertices, sizeof(PicaVertex), 1, 0x0);
        } else {
            BufInfo_Add(buf, g_vertices, sizeof(PicaVertex), 3, 0x210);
        }
        if (variant == 1) {
            C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, g_indices);
        } else {
            C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        }
        g_lastTopFramebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        C3D_FrameEnd(0);
        static const char* const kNames[] = {"sdmc:/3ds/brawl3ds/selftest_arrays.bmp",
                                             "sdmc:/3ds/brawl3ds/selftest_elements.bmp",
                                             "sdmc:/3ds/brawl3ds/selftest_fixedattr.bmp"};
        Brawl3dsPicaScreenshot(kNames[variant]);
    }
    // D: the smallest shader (selftest.v.pica: one 4x4 matrix), citro3d's tilted ortho projection, a green triangle
    // in 400x240 screen pixels, a fixed color: the same shape as devkitPro's simple_tri example.
    if (DVLB_s* dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(brawl3ds_selftest_shbin)),
                                      brawl3ds_selftest_shbin_size)) {
        shaderProgram_s program;
        shaderProgramInit(&program);
        shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
        const int mvpLoc = shaderInstanceGetUniformLocation(program.vertexShader, "mvp");
        const PicaVertex tri[3] = {{200.0f, 200.0f, 0.5f, 0, 0, 0, 0, 0, {}},
                                   {100.0f, 40.0f, 0.5f, 0, 0, 0, 0, 0, {}},
                                   {300.0f, 40.0f, 0.5f, 0, 0, 0, 0, 0, {}}};
        std::memcpy(g_vertices + 8, tri, sizeof(tri));
        GSPGPU_FlushDataCache(g_vertices + 8, sizeof(tri));
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, 0x202020FF, 0);
        C3D_FrameDrawOn(g_target);
        ApplyFixedState();
        C3D_BindProgram(&program);
        C3D_AttrInfo* attr = C3D_GetAttrInfo();
        AttrInfo_Init(attr);
        AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
        AttrInfo_AddFixed(attr, 1);
        C3D_FixedAttribSet(1, 0.0f, 1.0f, 0.0f, 1.0f);
        C3D_Mtx mvp;
        Mtx_OrthoTilt(&mvp, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mvpLoc, &mvp);
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, g_vertices + 8, sizeof(PicaVertex), 1, 0x0);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        g_lastTopFramebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        C3D_FrameEnd(0);
        Brawl3dsPicaScreenshot("sdmc:/3ds/brawl3ds/selftest_minimal.bmp");
        std::fprintf(stderr, "[pica] self-test D: mvp uniform at %d\n", mvpLoc);
    } else {
        std::fprintf(stderr, "[pica] self-test D: the self-test shader did not parse\n");
    }
    std::fprintf(stderr, "[pica] self-test frames written (selftest_*.bmp)\n");
}
} // namespace

extern "C" void Brawl3dsPicaDebugNextFrame()
{
    g_debugFrame = true;
    g_debugBatches = 0;
}

// An unchanged-state draw appended to the open batch; false when there is none (the caller draws it in full).
bool Brawl3dsPicaAppend(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint32_t vtxCount, bool bigEndian)
{
    if (!g_frameOpen || !g_batchOpen) return false;
    const uint32_t first = g_vtxUsed - g_batchVtx;
    if (first + vtxCount > 0xFFFF || g_vtxUsed + vtxCount > kMaxVertices || g_idxUsed + vtxCount * 3 > kMaxIndices) {
        ++g_droppedDraws;
        return true;  // dropped, but handled
    }
    if (!DecodeVertices(fmt, vertices, vtxCount, bigEndian)) return true;
    AppendIndices(prim, first, vtxCount);
    return true;
}

namespace {
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
    if (!rgb) return;
    GSPGPU_InvalidateDataCache(src, kShotBytes);
    std::memcpy(rgb, src, kShotBytes);
    Capture& c = g_captures[g_captureCount++];
    std::snprintf(c.path, sizeof(c.path), "%s", g_pendingCapturePath);
    c.rgb = rgb;
}
} // namespace

void WriteBmp(const char* path, const uint8_t* rgb);

extern "C" void Brawl3dsPicaCaptureNextFrame(const char* path)
{
    if (!g_ready || !g_lastTopFramebuffer) return;
    g_pendingCaptureSource = g_lastTopFramebuffer;
    std::snprintf(g_pendingCapturePath, sizeof(g_pendingCapturePath), "%s", path);
}

extern "C" void Brawl3dsPicaWriteCaptures()
{
    for (uint32_t i = 0; i < g_captureCount; ++i) {
        WriteBmp(g_captures[i].path, g_captures[i].rgb);
        std::free(g_captures[i].rgb);
    }
    g_captureCount = 0;
}

void Brawl3dsPicaEndFrame()
{
    if (!g_frameOpen) return;
    FlushBatch();
    GSPGPU_FlushDataCache(g_vertices, g_vtxUsed * sizeof(PicaVertex));
    GSPGPU_FlushDataCache(g_indices, g_idxUsed * sizeof(uint16_t));
    // The framebuffer citro3d copies this frame into (its back buffer until the copy is done and it swaps).
    // A capture asked for after the previous frame: that frame's copy is complete now (C3D_FrameBegin waited).
    TakePendingCapture();
    g_lastTopFramebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    C3D_FrameEnd(0);
    g_frameOpen = false;
    g_debugFrame = false;  // a debug dump covers one frame
}

extern "C" bool Brawl3dsPicaActive() { return g_ready; }
extern "C" uint32_t Brawl3dsPicaTakeDropped()
{
    const uint32_t n = g_droppedDraws;
    g_droppedDraws = 0;
    return n;
}

// The last rendered top-screen frame as a 400x240 24-bit BMP, written at once (the renderer's start-up self-test).
extern "C" void Brawl3dsPicaScreenshot(const char* path)
{
    if (!g_ready) return;
    constexpr uint32_t w = 240, h = 400;  // the framebuffer is portrait
    // citro3d runs a frame's GPU work asynchronously after C3D_FrameEnd, then copies the target into the top
    // screen's framebuffer (RGB8, 240x400, in ordinary memory). Let that finish, then read the copy the last frame
    // went to. (A GPU transfer of our own waited on a GSP event citro3d's queue can take first: one console run sat
    // in it for four minutes. The target itself is in VRAM, which the CPU cannot read.)
    if (!g_lastTopFramebuffer) return;
    svcSleepThread(200000000LL);
    GSPGPU_InvalidateDataCache(g_lastTopFramebuffer, w * h * 3);
    WriteBmp(path, g_lastTopFramebuffer);
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
