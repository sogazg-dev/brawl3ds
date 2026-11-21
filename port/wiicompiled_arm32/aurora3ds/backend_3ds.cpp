// 3DS back end for Aurora's GX front end (renderer milestone 1, docs/RENDERER_3DS.md).
//
// Aurora's lib/dolphin/gx (GX API -> g_gxState) and lib/gx/command_processor (display lists and packed draws) run
// unchanged; they emit work through the gfx:: staging layer and a few gx.cpp state functions, which WebGPU
// implements on PC. Here:
//   - push_verts/indices/storage and map_uniform copy into one per-frame CPU arena and return ranges into it;
//   - pipeline_ref keeps every distinct pipeline config in a table and returns its index;
//   - push_draw_command records the draw. Milestone 1 only counts draws and vertices (Brawl3dsRenderStats); the
//     PICA200 draw path (vertex decode on the CPU, citro3d) replaces the counting in milestone 2;
//   - texture objects, EFB copies, frame interpolation and window/VI sizing are inert or fixed to the 400x240 top
//     screen.
// Aurora is MIT licensed (Copyright (c) 2022 Luke Street); this file is new code against its interfaces.

#include "gfx/clear.hpp"
#include "gfx/common.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/texture.hpp"
#include "gfx/texture_replacement.hpp"
#include "gx/frame_interpolation.hpp"
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include "internal.hpp"
#include "logging.hpp"
#include "window.hpp"
#include "tracy/Tracy.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

extern "C" {
#include <3ds/svc.h>  // libctru headers have C linkage only through <3ds.h>
#include <3ds/os.h>   // SYSCLOCK_ARM11
}

extern "C" void Brawl3dsRendererCountDraw(uint32_t vertices, uint32_t indices);

namespace {
constexpr uint32_t kTopWidth = 400;
constexpr uint32_t kTopHeight = 240;

// One frame's staged bytes (vertices, indices, uniforms), in reusable 256 KiB chunks: a match frame stages over a
// megabyte, and one growing vector needed a contiguous block twice that size, which the fragmented regular heap did
// not have (std::bad_alloc 13 frames into the match). A range's offset is (chunk << 18) | offset in the chunk; a
// staging larger than a chunk gets a chunk of its own.
class Arena {
public:
    static constexpr uint32_t kChunkShift = 18;
    static constexpr size_t kChunkSize = size_t{1} << kChunkShift;

