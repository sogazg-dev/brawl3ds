#pragma once
// Scalar stand-ins for the AArch64 NEON intrinsics WiiCompiled's ISA headers use
// (runtime/include/isa/ppc_isa_{float,quantized}.h), so their AArch64 paths compile
// unchanged for the 3DS's ARM11 MPCore (ARMv6K + VFPv2, no NEON).
//
// Only the ~35 intrinsics those headers actually use are provided. Each one is a plain
// two-lane (or one-lane) loop; with -O2 they become a couple of VFP instructions.
//
// Multiply-add: VFPv2 has no fused multiply-add, and newlib's fma/fmaf on this target are the unfused
// vmla.f64 (fmaf converting to double and back), so neither was ever fused here. Brawl3dsHostFma/Fmaf compute the
// same thing inline (-ffp-contract=off keeps the multiply and the add separately rounded, exactly like vmla),
// without a library call per guest multiply-add.

#if !defined(__arm__) || defined(__aarch64__)
#error "brawl3ds_neon_shim.h is only for 32-bit ARM hosts"
#endif

#include <cmath>
#include <cstdint>
#include <cstring>

struct float32x2_t { float v[2]; };
struct uint32x2_t { uint32_t v[2]; };
struct uint64x1_t { uint64_t v[1]; };
struct float64x1_t { double v[1]; };

namespace brawl3ds_neon {
template <typename To, typename From>
inline To Bits(const From& from) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "reinterpret needs equal sizes");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}
} // namespace brawl3ds_neon

// Reinterpretations (bit casts between 64-bit vector types).
inline float32x2_t vreinterpret_f32_u32(uint32x2_t a) { return brawl3ds_neon::Bits<float32x2_t>(a); }
inline uint32x2_t vreinterpret_u32_f32(float32x2_t a) { return brawl3ds_neon::Bits<uint32x2_t>(a); }
inline uint64x1_t vreinterpret_u64_f64(float64x1_t a) { return brawl3ds_neon::Bits<uint64x1_t>(a); }
inline float64x1_t vreinterpret_f64_u64(uint64x1_t a) { return brawl3ds_neon::Bits<float64x1_t>(a); }
inline uint64x1_t vreinterpret_u64_u32(uint32x2_t a) { return brawl3ds_neon::Bits<uint64x1_t>(a); }
inline float64x1_t vreinterpret_f64_f32(float32x2_t a) { return brawl3ds_neon::Bits<float64x1_t>(a); }
inline float32x2_t vreinterpret_f32_f64(float64x1_t a) { return brawl3ds_neon::Bits<float32x2_t>(a); }

// Broadcasts.
inline uint32x2_t vdup_n_u32(uint32_t x) { return {{x, x}}; }
inline float32x2_t vdup_n_f32(float x) { return {{x, x}}; }
inline uint64x1_t vdup_n_u64(uint64_t x) { return {{x}}; }
inline float64x1_t vdup_n_f64(double x) { return {{x}}; }
inline float32x2_t vdup_lane_f32(float32x2_t a, int lane) { return {{a.v[lane], a.v[lane]}}; }

// Lane access.
inline float vget_lane_f32(float32x2_t a, int lane) { return a.v[lane]; }
inline double vget_lane_f64(float64x1_t a, int) { return a.v[0]; }
inline uint64_t vget_lane_u64(uint64x1_t a, int) { return a.v[0]; }
inline float32x2_t vset_lane_f32(float x, float32x2_t a, int lane) { a.v[lane] = x; return a; }

// Float arithmetic (two lanes).
inline float32x2_t vadd_f32(float32x2_t a, float32x2_t b) { return {{a.v[0] + b.v[0], a.v[1] + b.v[1]}}; }
inline float32x2_t vsub_f32(float32x2_t a, float32x2_t b) { return {{a.v[0] - b.v[0], a.v[1] - b.v[1]}}; }
inline float32x2_t vmul_f32(float32x2_t a, float32x2_t b) { return {{a.v[0] * b.v[0], a.v[1] * b.v[1]}}; }
inline float32x2_t vdiv_f32(float32x2_t a, float32x2_t b) { return {{a.v[0] / b.v[0], a.v[1] / b.v[1]}}; }
inline float32x2_t vneg_f32(float32x2_t a) { return {{-a.v[0], -a.v[1]}}; }
// Bit-identical to newlib's fma / fmaf on ARM32 VFPv2 (vmla.f64: double multiply, rounded, then double add).
inline double Brawl3dsHostFma(double a, double b, double c) { return a * b + c; }
inline float Brawl3dsHostFmaf(float a, float b, float c)
{
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c));
}
// NEON vfma_f32(a, b, c) = a + b * c.
inline float32x2_t vfma_f32(float32x2_t a, float32x2_t b, float32x2_t c)
{
    return {{Brawl3dsHostFmaf(b.v[0], c.v[0], a.v[0]), Brawl3dsHostFmaf(b.v[1], c.v[1], a.v[1])}};
}

// Comparisons produce all-ones / all-zeros lane masks.
inline uint32x2_t vceq_f32(float32x2_t a, float32x2_t b)
{
    return {{a.v[0] == b.v[0] ? 0xFFFFFFFFu : 0u, a.v[1] == b.v[1] ? 0xFFFFFFFFu : 0u}};
}
inline uint64x1_t vclt_f64(float64x1_t a, float64x1_t b) { return {{a.v[0] < b.v[0] ? ~0ull : 0ull}}; }

// Bitwise operations.
inline uint32x2_t vmvn_u32(uint32x2_t a) { return {{~a.v[0], ~a.v[1]}}; }
inline uint32x2_t vorr_u32(uint32x2_t a, uint32x2_t b) { return {{a.v[0] | b.v[0], a.v[1] | b.v[1]}}; }
inline uint32x2_t veor_u32(uint32x2_t a, uint32x2_t b) { return {{a.v[0] ^ b.v[0], a.v[1] ^ b.v[1]}}; }
inline uint64x1_t vand_u64(uint64x1_t a, uint64x1_t b) { return {{a.v[0] & b.v[0]}}; }
inline uint64x1_t vbic_u64(uint64x1_t a, uint64x1_t b) { return {{a.v[0] & ~b.v[0]}}; }

// Bit select: for each bit, mask ? a : b.
inline float32x2_t vbsl_f32(uint32x2_t mask, float32x2_t a, float32x2_t b)
{
    const uint32x2_t ua = vreinterpret_u32_f32(a), ub = vreinterpret_u32_f32(b);
    return vreinterpret_f32_u32({{(mask.v[0] & ua.v[0]) | (~mask.v[0] & ub.v[0]),
                                  (mask.v[1] & ua.v[1]) | (~mask.v[1] & ub.v[1])}});
}
inline uint64x1_t vbsl_u64(uint64x1_t mask, uint64x1_t a, uint64x1_t b)
{
    return {{(mask.v[0] & a.v[0]) | (~mask.v[0] & b.v[0])}};
}
