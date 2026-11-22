// GX texture formats to linear RGBA8 (see gx_texture_decode.hpp).

#include "gx_texture_decode.hpp"

namespace brawl3ds_pica {
namespace {

inline uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

// Reduction of the texture being decoded: 0 = full size, 1 = every second texel each way, 2 = every fourth.
uint32_t g_shift = 0;

inline void Put(uint8_t* out, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                uint8_t a)
{
    if (x >= w || y >= h) return;  // blocks past the edge of a texture whose size is not a block multiple
    if (g_shift) {
        if ((x | y) & ((1u << g_shift) - 1)) return;
        x >>= g_shift, y >>= g_shift;
        w = w >> g_shift ? w >> g_shift : 1;
    }
    uint8_t* p = out + (static_cast<std::size_t>(y) * w + x) * 4;
    p[0] = r, p[1] = g, p[2] = b, p[3] = a;
}

inline void Rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = static_cast<uint8_t>((c >> 11) << 3 | (c >> 13));
    g = static_cast<uint8_t>(((c >> 5) & 63) << 2 | ((c >> 9) & 3));
    b = static_cast<uint8_t>((c & 31) << 3 | ((c >> 2) & 7));
}

inline void Rgb5a3(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    if (c & 0x8000) {  // RGB555, opaque
        const uint32_t r5 = (c >> 10) & 31, g5 = (c >> 5) & 31, b5 = c & 31;
        r = static_cast<uint8_t>(r5 << 3 | r5 >> 2), g = static_cast<uint8_t>(g5 << 3 | g5 >> 2);
        b = static_cast<uint8_t>(b5 << 3 | b5 >> 2), a = 255;
    } else {  // A3 RGB444
        const uint32_t a3 = (c >> 12) & 7;
        r = static_cast<uint8_t>(((c >> 8) & 15) * 17), g = static_cast<uint8_t>(((c >> 4) & 15) * 17);
        b = static_cast<uint8_t>((c & 15) * 17), a = static_cast<uint8_t>(a3 << 5 | a3 << 2 | a3 >> 1);
    }
}

inline void Palette(const uint8_t* tlut, uint32_t format, uint32_t entries, uint32_t index, uint8_t& r, uint8_t& g,
                    uint8_t& b, uint8_t& a)
{
    if (!tlut || index >= entries) {
        r = g = b = a = 0;
        return;
    }
    const uint16_t c = Be16(tlut + index * 2);
    switch (format) {
    case kGxTlutIA8: a = static_cast<uint8_t>(c >> 8), r = g = b = static_cast<uint8_t>(c & 0xFF); break;
    case kGxTlutRGB565: Rgb565(c, r, g, b), a = 255; break;
    default: Rgb5a3(c, r, g, b, a); break;
    }
}

// Calls fn(x, y, src) for every texel of a texture tiled in bw x bh blocks of `blockBytes`, where `src` is the
// texel's block and fn finds the texel in it from (x % bw, y % bh).
template <typename Fn>
void ForBlocks(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t blockBytes, const uint8_t* src, Fn fn)
{
    for (uint32_t by = 0; by < h; by += bh)
        for (uint32_t bx = 0; bx < w; bx += bw, src += blockBytes)
            for (uint32_t y = 0; y < bh; ++y)
                for (uint32_t x = 0; x < bw; ++x) fn(bx + x, by + y, x, y, src);
}

void DecodeCmpr(const uint8_t* src, uint32_t w, uint32_t h, uint8_t* out)
{
    for (uint32_t by = 0; by < h; by += 8)
        for (uint32_t bx = 0; bx < w; bx += 8)
            for (uint32_t sub = 0; sub < 4; ++sub, src += 8) {  // sub-blocks: top left, top right, bottom left, bottom right
                const uint32_t ox = bx + (sub & 1) * 4, oy = by + (sub >> 1) * 4;
                const uint16_t c0 = Be16(src), c1 = Be16(src + 2);
                uint8_t pal[4][4];
                Rgb565(c0, pal[0][0], pal[0][1], pal[0][2]);
                Rgb565(c1, pal[1][0], pal[1][1], pal[1][2]);
                pal[0][3] = pal[1][3] = 255;
                for (int c = 0; c < 3; ++c) {
                    if (c0 > c1) {
                        pal[2][c] = static_cast<uint8_t>((2 * pal[0][c] + pal[1][c]) / 3);
                        pal[3][c] = static_cast<uint8_t>((pal[0][c] + 2 * pal[1][c]) / 3);
                    } else {
                        pal[2][c] = static_cast<uint8_t>((pal[0][c] + pal[1][c]) / 2);
                        pal[3][c] = 0;
                    }
                }
                pal[2][3] = 255;
                pal[3][3] = c0 > c1 ? 255 : 0;
                for (uint32_t y = 0; y < 4; ++y) {
                    const uint8_t bits = src[4 + y];
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint8_t* p = pal[(bits >> (6 - 2 * x)) & 3];
                        Put(out, w, h, ox + x, oy + y, p[0], p[1], p[2], p[3]);
                    }
                }
            }
}
} // namespace