    uint32_t push(const uint8_t* data, size_t length, uint32_t align)
    {
        size_t offset = (m_pos + (align - 1)) & ~static_cast<size_t>(align - 1);
        if (!m_started || offset + length > m_chunks[m_current].size) {
            next_chunk(length);
            offset = 0;
        }
        Chunk& chunk = m_chunks[m_current];
        if (data && length) std::memcpy(chunk.bytes + offset, data, length);
        m_pos = offset + length;
        return static_cast<uint32_t>((m_current << kChunkShift) | offset);
    }
    uint8_t* at(uint32_t encoded)
    {
        return m_chunks[encoded >> kChunkShift].bytes + (encoded & (kChunkSize - 1));
    }
    // A new frame: chunks are reused; one-off oversized chunks and anything past the first eight are released.
    void reset()
    {
        size_t kept = 0;
        for (Chunk& chunk : m_chunks) {
            if (chunk.size == kChunkSize && kept < 8) {
                m_chunks[kept++] = chunk;
            } else {
                std::free(chunk.bytes);
            }
        }
        m_chunks.resize(kept);
        m_current = 0;
        m_pos = 0;
        m_started = false;
    }

private:
    struct Chunk {
        uint8_t* bytes;
        size_t size;
    };
    void next_chunk(size_t length)
    {
        if (length > kChunkSize) {  // a staging of its own
            m_chunks.push_back({static_cast<uint8_t*>(std::malloc(length)), length});
            m_current = static_cast<uint32_t>(m_chunks.size() - 1);
        } else {
            size_t next = m_started ? m_current + 1 : 0;
            while (next < m_chunks.size() && m_chunks[next].size != kChunkSize) ++next;
            if (next >= m_chunks.size()) {
                m_chunks.push_back({static_cast<uint8_t*>(std::malloc(kChunkSize)), kChunkSize});
                next = m_chunks.size() - 1;
            }
            m_current = static_cast<uint32_t>(next);
        }
        if (!m_chunks[m_current].bytes) std::abort();
        m_started = true;
        m_pos = 0;
    }
    std::vector<Chunk> m_chunks;
    uint32_t m_current = 0;
    size_t m_pos = 0;
    bool m_started = false;  // m_current names the chunk being filled this frame
};
Arena g_arena;
// push_storage: the game's vertex attribute arrays, which stay where they are in guest memory for the whole frame.
// The range's offset indexes this table instead of a copy.
std::vector<const uint8_t*> g_storage;
aurora::gx::DrawData g_lastDraw{};
bool g_haveDraw = false;     // g_lastDraw is this frame's current draw (unchanged-state draws append to it)
GXVtxFmt g_lastDrawFmt = GX_VTXFMT0;
uint64_t g_appendedDraws = 0, g_appendedVerts = 0;
std::vector<aurora::gx::PipelineConfig> g_pipelines;
std::unordered_multimap<size_t, size_t> g_pipelineIndex;  // config hash -> index in g_pipelines
uint64_t g_displayListTicks = 0;  // time in Aurora's GXCallDisplayList (the runtime's display-list draws)
uint32_t g_frame = 0;
} // namespace

namespace aurora {
AuroraConfig g_config{};

void log_internal(AuroraLogLevel level, const char* module, const char* message, unsigned int len) noexcept
{
    if (level < LOG_WARNING) return;
    std::fprintf(stderr, "[aurora %s] %.*s\n", module ? module : "", static_cast<int>(len), message);
}

void Module::show_fatal_dialog(const char* module, std::string_view message) noexcept
{
    std::fprintf(stderr, "[aurora %s] FATAL: %.*s\n", module ? module : "", static_cast<int>(message.size()),
                 message.data());
}

std::recursive_mutex& renderer_gpu_mutex() noexcept
{
    static std::recursive_mutex mutex;
    return mutex;
}

// No frame worker on the 3DS (yet): nothing to wait for.
std::chrono::nanoseconds wait_for_frame_worker_sealed() noexcept { return std::chrono::nanoseconds{0}; }

namespace vi {
Vec2<uint32_t> configured_fb_size() noexcept { return {640, 456}; }  // Brawl's EFB; scaled to the top screen later
Vec2<uint32_t> visible_fb_size() noexcept { return {640, 456}; }
} // namespace vi

namespace window {
AuroraWindowSize get_window_size()
{
    AuroraWindowSize size{};
    size.width = kTopWidth;
    size.height = kTopHeight;
    size.fb_width = kTopWidth;
    size.fb_height = kTopHeight;
    size.native_fb_width = kTopWidth;
    size.native_fb_height = kTopHeight;
    size.scale = 1.0f;
    return size;
}
void set_frame_buffer_aspect_fit(bool) {}
void set_fixed_aspect(float) {}
void set_present_surface_fill(bool) {}
} // namespace window
} // namespace aurora

