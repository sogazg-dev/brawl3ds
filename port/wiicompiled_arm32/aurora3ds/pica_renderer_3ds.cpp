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

#include "pica_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" const unsigned char brawl3ds_gx_m4_shbin[];
extern "C" const unsigned int brawl3ds_gx_m4_shbin_size;

// What the renderer is doing, for main_3ds.cpp's stall report (heartbeat thread).
extern "C" const char* volatile g_brawl3dsRenderPhase = "not started";

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

bool g_ready = false;
bool g_failed = false;
} // namespace
unsigned brawl3ds_pica::g_debugFlags = 0;
extern "C" void Brawl3dsPicaSetDebugFlags(unsigned flags) { brawl3ds_pica::g_debugFlags = flags; }
namespace {
bool g_frameOpen = false;
C3D_RenderTarget* g_target = nullptr;
DVLB_s* g_dvlb = nullptr;
shaderProgram_s g_program;
int g_projLoc = -1, g_mtxLoc = -1, g_texMtxLoc = -1, g_texSrcLoc = -1;
int g_ambLoc = -1, g_lselLoc = -1, g_lightsLoc = -1, g_lightLoopLoc = -1, g_litLoc = -1;
int g_fogpLoc = -1, g_fogvLoc = -1, g_twosetLoc = -1;
uint8_t* g_vtxBytes = nullptr;  // the frame's vertices, each batch in its layout (PicaVertex or PicaVertex2)
uint16_t* g_indices = nullptr;
uint32_t g_vtxBytesUsed = 0, g_idxUsed = 0;
// The open batch: where its vertices start in the vertex buffer, its first index, its vertex count and layout
// (0: PicaVertex, 1: PicaVertex2), and its material.
uint32_t g_batchVtxByte = 0, g_batchIdx = 0, g_batchVerts = 0;
int g_batchLayout = 0;
int g_curLayout = -1;  // the attribute layout citro3d has
bool g_batchOpen = false;
MaterialPlan g_plan;
uint32_t g_droppedDraws = 0;
uint32_t g_statClears = 0, g_statClearsSkipped = 0, g_statPartViewport = 0;
bool g_skipFrame = false;       // OpenFrame gave up on a busy GPU: no drawing until the frame ends
uint32_t g_gpuStalls = 0;
bool g_debugFrame = false;  // Brawl3dsPicaDebugNextFrame: log the next frame's first batches
uint32_t g_debugBatches = 0;
uint32_t g_debugSerial = 0;  // batches of the debug frame so far
const uint8_t* g_lastTopFramebuffer = nullptr;  // for the screenshots
// citro3d's command buffer: a match frame has ~555 batches, each uploading ~46 uniform vectors, its combiners,
// textures and fragment state, plus its draw (~1 KiB a batch).
constexpr size_t kCommandBufferSize = 768 * 1024;

bool Init()
{
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeded
    auto fail = [](const char* what) {
        std::fprintf(stderr, "[pica] renderer NOT started: %s failed (%u KiB linear, %u KiB VRAM free)\n", what,
                     static_cast<unsigned>(linearSpaceFree() >> 10), static_cast<unsigned>(vramSpaceFree() >> 10));
        return false;
    };
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
    g_dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(brawl3ds_gx_m4_shbin)),
                            brawl3ds_gx_m4_shbin_size);
    if (!g_dvlb) return fail("the shader");
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
        g_twosetLoc < 0)
        return fail("the shader's uniforms");
    g_vtxBytes = static_cast<uint8_t*>(linearAlloc(kVertexBytes));
    if (!g_vtxBytes) return fail("the vertex buffer");
    g_indices = static_cast<uint16_t*>(linearAlloc(sizeof(uint16_t) * kMaxIndices));
    if (!g_indices) return fail("the index buffer");
    std::fprintf(stderr,
                 "[pica] renderer ready: %u vertices, %u indices per frame (%u KiB linear), uniforms %d %d %d %d; "
                 "%u KiB linear, %u KiB VRAM free\n",
                 kMaxVertices, kMaxIndices,
                 static_cast<unsigned>((kVertexBytes + 2 * kMaxIndices) >> 10), g_projLoc,
                 g_mtxLoc, g_texMtxLoc, g_texSrcLoc, static_cast<unsigned>(linearSpaceFree() >> 10),
                 static_cast<unsigned>(vramSpaceFree() >> 10));
    g_failed = false;
    g_ready = true;
    return true;
}

