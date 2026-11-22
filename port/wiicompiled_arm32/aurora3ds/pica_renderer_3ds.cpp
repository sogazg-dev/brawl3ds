// PICA200 draw path for Aurora's GX front end on the 3DS (docs/RENDERER_3DS.md). Milestone 3: textures, TEV
// (pica_tev_3ds.cpp), alpha test, blending, depth and culling.
//
// A batch is one full draw from Aurora (backend_3ds.cpp's push_draw_command: a GX state change, with its
// triangle-list indices) plus the unchanged-state draws appended to it (brawl3ds_append_draw). At a batch's start
// the GX state is translated (ApplyMaterial), its textures bound (pica_texture_3ds.cpp) and the GX matrices
// uploaded. Each GX vertex is decoded on the CPU from the current vertex format into position, channel color,
// matrix row and up to two texture-coordinate sets; the vertex shader (gx_m3.v.pica) applies the GX position
// matrix, the projection and the texture-coordinate generators. Batches go to citro3d as indexed triangle lists
// into per-frame linear-memory buffers; the frame begins at its first draw and ends in Brawl3dsPicaEndFrame.
// Not yet: lighting (the channel color is approximated), fog, indirect textures, viewport/scissor, EFB copies.

#include "pica_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" const unsigned char brawl3ds_gx_m3_shbin[];
extern "C" const unsigned int brawl3ds_gx_m3_shbin_size;

// What the renderer is doing, for main_3ds.cpp's stall report (heartbeat thread).
extern "C" const char* volatile g_brawl3dsRenderPhase = "not started";

namespace {
using namespace aurora::gx;
using namespace brawl3ds_pica;

struct PicaVertex {
    float x, y, z;
    uint8_t r, g, b, a;   // the GX channel color (COLOR0A0)
    uint8_t row;          // GX matrix row of the position matrix (0, 3, ..., 27)
    uint8_t pad;
    int16_t uv[2];        // one texture-coordinate set in 1/512 steps (+-64; MaterialPlan::uvAttr[0])
    uint8_t pad2[2];
};
static_assert(sizeof(PicaVertex) == 24);
constexpr float kUvScale = 512.0f;  // gx_m3.v.pica multiplies by 1/512

// Linear memory is nearly all guest RAM: these buffers, citro3d's command buffer and the texture staging buffer
// have to fit in the ~3.5 MiB it leaves (main_3ds.cpp's kLinearHeap).
constexpr uint32_t kMaxVertices = 78 * 1024;   // per frame; Brawl's match draws up to ~74,400
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
int g_projLoc = -1, g_mtxLoc = -1, g_texMtxLoc = -1, g_texSrcLoc = -1;
PicaVertex* g_vertices = nullptr;
uint16_t* g_indices = nullptr;
uint32_t g_vtxUsed = 0, g_idxUsed = 0;
// The open batch: its first vertex and index in the frame buffers, and its material.
uint32_t g_batchVtx = 0, g_batchIdx = 0;
bool g_batchOpen = false;
MaterialPlan g_plan;
uint32_t g_droppedDraws = 0;
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
    g_dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(brawl3ds_gx_m3_shbin)),
                            brawl3ds_gx_m3_shbin_size);
    if (!g_dvlb) return fail("the shader");
    shaderProgramInit(&g_program);
    shaderProgramSetVsh(&g_program, &g_dvlb->DVLE[0]);
    g_projLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "proj");
    g_mtxLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "posmtx");
    g_texMtxLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "texmtx");
    g_texSrcLoc = shaderInstanceGetUniformLocation(g_program.vertexShader, "texsrc");
    g_vertices = static_cast<PicaVertex*>(linearAlloc(sizeof(PicaVertex) * kMaxVertices));
    if (!g_vertices) return fail("the vertex buffer");
    g_indices = static_cast<uint16_t*>(linearAlloc(sizeof(uint16_t) * kMaxIndices));
    if (!g_indices) return fail("the index buffer");
    std::fprintf(stderr,
                 "[pica] renderer ready: %u vertices, %u indices per frame (%u KiB linear), uniforms %d %d %d %d; "
                 "%u KiB linear, %u KiB VRAM free\n",
                 kMaxVertices, kMaxIndices,
                 static_cast<unsigned>((sizeof(PicaVertex) * kMaxVertices + 2 * kMaxIndices) >> 10), g_projLoc,
                 g_mtxLoc, g_texMtxLoc, g_texSrcLoc, static_cast<unsigned>(linearSpaceFree() >> 10),
                 static_cast<unsigned>(vramSpaceFree() >> 10));
    g_failed = false;
    g_ready = true;
    return true;
}

