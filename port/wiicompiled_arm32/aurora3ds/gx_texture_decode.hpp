#pragma once
// GX texture formats to linear RGBA8, written into a caller-provided buffer (no allocations: the 3DS heap is too
// fragmented by the match for the temporary buffers a general-purpose converter allocates).
//
// GX stores textures in tiles (blocks): 8x8 texels for 4-bit formats, 8x4 for 8-bit, 4x4 for 16 and 32-bit, row by
// row of blocks; multi-byte values are big-endian. CMPR is DXT1 in 8x8 blocks of four 4x4 sub-blocks.

#include <cstddef>
#include <cstdint>

namespace brawl3ds_pica {

enum GxTexFormat : uint32_t {
    kGxI4 = 0x0, kGxI8 = 0x1, kGxIA4 = 0x2, kGxIA8 = 0x3, kGxRGB565 = 0x4, kGxRGB5A3 = 0x5, kGxRGBA8 = 0x6,
    kGxC4 = 0x8, kGxC8 = 0x9, kGxC14X2 = 0xA, kGxCMPR = 0xE,
};
enum GxTlutFormat : uint32_t { kGxTlutIA8 = 0, kGxTlutRGB565 = 1, kGxTlutRGB5A3 = 2 };

// Decodes the base level of a w x h texture into `out` (RGBA, (w >> shift) x (h >> shift) texels: `shift` keeps
// every 2^shift-th texel each way, for textures larger than the caller's buffer). `tlut` (big-endian entries, the
// palette format `tlutFormat`, `tlutEntries` long) is needed for C4/C8/C14X2. False for an unknown format.
bool DecodeGxTexture(uint32_t format, const uint8_t* src, uint32_t w, uint32_t h, const uint8_t* tlut,
                     uint32_t tlutFormat, uint32_t tlutEntries, uint8_t* out, uint32_t shift = 0);

} // namespace brawl3ds_pica