// The vertex attribute layout: 0 = PicaVertex (four inputs), 1 = PicaVertex2 (set B as a fifth). No fixed attribute:
// the console's GPU hung on the first draws (2 s every frame) in a build that gave layout 0 set B as a fixed 0 and
// whose shader wrote texcoord2 twice (one of the two; per-vertex fog was cleared on the console later). The shader
// reads set B only under its twoset flag. citro3d re-sends the attribute state whenever it is fetched for writing,
// so it changes only when the layout does.
inline void SetBoolUniform(int loc, bool on)
{
    if (C3D_BoolUnifs[GPU_VERTEX_SHADER] & BIT(loc - 0x68) ? !on : on) C3D_BoolUnifSet(GPU_VERTEX_SHADER, loc, on);
}

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

bool OpenFrame()
{
    if (g_frameOpen) return true;
    if (g_skipFrame || !Init()) return false;
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
            return false;
        }
        svcSleepThread(500000);  // 0.5 ms
    }
    const auto& cc = g_gxState.clearColor;
    auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const uint32_t clear = byte(cc[0]) << 24 | byte(cc[1]) << 16 | byte(cc[2]) << 8 | 0xFF;
    C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, clear, 0x00FFFFFF);
    C3D_FrameDrawOn(g_target);
    ApplyFixedState();
    TexturesBeginFrame();
    g_vtxBytesUsed = g_idxUsed = 0;
    g_batchOpen = false;
    g_frameOpen = true;
    return true;
}

void FlushBatch()
{
    if (!g_batchOpen) return;
    g_batchOpen = false;
    const uint32_t count = g_idxUsed - g_batchIdx;
    if (count == 0 || g_plan.skip) return;
    SetLayout(g_batchLayout);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    if (g_batchLayout)
        BufInfo_Add(buf, g_vtxBytes + g_batchVtxByte, sizeof(PicaVertex2), 5, 0x43210);
    else
        BufInfo_Add(buf, g_vtxBytes + g_batchVtxByte, sizeof(PicaVertex), 4, 0x3210);
    C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, g_indices + g_batchIdx);
}

inline C3D_FVec Row(const aurora::Vec4<float>& r) { return FVec4_New(r[0], r[1], r[2], r[3]); }

// citro3d uploads every uniform vector marked dirty before the next draw; ~555 batches a frame rewriting all 46
// overflowed the command buffer (svcBreak PANIC at the match start). Only vectors that changed are marked.
inline void SetVec(int id, const C3D_FVec& v)
{
    C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][id];
    if (std::memcmp(&cur, &v, sizeof(v)) != 0) {
        cur = v;
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][id] = true;
    }
}

// Uniforms for a batch: the GX projection, position matrices and the texture-coordinate generators of its units.
void UploadUniforms(const float scaleS[kMaxUnits], const float scaleT[kMaxUnits])
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
    // matrix, rows s, t and q (q = 1 for a 2x4 matrix), scaled to the padded PICA texture.
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
            for (int c = 0; c < 4; ++c) m0.c[c] *= scaleS[u], m1.c[c] *= scaleT[u];
        }
        SetVec(g_texSrcLoc + u, s);
        SetVec(g_texMtxLoc + u * 3 + 0, m0);
        SetVec(g_texMtxLoc + u * 3 + 1, m1);
        SetVec(g_texMtxLoc + u * 3 + 2, m2);
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