// The state that does not change between batches; set at the start of every frame.
void ApplyFixedState()
{
    C3D_BindProgram(&g_program);
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);          // position
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);  // channel color
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 2);  // matrix row (+ padding)
    AttrInfo_AddLoader(attr, 3, GPU_SHORT, 2);          // texture coordinates x 512 (the shader sees u, v, 0, 1)
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
    if (count == 0 || g_plan.skip) return;
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_vertices + g_batchVtx, sizeof(PicaVertex), 4, 0x3210);
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
    // The GX projection with the top screen's quarter turn folded in: clip (x, y) -> (y, -x), i.e. rows 1 and -0.
    const auto& p = g_gxState.proj;
    SetVec(g_projLoc + 0, FVec4_New(p.m1[0], p.m1[1], p.m1[2], p.m1[3]));
    SetVec(g_projLoc + 1, FVec4_New(-p.m0[0], -p.m0[1], -p.m0[2], -p.m0[3]));
    SetVec(g_projLoc + 2, FVec4_New(p.m2[0], p.m2[1], p.m2[2], p.m2[3]));
    SetVec(g_projLoc + 3, FVec4_New(p.m3[0], p.m3[1], p.m3[2], p.m3[3]));
    for (u32 i = 0; i < 10 && i < MaxPnMtx; ++i) {
        const auto& m = g_gxState.pnMtx[i].pos;
        SetVec(g_mtxLoc + i * 3 + 0, Row(m.m0));
        SetVec(g_mtxLoc + i * 3 + 1, Row(m.m1));
        SetVec(g_mtxLoc + i * 3 + 2, Row(m.m2));
    }

    // Per unit: the source vector's weights (uv set A, uv set B, object position) and the texture matrix, rows
    // s, t and q (q = 1 for a 2x4 matrix), scaled to the padded PICA texture.
    for (int u = 0; u < kMaxUnits; ++u) {
        C3D_FVec s = FVec4_New(1, 0, 0, 0), m0 = FVec4_New(1, 0, 0, 0), m1 = FVec4_New(0, 1, 0, 0),
                 m2 = FVec4_New(0, 0, 0, 1);
        if (u < g_plan.unitCount) {
            const auto& tcg = g_gxState.tcgs[g_plan.units[u].texCoord & 7];
            const int src = static_cast<int>(tcg.src);
            if (src == GX_TG_POS) {
                s = FVec4_New(0, 0, 1, 0);
            } else if (src >= GX_TG_TEX0 && src <= GX_TG_TEX0 + 7) {
                s = FVec4_New(1, 0, 0, 0);  // the vertex's one texture-coordinate set
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
    bool hasUv;
    AttrPlan uv;
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
        } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
            a.two = af.cnt == GX_TEX_ST;
            if (g_plan.uvAttr[0] == attr) plan.hasUv = true, plan.uv = a;
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
    const float scale = a.scale;
    const bool be = a.be;
    for (uint32_t n = 0; n < count; ++n, src += plan.size) {
        const uint8_t* values = AttrValues(a, src, streamBe);
        PicaVertex& v = out[n];
        v.x = LoadComponent(values, Type, scale, be);
        v.y = LoadComponent(values + step, Type, scale, be);
        v.z = a.two ? LoadComponent(values + 2 * step, Type, scale, be) : 0.0f;
    }
}

void DecodeUvs(const AttrPlan& a, const uint8_t* src, bool streamBe, uint32_t count, uint32_t size, PicaVertex* out)
{
    const uint32_t step = a.type == GX_F32 ? 4 : (a.type == GX_U16 || a.type == GX_S16) ? 2 : 1;
    for (uint32_t n = 0; n < count; ++n, src += size) {
        const uint8_t* values = AttrValues(a, src, streamBe);
        const float u = LoadComponent(values, a.type, a.scale, a.be) * kUvScale;
        const float v = a.two ? LoadComponent(values + step, a.type, a.scale, a.be) * kUvScale : 0.0f;
        out[n].uv[0] = static_cast<int16_t>(std::clamp(u, -32768.0f, 32767.0f));
        out[n].uv[1] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }
}

// The GX channel COLOR0A0 for this batch: material (or vertex) color times the lighting. Lighting is milestone 4:
// until then a lit channel is its ambient plus half of each enabled light's color, without direction.
struct ChannelPlan {
    bool rgbFromVertex, alphaFromVertex;
    uint32_t rgbMul[3], alphaMul;  // 0..256 fixed point
    uint8_t rgb[3], alpha;          // material color (register source)
};

void BuildChannel(ChannelPlan& ch)
{
    auto light = [](const ColorChannelConfig& cfg, const ColorChannelState& st, int c) {
        if (!cfg.lightingEnabled) return 1.0f;
        float v = st.ambColor[c];
        for (u32 i = 0; i < GX::MaxLights; ++i)
            if (st.lightMask.test(i)) v += 0.5f * g_gxState.lights[i].color[c];
        return std::clamp(v, 0.0f, 1.0f);
    };
    auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const auto& cc = g_gxState.colorChannelConfig[0];  // GX_COLOR0
    const auto& cs = g_gxState.colorChannelState[0];
    const auto& ac = g_gxState.colorChannelConfig[2];  // GX_ALPHA0
    const auto& as = g_gxState.colorChannelState[2];
    const bool none = g_gxState.numChans == 0;
    ch.rgbFromVertex = !none && cc.matSrc == GX_SRC_VTX;
    ch.alphaFromVertex = !none && ac.matSrc == GX_SRC_VTX;
    for (int c = 0; c < 3; ++c) {
        ch.rgb[c] = none ? 255 : byte(cs.matColor[c]);
        ch.rgbMul[c] = none ? 256 : static_cast<uint32_t>(light(cc, cs, c) * 256.0f);
    }
    ch.alpha = none ? 255 : byte(as.matColor[3]);
    ch.alphaMul = none ? 256 : static_cast<uint32_t>(light(ac, as, 3) * 256.0f);
}

ChannelPlan g_channel;

// Decode `count` GX vertices of format `fmt` into the frame's vertex buffer; false when it is full.
bool DecodeVertices(GXVtxFmt fmt, const uint8_t* src, uint32_t count, bool be)
{
    if (g_vtxUsed + count > kMaxVertices) return false;
    VertexPlan plan;
    BuildPlan(fmt, be, plan);
    PicaVertex* out = g_vertices + g_vtxUsed;

    // Channel color.
    const ChannelPlan& ch = g_channel;
    if ((ch.rgbFromVertex || ch.alphaFromVertex) && plan.hasClr) {
        const uint8_t* vtx = src;
        for (uint32_t n = 0; n < count; ++n, vtx += plan.size) {
            uint8_t c[4];
            LoadColor(AttrValues(plan.clr, vtx, be), plan.clr.type, plan.clr.be, c);
            PicaVertex& v = out[n];
            v.r = static_cast<uint8_t>(((ch.rgbFromVertex ? c[0] : ch.rgb[0]) * ch.rgbMul[0]) >> 8);
            v.g = static_cast<uint8_t>(((ch.rgbFromVertex ? c[1] : ch.rgb[1]) * ch.rgbMul[1]) >> 8);
            v.b = static_cast<uint8_t>(((ch.rgbFromVertex ? c[2] : ch.rgb[2]) * ch.rgbMul[2]) >> 8);
            v.a = static_cast<uint8_t>(((ch.alphaFromVertex ? c[3] : ch.alpha) * ch.alphaMul) >> 8);
        }
    } else {
        const uint8_t r = static_cast<uint8_t>((ch.rgb[0] * ch.rgbMul[0]) >> 8);
        const uint8_t g = static_cast<uint8_t>((ch.rgb[1] * ch.rgbMul[1]) >> 8);
        const uint8_t b = static_cast<uint8_t>((ch.rgb[2] * ch.rgbMul[2]) >> 8);
        const uint8_t a = static_cast<uint8_t>((ch.alpha * ch.alphaMul) >> 8);
        for (uint32_t n = 0; n < count; ++n) out[n].r = r, out[n].g = g, out[n].b = b, out[n].a = a;
    }
    // Matrix row.
    if (plan.pnMtxOffset >= 0) {
        const uint8_t* vtx = src + plan.pnMtxOffset;
        for (uint32_t n = 0; n < count; ++n, vtx += plan.size) out[n].row = *vtx;
    } else {
        const uint8_t row = static_cast<uint8_t>(g_gxState.currentPnMtx * 3);
        for (uint32_t n = 0; n < count; ++n) out[n].row = row;
    }
    // Texture coordinates.
    if (plan.hasUv) {
        DecodeUvs(plan.uv, src, be, count, plan.size, out);
    } else {
        for (uint32_t n = 0; n < count; ++n) out[n].uv[0] = out[n].uv[1] = 0;
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

    g_brawl3dsRenderPhase = "vertices";
    g_batchVtx = g_vtxUsed;
    g_batchIdx = g_idxUsed;
    if (!DecodeVertices(fmt, vertices, vtxCount, bigEndian)) {
        ++g_droppedDraws;
        return;
    }
    std::memcpy(g_indices + g_idxUsed, indices, indexCount * sizeof(uint16_t));
    g_idxUsed += indexCount;
    g_batchOpen = true;

    // One frame's debug dump (Brawl3dsPicaDebugNextFrame): the first batches, then the ones whose material may be
    // drawn wrong (approximated, a texture missing, three or more TEV stages), with their TEV stages.
    ++g_debugSerial;
    const bool suspect = g_plan.approximated || whiteUnits > 0 || g_gxState.numTevStages >= 3;
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
        const PicaVertex& v = g_vertices[g_batchVtx];
        std::fprintf(stderr,
                     "[pica] batch %u: fmt %d, %u verts, %u indices, tev %u stages, %d units (", g_debugBatches,
                     static_cast<int>(fmt), vtxCount, indexCount, g_gxState.numTevStages, g_plan.unitCount);
        for (int u = 0; u < g_plan.unitCount; ++u) {
            const auto& t = g_gxState.loadedTextures[g_plan.units[u].texMap];
            std::fprintf(stderr, "%smap %u coord %u %ux%u fmt %u", u ? ", " : "", g_plan.units[u].texMap,
                         g_plan.units[u].texCoord, t.width(), t.height(), t.format());
        }
        std::fprintf(stderr,
                     "), uv attrs %d %d, blend %d, z %d/%d/%d, cull %d, skip %d\n"
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
    if (f) std::fprintf(f, "GPU stalls (frames not drawn after 2 s of waiting): %u\n", g_gpuStalls);
    WriteTevStats(f, frames);
    WriteTextureStats(f, frames);
}

void Brawl3dsPicaEndFrame()
{
    g_skipFrame = false;
    if (!g_frameOpen) return;
    FlushBatch();
    GSPGPU_FlushDataCache(g_vertices, g_vtxUsed * sizeof(PicaVertex));
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