bool DecodeGxTexture(uint32_t format, const uint8_t* src, uint32_t w, uint32_t h, const uint8_t* tlut,
                     uint32_t tlutFormat, uint32_t tlutEntries, uint8_t* out, uint32_t shift)
{
    g_shift = shift;
    switch (format) {
    case kGxI4:
        ForBlocks(w, h, 8, 8, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            const uint8_t v = b[ty * 4 + tx / 2];
            const uint8_t i = static_cast<uint8_t>(((tx & 1) ? v & 15 : v >> 4) * 17);
            Put(out, w, h, x, y, i, i, i, i);
        });
        return true;
    case kGxI8:
        ForBlocks(w, h, 8, 4, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            const uint8_t i = b[ty * 8 + tx];
            Put(out, w, h, x, y, i, i, i, i);
        });
        return true;
    case kGxIA4:
        ForBlocks(w, h, 8, 4, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            const uint8_t v = b[ty * 8 + tx];
            const uint8_t i = static_cast<uint8_t>((v & 15) * 17), a = static_cast<uint8_t>((v >> 4) * 17);
            Put(out, w, h, x, y, i, i, i, a);
        });
        return true;
    case kGxIA8:
        ForBlocks(w, h, 4, 4, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            const uint8_t* p = b + (ty * 4 + tx) * 2;
            Put(out, w, h, x, y, p[1], p[1], p[1], p[0]);
        });
        return true;
    case kGxRGB565:
        ForBlocks(w, h, 4, 4, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            uint8_t r, g, bl;
            Rgb565(Be16(b + (ty * 4 + tx) * 2), r, g, bl);
            Put(out, w, h, x, y, r, g, bl, 255);
        });
        return true;
    case kGxRGB5A3:
        ForBlocks(w, h, 4, 4, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            uint8_t r, g, bl, a;
            Rgb5a3(Be16(b + (ty * 4 + tx) * 2), r, g, bl, a);
            Put(out, w, h, x, y, r, g, bl, a);
        });
        return true;
    case kGxRGBA8:
        // 64-byte blocks: 16 (A, R) pairs, then 16 (G, B) pairs.
        ForBlocks(w, h, 4, 4, 64, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            const uint32_t i = ty * 4 + tx;
            Put(out, w, h, x, y, b[i * 2 + 1], b[32 + i * 2], b[32 + i * 2 + 1], b[i * 2]);
        });
        return true;
    case kGxC4:
        ForBlocks(w, h, 8, 8, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            const uint8_t v = b[ty * 4 + tx / 2];
            uint8_t r, g, bl, a;
            Palette(tlut, tlutFormat, tlutEntries, (tx & 1) ? v & 15 : v >> 4, r, g, bl, a);
            Put(out, w, h, x, y, r, g, bl, a);
        });
        return true;
    case kGxC8:
        ForBlocks(w, h, 8, 4, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            uint8_t r, g, bl, a;
            Palette(tlut, tlutFormat, tlutEntries, b[ty * 8 + tx], r, g, bl, a);
            Put(out, w, h, x, y, r, g, bl, a);
        });
        return true;
    case kGxC14X2:
        ForBlocks(w, h, 4, 4, 32, src, [&](uint32_t x, uint32_t y, uint32_t tx, uint32_t ty, const uint8_t* b) {
            uint8_t r, g, bl, a;
            Palette(tlut, tlutFormat, tlutEntries, Be16(b + (ty * 4 + tx) * 2) & 0x3FFF, r, g, bl, a);
            Put(out, w, h, x, y, r, g, bl, a);
        });
        return true;
    case kGxCMPR:
        DecodeCmpr(src, w, h, out);
        return true;
    default:
        return false;
    }
}

} // namespace brawl3ds_pica