// How to read the vertices of one draw, worked out once per draw from the GX vertex descriptor and format.
struct AttrPlan {
    const uint8_t* base;  // indexed: the array
    uint32_t stride;
    uint32_t offset;      // where the value, or its index, starts in the GX vertex
    uint32_t index;       // 0 = direct, 1 = 8-bit index, 2 = 16-bit index (in the stream's byte order)
    bool be;              // the values' byte order
    GXCompType type;
    float scale;          // fixed-point components: 1 / 2^frac
    bool two;             // texture coordinates: s and t (else s only); position: x y z (else x y)
};
struct VertexPlan {
    uint32_t size;        // bytes per GX vertex
    int pnMtxOffset;      // -1 = none
    bool hasPos, hasClr;
    AttrPlan pos, clr;
    bool hasUv, hasUv2;
    AttrPlan uv, uv2;
    bool hasNrm;
    AttrPlan nrm;
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
        a.scale = af.type == GX_F32 ? 1.0f : 1.0f / static_cast<float>(1u << af.frac);
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

inline const uint8_t* AttrValues(const AttrPlan& a, const uint8_t* vtx, bool streamBe)
{
    const uint8_t* p = vtx + a.offset;
    if (a.index == 0) return p;
    return a.base + (a.index == 2 ? Load16(p, streamBe) : *p) * a.stride;
}

// The positions of one draw, in a loop specialized for their component type.
template <GXCompType Type, class V>
void DecodePositions(const VertexPlan& plan, const uint8_t* src, bool streamBe, uint32_t count, V* out)
{
    const AttrPlan& a = plan.pos;
    constexpr uint32_t step = Type == GX_F32 ? 4 : (Type == GX_U16 || Type == GX_S16) ? 2 : 1;
    const float scale = a.scale;
    const bool be = a.be;
    for (uint32_t n = 0; n < count; ++n, src += plan.size) {
        const uint8_t* values = AttrValues(a, src, streamBe);
        V& v = out[n];
        v.x = LoadComponent(values, Type, scale, be);
        v.y = LoadComponent(values + step, Type, scale, be);
        v.z = a.two ? LoadComponent(values + 2 * step, Type, scale, be) : 0.0f;
    }
}

// One texture-coordinate set of a draw into `field` (uv or uv2) of each vertex.
template <class V>
void DecodeUvs(const AttrPlan& a, const uint8_t* src, bool streamBe, uint32_t count, uint32_t size, V* out,
               int16_t (V::*field)[2])
{
    const uint32_t step = a.type == GX_F32 ? 4 : (a.type == GX_U16 || a.type == GX_S16) ? 2 : 1;
    for (uint32_t n = 0; n < count; ++n, src += size) {
        const uint8_t* values = AttrValues(a, src, streamBe);
        const float u = LoadComponent(values, a.type, a.scale, a.be) * kUvScale;
        const float v = a.two ? LoadComponent(values + step, a.type, a.scale, a.be) * kUvScale : 0.0f;
        (out[n].*field)[0] = static_cast<int16_t>(std::clamp(u, -32768.0f, 32767.0f));
        (out[n].*field)[1] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }
}

// The GX channel COLOR0A0 for this batch: its material color goes into the vertices (from the vertex color or the
// material register); its lighting, when enabled, into the shader's uniforms (UploadLighting).
struct ChannelPlan {
    bool rgbFromVertex, alphaFromVertex;
    uint8_t rgb[3], alpha;  // material color (register source)
    bool lit;               // the shader lights the batch: the vertices need normals
};

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
    if (C3D_BoolUnifs[GPU_VERTEX_SHADER] & BIT(g_litLoc - 0x68) ? !ch.lit : ch.lit)
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_litLoc, ch.lit);
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
    const C3D_IVec loop = IVec_Pack(n - 1, 0, 5, 0);
    if (C3D_IVUnif[GPU_VERTEX_SHADER][g_lightLoopLoc - 0x60] != loop)
        C3D_IVUnifSet(GPU_VERTEX_SHADER, g_lightLoopLoc, n - 1, 0, 5, 0);
}

ChannelPlan g_channel;

// Per-vertex fog (gx_m4.v.pica): the GX fog parameters as uniforms, the ramp on unit 2.
void UploadVertexFog()
{
    const bool on = g_plan.vertexFog && BindFogRamp(2);
    if (on) {
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
    if (C3D_BoolUnifs[GPU_VERTEX_SHADER] & BIT(g_fogvLoc - 0x68) ? !on : on)
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_fogvLoc, on);
}

inline int8_t NormalByte(float v) { return static_cast<int8_t>(std::clamp(v * 127.0f, -127.0f, 127.0f)); }