namespace aurora::gfx {
uint32_t g_mergedDrawCallCount = 0;

uint32_t current_frame() noexcept { return g_frame; }
void begin_offscreen(uint32_t, uint32_t) {}
void end_offscreen() {}
uint32_t get_sample_count() noexcept { return 1; }
Vec2<uint32_t> get_render_target_size() noexcept { return {640, 456}; }
Vec2<uint32_t> get_frame_buffer_size() noexcept { return {640, 456}; }
void push_debug_group(std::string) {}
void insert_debug_marker(std::string) {}

Range push_verts(const uint8_t* data, size_t length)
{
    return {g_arena.push(data, length, 4), static_cast<uint32_t>(length)};
}
Range push_indices(const uint8_t* data, size_t length)
{
    return {g_arena.push(data, length, 4), static_cast<uint32_t>(length)};
}
Range push_storage(const uint8_t* data, size_t length)
{
    g_storage.push_back(data);  // no copy: the array stays in guest memory for the frame
    return {static_cast<uint32_t>(g_storage.size() - 1), static_cast<uint32_t>(length)};
}
std::pair<ByteBuffer, Range> map_uniform(size_t length)
{
    const uint32_t offset = g_arena.push(nullptr, length, 16);
    return {ByteBuffer{g_arena.at(offset), length}, Range{offset, static_cast<uint32_t>(length)}};
}
uint32_t align_uniform(uint32_t value) { return (value + 15u) & ~15u; }
uint64_t staging_uniform_bytes(uint64_t bytes) { return (bytes + 15u) & ~uint64_t{15}; }
uint64_t staging_storage_bytes(uint64_t bytes) { return (bytes + 3u) & ~uint64_t{3}; }
bool staging_has_space(const StagingSizes&) { return true; }  // the arena grows
void split_staging_batch() {}

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config)
{
    // Hashed: the first version compared each new config with every config seen so far (a linear memcmp of a
    // large struct), which the command processor's small 3DS pipeline-state cache calls often.
    const std::string_view bytes(reinterpret_cast<const char*>(&config), sizeof(config));
    const size_t hash = std::hash<std::string_view>{}(bytes);
    auto [begin, end] = g_pipelineIndex.equal_range(hash);
    for (auto it = begin; it != end; ++it) {
        if (std::memcmp(&g_pipelines[it->second], &config, sizeof(config)) == 0) return it->second;
    }
    g_pipelines.push_back(config);
    g_pipelineIndex.emplace(hash, g_pipelines.size() - 1);
    return g_pipelines.size() - 1;
}
template <>
PipelineRef pipeline_ref(const clear::PipelineConfig&)
{
    return 0;
}

template <>
void push_draw_command(gx::DrawData data)
{
    g_lastDraw = data;
    g_haveDraw = true;
    g_lastDrawFmt = gx::g_gxState.lastVtxFmt;
    Brawl3dsRendererCountDraw(data.vtxCount, data.indexCount);
}
template <>
void push_draw_command(clear::DrawData)
{
}
template <>
gx::DrawData* get_last_draw_command()
{
    return &g_lastDraw;
}

TextureHandle new_render_texture(uint32_t, uint32_t, u32, const char*) noexcept { return {}; }
TextureHandle new_conv_texture(uint32_t, uint32_t, u32, const char*) noexcept { return {}; }
bool add_resolve_patch(const TextureHandle&, TextureHandle, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
void resolve_pass(TextureHandle, ClipRect, bool, bool, bool, Vec4<float>, float, GXTexFmt, const Vec4<float>*, bool,
                  const std::array<u32, 3>*, bool, float, bool, bool, bool)
{
}

namespace depth_peek {
void request_snapshot() noexcept {}
bool read_latest(uint16_t, uint16_t, uint32_t& z) noexcept
{
    z = 0xFFFFFF;
    return false;
}
void poll() noexcept {}
} // namespace depth_peek

namespace efb_ram {
void schedule(void*, uint32_t, uint32_t, GXTexFmt, TextureHandle, uint32_t) noexcept {}
} // namespace efb_ram

namespace tex_copy_conv {
bool needs_conversion(GXTexFmt) { return false; }
} // namespace tex_copy_conv

namespace texture_replacement {
void register_tlut(const GXTlutObj*, const void*, GXTlutFmt, uint16_t) noexcept {}
void load_tlut(const GXTlutObj*, uint32_t) noexcept {}
} // namespace texture_replacement
} // namespace aurora::gfx

