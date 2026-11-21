#pragma once
// 3DS stand-in for Dawn's <webgpu/webgpu_cpp.h>, so Aurora's GX front end (lib/dolphin/gx, lib/gx/command_processor,
// lib/gx/shader_info) compiles for the PICA200 build. Aurora's headers carry WebGPU handles and descriptors in their
// structs; the 3DS back end (port/wiicompiled_arm32/aurora3ds/) never creates a WebGPU object, so every handle here is
// an empty, always-null value and every descriptor is a plain struct with the fields Aurora's code touches.

#include <cstdint>
#include <string_view>

// The few WebGPU C-API names Aurora's headers use directly.
typedef struct WGPUBindGroupImpl* WGPUBindGroup;
struct WGPUBindGroupDescriptor {
    const void* nextInChain = nullptr;
    const void* layout = nullptr;
    size_t entryCount = 0;
    const void* entries = nullptr;
};

namespace wgpu {

// An opaque, always-null handle (Aurora only stores, compares and tests these in the front end).
template <typename Tag>
class Handle {
public:
    Handle() = default;
    Handle(std::nullptr_t) {}
    explicit operator bool() const { return false; }
    bool operator==(const Handle&) const { return true; }
    bool operator!=(const Handle&) const { return false; }
    void* Get() const { return nullptr; }
};

#define BRAWL3DS_WGPU_HANDLE(Name) \
    struct Name##Tag {};           \
    using Name = Handle<Name##Tag>;
BRAWL3DS_WGPU_HANDLE(BindGroup)
BRAWL3DS_WGPU_HANDLE(BindGroupLayout)
BRAWL3DS_WGPU_HANDLE(Buffer)
BRAWL3DS_WGPU_HANDLE(CommandEncoder)
BRAWL3DS_WGPU_HANDLE(Device)
BRAWL3DS_WGPU_HANDLE(Instance)
BRAWL3DS_WGPU_HANDLE(Queue)
BRAWL3DS_WGPU_HANDLE(RenderPassEncoder)
BRAWL3DS_WGPU_HANDLE(RenderPipeline)
BRAWL3DS_WGPU_HANDLE(Sampler)
BRAWL3DS_WGPU_HANDLE(ShaderModule)
BRAWL3DS_WGPU_HANDLE(Surface)
BRAWL3DS_WGPU_HANDLE(Texture)
BRAWL3DS_WGPU_HANDLE(TextureView)
#undef BRAWL3DS_WGPU_HANDLE

enum class BackendType : uint32_t { Undefined, Null };

enum class TextureFormat : uint32_t {
    Undefined,
    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    BGRA8Unorm,
    R16Float,
    RGBA16Float,
    R32Float,
    Depth16Unorm,
    Depth24Plus,
    Depth24PlusStencil8,
    Depth32Float,
    BC1RGBAUnorm,
};

enum class TextureAspect : uint32_t { Undefined, All, StencilOnly, DepthOnly };
enum class AddressMode : uint32_t { Undefined, ClampToEdge, Repeat, MirrorRepeat };
enum class FilterMode : uint32_t { Undefined, Nearest, Linear };
enum class MipmapFilterMode : uint32_t { Undefined, Nearest, Linear };
enum class CompareFunction : uint32_t { Undefined, Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

struct StringView {
    const char* data = nullptr;
    size_t length = 0;
    StringView() = default;
    StringView(std::string_view s) : data(s.data()), length(s.size()) {}
    operator std::string_view() const { return data ? std::string_view(data, length) : std::string_view(); }
};

struct Extent3D {
    uint32_t width = 0;
    uint32_t height = 1;
    uint32_t depthOrArrayLayers = 1;
};

struct Origin3D {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
};

struct Color {
    double r = 0;
    double g = 0;
    double b = 0;
    double a = 0;
};

struct TexelCopyBufferLayout {
    uint64_t offset = 0;
    uint32_t bytesPerRow = 0;
    uint32_t rowsPerImage = 0;
};

struct TexelCopyTextureInfo {
    Texture texture;
    uint32_t mipLevel = 0;
    Origin3D origin;
    TextureAspect aspect = TextureAspect::All;
};

struct SamplerDescriptor {
    StringView label;
    AddressMode addressModeU = AddressMode::ClampToEdge;
    AddressMode addressModeV = AddressMode::ClampToEdge;
    AddressMode addressModeW = AddressMode::ClampToEdge;
    FilterMode magFilter = FilterMode::Nearest;
    FilterMode minFilter = FilterMode::Nearest;
    MipmapFilterMode mipmapFilter = MipmapFilterMode::Nearest;
    float lodMinClamp = 0.0f;
    float lodMaxClamp = 32.0f;
    CompareFunction compare = CompareFunction::Undefined;
    uint16_t maxAnisotropy = 1;
};

struct VertexBufferLayout {
    uint64_t arrayStride = 0;
};

struct SurfaceConfiguration {
    Device device;
    TextureFormat format = TextureFormat::Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace wgpu