// The normals of one draw, x 127 (their GX fixed-point scale applied).
template <class V>
void DecodeNormals(const AttrPlan& a, const uint8_t* src, bool streamBe, uint32_t count, uint32_t size, V* out)
{
    const uint32_t step = a.type == GX_F32 ? 4 : (a.type == GX_U16 || a.type == GX_S16) ? 2 : 1;
    for (uint32_t n = 0; n < count; ++n, src += size) {
        const uint8_t* values = AttrValues(a, src, streamBe);
        out[n].n[0] = NormalByte(LoadComponent(values, a.type, a.scale, a.be));
        out[n].n[1] = NormalByte(LoadComponent(values + step, a.type, a.scale, a.be));
        out[n].n[2] = NormalByte(LoadComponent(values + 2 * step, a.type, a.scale, a.be));
    }
}

// Decode `count` GX vertices of format `fmt` into the frame's vertex buffer as V; false when it is full.
template <class V>
bool DecodeVerticesAs(GXVtxFmt fmt, const uint8_t* src, uint32_t count, bool be)
{
    if (g_vtxBytesUsed + count * sizeof(V) > kVertexBytes) return false;
    VertexPlan plan;
    BuildPlan(fmt, be, plan);
    V* out = reinterpret_cast<V*>(g_vtxBytes + g_vtxBytesUsed);

    // Channel color.
    const ChannelPlan& ch = g_channel;
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
    if (ch.lit || g_plan.needsNormals) {
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
        const int8_t row = static_cast<int8_t>(g_gxState.currentPnMtx * 3);
        for (uint32_t n = 0; n < count; ++n) out[n].row = row;
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
        case GX_U8: DecodePositions<GX_U8>(plan, src, be, count, out); break;
        case GX_S8: DecodePositions<GX_S8>(plan, src, be, count, out); break;
        case GX_U16: DecodePositions<GX_U16>(plan, src, be, count, out); break;
        case GX_S16: DecodePositions<GX_S16>(plan, src, be, count, out); break;
        default: DecodePositions<GX_F32>(plan, src, be, count, out); break;
        }
    } else {
        for (uint32_t n = 0; n < count; ++n) out[n].x = out[n].y = out[n].z = 0.0f;
    }
    g_vtxBytesUsed += count * sizeof(V);
    g_batchVerts += count;
    return true;
}