namespace aurora::gx {
GXState g_gxState{};

namespace detail {
std::atomic_uint32_t g_frameInterpolationFps{0};
} // namespace detail
bool frame_interpolation_replay_safe() noexcept { return true; }
std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(const FrameInterpolationDrawIdentity&,
                                                                        const Mat4x4<float>&, uint16_t,
                                                                        const InterpolatedUniformLayout&) noexcept
{
    return {};
}
void extend_interpolation_draw(uint16_t) noexcept {}

Vec2<uint32_t> logical_fb_size() noexcept { return vi::configured_fb_size(); }

MappedRenderState map_logical_render_state() noexcept
{
    return {.viewport = g_gxState.logicalViewport, .scissor = g_gxState.logicalScissor};
}
void set_logical_viewport(const gfx::Viewport& viewport) noexcept
{
    g_gxState.stateDirty = g_gxState.stateDirty || viewport != g_gxState.logicalViewport;
    g_gxState.logicalViewport = viewport;
    g_gxState.renderViewport = viewport;
}
void set_render_viewport(const gfx::Viewport& viewport) noexcept
{
    if (viewport == g_gxState.renderViewport) return;
    g_gxState.renderViewport = viewport;
    g_gxState.stateDirty = true;
}
void set_logical_scissor(const gfx::ClipRect& scissor) noexcept
{
    g_gxState.logicalScissor = scissor;
    g_gxState.renderScissor = scissor;
}
void set_render_scissor(const gfx::ClipRect& scissor) noexcept { g_gxState.renderScissor = scissor; }

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }
void resolve_sampled_textures(const ShaderInfo&) noexcept {}
GXBindGroups build_bind_groups(const ShaderInfo&) noexcept { return {}; }
void evict_copy_texture(const void*) noexcept {}
void evict_texture_object(u32) noexcept {}
void evict_tlut_object(u32) noexcept {}
void invalidate_static_texture_cache() noexcept {}
GXState::CopyTextureRef* find_copy_texture_for_texobj(const GXTexObj_&) noexcept { return nullptr; }
std::vector<PatchTarget> find_patch_targets(const void*) { return {}; }
void notify_copy_texture_created() noexcept {}
void set_display_copy_present_source() noexcept {}

u8 comp_type_size(GXAttr attr, GXCompType type) noexcept
{
    if (attr >= GX_VA_PNMTXIDX && attr <= GX_VA_TEX7MTXIDX) return 1;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        switch (type) {
        case GX_RGB565:
        case GX_RGBA4:
            return 2;
        case GX_RGB8:
        case GX_RGBA6:
            return 3;
        default:
            return 4;  // GX_RGBX8, GX_RGBA8
        }
    }
    switch (type) {
    case GX_U8:
    case GX_S8:
        return 1;
    case GX_U16:
    case GX_S16:
        return 2;
    default:
        return 4;  // GX_F32
    }
}

u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept
{
    if (attr >= GX_VA_PNMTXIDX && attr <= GX_VA_TEX7MTXIDX) return 1;
    switch (attr) {
    case GX_VA_POS:
        return cnt == GX_POS_XY ? 2 : 3;
    case GX_VA_NRM:
        return cnt == GX_NRM_XYZ ? 3 : 9;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        return 1;
    default:
        return cnt == GX_TEX_S ? 1 : 2;  // GX_VA_TEX0..7
    }
}

// Milestone 1: only the vertex layout (what the draw decoder will need); the TEV, channel and blend parts of the
// config come with the PICA draw path.
void populate_pipeline_config(PipelineConfig& config, GXPrimitive, GXVtxFmt fmt) noexcept
{
    const auto& vtxFmt = g_gxState.vtxFmts[fmt];
    u8 offset = 0;
    for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
        const auto attr = static_cast<GXAttr>(i);
        const auto type = g_gxState.vtxDesc[i];
        auto& mapping = config.shaderConfig.attrs[i];
        if (type == GX_NONE) {
            mapping = {};
            continue;
        }
        const auto& attrFmt = vtxFmt.attrs[i];
        mapping = {};
        mapping.attrType = static_cast<u8>(type);
        mapping.cnt = comp_cnt_count(attr, attrFmt.cnt);
        mapping.compType = static_cast<u8>(attrFmt.type);
        mapping.offset = offset;
        offset += type == GX_DIRECT ? comp_type_size(attr, attrFmt.type) * mapping.cnt
                                    : (type == GX_INDEX16 ? 2 : 1);
    }
}
} // namespace aurora::gx