// In the open batch's layout.
bool DecodeVertices(GXVtxFmt fmt, const uint8_t* src, uint32_t count, bool be)
{
    return g_batchLayout ? DecodeVerticesAs<PicaVertex2>(fmt, src, count, be)
                         : DecodeVerticesAs<PicaVertex>(fmt, src, count, be);
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
    if (g_vtxBytesUsed + 8 + vtxCount * sizeof(PicaVertex2) > kVertexBytes || g_idxUsed + indexCount > kMaxIndices) {
        ++g_droppedDraws;
        return;
    }
    // The batch's material: combiners and fragment state, its textures, then the uniforms (which need the
    // textures' padding scales) and the channel color.
    g_brawl3dsRenderPhase = "material and textures";
    ApplyMaterial(g_plan);
    float scaleS[kMaxUnits] = {1, 1, 1}, scaleT[kMaxUnits] = {1, 1, 1};
    int whiteUnits = 0;
    for (int u = 0; u < g_plan.unitCount; ++u)
        if (!BindTexture(u, g_plan.units[u].texMap, scaleS[u], scaleT[u])) ++whiteUnits;
    UploadUniforms(scaleS, scaleT);
    BuildChannel(g_channel);
    UploadVertexFog();

    g_brawl3dsRenderPhase = "vertices";
    g_batchLayout = g_plan.uvAttr[1] >= 0 ? 1 : 0;
    SetBoolUniform(g_twosetLoc, g_batchLayout == 1);
    // Every batch starts 8-byte aligned, as it always did with 24-byte vertices (28-byte ones break that).
    g_vtxBytesUsed = (g_vtxBytesUsed + 7) & ~7u;
    g_batchVtxByte = g_vtxBytesUsed;
    g_batchIdx = g_idxUsed;
    g_batchVerts = 0;
    if (!DecodeVertices(fmt, vertices, vtxCount, bigEndian)) {
        ++g_droppedDraws;
        return;
    }
    std::memcpy(g_indices + g_idxUsed, indices, indexCount * sizeof(uint16_t));
    g_idxUsed += indexCount;
    g_batchOpen = true;

    // One frame's debug dump (Brawl3dsPicaDebugNextFrame): the first batches, then the ones whose material may be
    // drawn wrong (approximated, a texture missing, three or more TEV stages, two texture-coordinate sets, coordinates
    // from normals), with their TEV stages. Kept to 40 batches: each line goes to the SD card, and a 60-batch dump
    // with every fogged batch made the title's dump frame take 6.4 s on the console.
    ++g_debugSerial;
    const bool suspect = g_plan.approximated || whiteUnits > 0 || g_gxState.numTevStages >= 3 ||
                         g_plan.uvAttr[1] >= 0 || g_plan.needsNormals;
    if (g_debugFrame && (g_debugSerial <= 6 || (suspect && g_debugBatches < 40))) {
        ++g_debugBatches;
        std::fprintf(stderr, "[pica] -- batch #%u of the frame: %s%s%d PICA stages, %d white units\n", g_debugSerial,
                     g_plan.approximated ? "approximated, " : "", suspect ? "suspect, " : "", g_plan.picaStages,
                     whiteUnits);
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
        const PicaVertex& v = *reinterpret_cast<const PicaVertex*>(g_vtxBytes + g_batchVtxByte);
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
            if (g_batchLayout) {
                const auto& v2 = *reinterpret_cast<const PicaVertex2*>(&v);
                std::fprintf(stderr, ", uv2 (%.3f %.3f)", v2.uv2[0] / kUvScale, v2.uv2[1] / kUvScale);
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr,
                     "[pica] batch %u: fmt %d, %u verts, %u indices, tev %u stages, %d units (", g_debugBatches,
                     static_cast<int>(fmt), vtxCount, indexCount, g_gxState.numTevStages, g_plan.unitCount);
        for (int u = 0; u < g_plan.unitCount; ++u) {
            const auto& t = g_gxState.loadedTextures[g_plan.units[u].texMap];
            const auto& tcg = g_gxState.tcgs[g_plan.units[u].texCoord & 7];
            std::fprintf(stderr, "%smap %u coord %u (src %d mtx %d type %d set %u) %ux%u fmt %u", u ? ", " : "",
                         g_plan.units[u].texMap, g_plan.units[u].texCoord, static_cast<int>(tcg.src),
                         static_cast<int>(tcg.mtx), static_cast<int>(tcg.type), g_plan.unitSet[u], t.width(),
                         t.height(), t.format());
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
}

extern "C" void Brawl3dsPicaDebugNextFrame()
{
    g_debugFrame = true;
    g_debugBatches = 0;
    g_debugSerial = 0;
}

// An unchanged-state draw appended to the open batch; false when there is none (the caller draws it in full).
bool Brawl3dsPicaAppend(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint32_t vtxCount, bool bigEndian)
{
    if (!g_frameOpen || !g_batchOpen) return false;
    const uint32_t first = g_batchVerts;
    const uint32_t stride = g_batchLayout ? sizeof(PicaVertex2) : sizeof(PicaVertex);
    if (first + vtxCount > 0xFFFF || g_vtxBytesUsed + vtxCount * stride > kVertexBytes ||
        g_idxUsed + vtxCount * 3 > kMaxIndices) {
        ++g_droppedDraws;
        return true;  // dropped, but handled
    }
    if (!DecodeVertices(fmt, vertices, vtxCount, bigEndian)) return true;
    AppendIndices(prim, first, vtxCount);
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
    if (!OpenFrame()) return;
    FlushBatch();
    g_vtxBytesUsed = (g_vtxBytesUsed + 7) & ~7u;
    if (g_vtxBytesUsed + 4 * sizeof(PicaVertex) > kVertexBytes || g_idxUsed + 6 > kMaxIndices) {
        ++g_droppedDraws;
        return;
    }
    ++g_statClears;
    // The quad in GX clip space (y up), at the clear depth: depth = z + 1 with w = 1, kept off the far plane.
    const float x0 = 2.0f * static_cast<float>(x) / kEfbWidth - 1.0f;
    const float x1 = 2.0f * static_cast<float>(x + w) / kEfbWidth - 1.0f;
    const float y0 = 1.0f - 2.0f * static_cast<float>(y) / kEfbHeight;
    const float y1 = 1.0f - 2.0f * static_cast<float>(y + h) / kEfbHeight;
    const float z = std::min(static_cast<float>(g_gxState.clearDepth & 0xFFFFFF) / 16777215.0f - 1.0f, -1.0f / 65536.0f);
    auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const auto& cc = g_gxState.clearColor;
    PicaVertex* v = reinterpret_cast<PicaVertex*>(g_vtxBytes + g_vtxBytesUsed);
    const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i) {
        v[i] = {};
        v[i].x = corners[i][0], v[i].y = corners[i][1], v[i].z = z;
        v[i].r = byte(cc[0]), v[i].g = byte(cc[1]), v[i].b = byte(cc[2]), v[i].a = byte(cc[3]);
    }
    uint16_t* idx = g_indices + g_idxUsed;
    idx[0] = 0, idx[1] = 1, idx[2] = 2, idx[3] = 0, idx[4] = 2, idx[5] = 3;
    // Uniforms: position matrix 0 = identity, the projection only turns the screen (the next batch's
    // UploadUniforms sets its own again), no lighting.
    SetVec(g_projLoc + 0, FVec4_New(0, 1, 0, 0));
    SetVec(g_projLoc + 1, FVec4_New(-1, 0, 0, 0));
    SetVec(g_projLoc + 2, FVec4_New(0, 0, 1, 0));
    SetVec(g_projLoc + 3, FVec4_New(0, 0, 0, 1));
    SetVec(g_mtxLoc + 0, FVec4_New(1, 0, 0, 0));
    SetVec(g_mtxLoc + 1, FVec4_New(0, 1, 0, 0));
    SetVec(g_mtxLoc + 2, FVec4_New(0, 0, 1, 0));
    if (C3D_BoolUnifs[GPU_VERTEX_SHADER] & BIT(g_litLoc - 0x68)) C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_litLoc, false);
    if (C3D_BoolUnifs[GPU_VERTEX_SHADER] & BIT(g_fogvLoc - 0x68)) C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_fogvLoc, false);
    SetBoolUniform(g_twosetLoc, false);
    ApplyClearState(color, alpha, depth);
    SetLayout(0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, v, sizeof(PicaVertex), 4, 0x3210);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, idx);
    g_vtxBytesUsed += 4 * sizeof(PicaVertex);
    g_idxUsed += 6;
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