// A GX draw with no state change since the current draw (tools/patch_wiicompiled_arm32.py hooks it into Aurora's
// handle_draw ahead of its merge path). Milestone 1 counts it; milestone 2 decodes its vertices into the current
// PICA batch. Lines and points keep Aurora's path (they draw expanded), as does a draw in another vertex format.
namespace aurora::gx::fifo {
bool brawl3ds_append_draw(GXPrimitive prim, GXVtxFmt fmt, const u8*, u16 vtxCount, u32, bool) noexcept
{
    if (!g_haveDraw || g_lastDraw.expandedPrimitive || fmt != g_lastDrawFmt || prim == GX_LINES ||
        prim == GX_LINESTRIP || prim == GX_POINTS) {
        return false;
    }
    ++g_appendedDraws;
    g_appendedVerts += vtxCount;
    Brawl3dsRendererCountDraw(vtxCount, 0);
    return true;
}
} // namespace aurora::gx::fifo

// Aurora's ZoneScoped sites (aurora3ds/tracy/Tracy.hpp): per-site inclusive time and calls over the match.
namespace brawl3ds_zones {
Site*& Head() noexcept
{
    static Site* head = nullptr;
    return head;
}
} // namespace brawl3ds_zones

extern "C" void Brawl3dsZonesReset()
{
    for (auto* site = brawl3ds_zones::Head(); site; site = site->next) site->ticks = site->calls = 0;
}

extern "C" void Brawl3dsZonesWrite(std::FILE* f, unsigned frames)
{
    if (!f || frames == 0) return;
    std::fprintf(f, "Aurora GX front-end zones (inclusive, per match frame):\n");
    for (auto* site = brawl3ds_zones::Head(); site; site = site->next) {
        if (!site->calls) continue;
        const double ms = static_cast<double>(site->ticks) * 1000.0 / SYSCLOCK_ARM11 / frames;
        std::fprintf(f, "  %8.2f ms  %8.1f calls  %6.2f us/call  %s:%d\n", ms, static_cast<double>(site->calls) / frames,
                     ms * 1000.0 * frames / static_cast<double>(site->calls), site->name, site->line);
    }
}

// The runtime's display-list draws, timed (-Wl,--wrap=GXCallDisplayList in port/headless/CMakeLists.txt: calls from
// the runtime go through here, Aurora's own internal calls do not).
extern "C" void __real_GXCallDisplayList(const void* list, u32 bytes);
extern "C" void __wrap_GXCallDisplayList(const void* list, u32 bytes)
{
    const uint64_t start = svcGetSystemTick();
    __real_GXCallDisplayList(list, bytes);
    g_displayListTicks += svcGetSystemTick() - start;
}

// For the match summary: ticks spent in GXCallDisplayList since the last call, and the distinct pipeline configs.
extern "C" void Brawl3dsRendererTakeStats(uint64_t* displayListTicks, uint32_t* pipelines)
{
    *displayListTicks = g_displayListTicks;
    g_displayListTicks = 0;
    *pipelines = static_cast<uint32_t>(g_pipelines.size());
}

// Called once per game frame by the 3DS frame hook: the staged bytes of the frame are no longer needed.
extern "C" void Brawl3dsRendererEndFrame()
{
    g_arena.reset();
    g_storage.clear();
    g_haveDraw = false;  // a new frame has no current draw
    ++g_frame;
}