// Material and texture statistics for the match summary (headless.log).
extern "C" void Brawl3dsPicaWriteStats(std::FILE* f, unsigned frames)
{
    if (f) {
        std::fprintf(f, "GPU stalls (frames not drawn after 2 s of waiting): %u\n", g_gpuStalls);
        std::fprintf(f,
                     "lighting: %u of %u batches lit (%.1f lights each), %u lit approximately, %u read channel "
                     "COLOR1A1 (drawn as COLOR0A0)\n",
                     g_statLit, g_statBatches, g_statLit ? static_cast<double>(g_statLights) / g_statLit : 0.0,
                     g_statLitApprox, g_statColor1);
        std::fprintf(f,
                     "viewport and copies: %u batches in a part of the screen; %u EFB copy clears drawn, %u skipped "
                     "(whole screen)\n",
                     g_statPartViewport, g_statClears, g_statClearsSkipped);
    }
    g_statBatches = g_statLit = g_statLitApprox = g_statLights = g_statColor1 = 0;
    g_statPartViewport = g_statClears = g_statClearsSkipped = 0;
    WriteTevStats(f, frames);
    WriteTextureStats(f, frames);
}

void Brawl3dsPicaEndFrame()
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
    g_debugFrame = false;  // a debug dump covers one frame
}

extern "C" bool Brawl3dsPicaActive() { return g_ready; }
extern "C" uint32_t Brawl3dsPicaTakeDropped()
{
    const uint32_t n = g_droppedDraws;
    g_droppedDraws = 0;
    return n;
}
