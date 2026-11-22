// GX TEV and pixel state on the PICA200 (renderer milestone 3).
//
// A GX TEV stage computes, separately for color and alpha, out = (d +/- lerp(a, b, c) + bias) * scale with
// a..d picked from registers, the texture, the rasterized (channel) color and constants. The PICA200 has six
// combiner stages, each one function of up to three sources (replace, modulate, add, subtract, interpolate,
// multiply-add, ...) with one constant color, plus one "buffer" that can hold an earlier stage's output.
//
// The translation keeps, for every GX register and each half (color, alpha), what it holds: a constant known on
// the CPU, or the output of an emitted PICA stage. Each GX stage becomes a short sum of products; constant parts are
// folded on the CPU, and the rest is emitted as one to three PICA stages (interpolate or multiply-add where the
// shape allows). A value from an older stage than the previous one is read through the combiner buffer when its
// update flags allow it. Whatever does not fit (more than six stages, buffer conflicts, comparison ops, a divide by
// two without a spare stage) is approximated and counted.

#include "pica_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace brawl3ds_pica {
namespace {
using namespace aurora::gx;

constexpr int kMaxPicaStages = 6;

enum Half { kRgb = 0, kAlpha = 1 };

// A value as the translation knows it.
struct Val {
    bool isConst = true;
    float c[3] = {0, 0, 0};  // the constant (rgb; alpha uses c[0])
    int stage = -1;          // not constant: the PICA stage whose output (this half) holds it
};

// One source of a PICA op.
struct Opnd {
    enum Kind { kConst, kSource, kStage } kind = kConst;
    float c[3] = {0, 0, 0};
    GPU_TEVSRC src = GPU_PRIMARY_COLOR;
    int stage = -1;
    int valueHalf = kRgb;  // kStage: which half's output holds the value
    int mod = 0;           // operand modifier (color: GPU_TEVOP_RGB_*, alpha: GPU_TEVOP_A_*)
};

struct PicaOp {
    GPU_COMBINEFUNC func = GPU_REPLACE;
    int src[3] = {GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS};
    int mod[3] = {0, 0, 0};
    bool hasConst = false;
    float c[3] = {0, 0, 0};
    GPU_TEVSCALE scale = GPU_TEVSCALE_1;
};

struct HalfProgram {
    PicaOp ops[16];
    int count = 0;
};

struct Compiler {
    HalfProgram prog[2];
    Val regs[2][4];      // [half][GX_TEVPREV, REG0, REG1, REG2]
    int bufferMask[2] = {0, 0};
    struct Reservation { int from, to; };
    Reservation reservations[2][8];
    int reservationCount[2] = {0, 0};
    bool approximated = false;
};

// Statistics (WriteTevStats).
uint64_t g_statMaterials = 0, g_statApprox = 0, g_statTruncated = 0, g_statStagesSum = 0, g_statGxStagesSum = 0;
uint64_t g_statUnitOverflow = 0, g_statCullAll = 0;
int g_statMaxStages = 0;

bool IsConstValue(const Opnd& o, int n, float v)
{
    if (o.kind != Opnd::kConst) return false;
    for (int i = 0; i < n; ++i)
        if (std::fabs(o.c[i] - v) > 1e-6f) return false;
    return true;
}

Opnd ConstOpnd(float r, float g, float b)
{
    Opnd o;
    o.kind = Opnd::kConst;
    o.c[0] = r, o.c[1] = g, o.c[2] = b;
    return o;
}

Opnd Invert(Opnd o, int n)
{
    if (o.kind == Opnd::kConst) {
        for (int i = 0; i < n; ++i) o.c[i] = 1.0f - o.c[i];
    } else {
        o.mod ^= 1;  // every PICA operand modifier has its "one minus" form one above it
    }
    return o;
}

// Whether the buffer can carry stage `from`'s output (of `half`) to stage `to`, reserving it if so. The PICA
// combiner buffer seen by stage k holds the output of the last stage j <= k - 2 whose update flag is set
// (only stages 0-3 have one).
bool ReserveBuffer(Compiler& cc, int half, int from, int to)
{
    if (from < 0 || from > 3 || from > to - 2) return false;
    const int mask = cc.bufferMask[half];
    for (int j = from + 1; j <= to - 2 && j <= 3; ++j)
        if (mask & (1 << j)) return false;  // a later update would overwrite it
    for (int r = 0; r < cc.reservationCount[half]; ++r) {
        const auto& res = cc.reservations[half][r];
        if (res.from == from) continue;
        if (from > res.from && from <= res.to - 2) return false;  // setting this flag breaks an earlier read
    }
    if (cc.reservationCount[half] >= 8) return false;
    cc.bufferMask[half] |= 1 << from;
    cc.reservations[half][cc.reservationCount[half]++] = {from, to};
    return true;
}

// Emits one PICA op into `half`'s program; false (nothing emitted) when its operands need two different
// constants.
bool Emit(Compiler& cc, int half, GPU_COMBINEFUNC func, std::initializer_list<Opnd> srcs,
          GPU_TEVSCALE scale = GPU_TEVSCALE_1)
{
    HalfProgram& p = cc.prog[half];
    if (p.count >= 16) {
        cc.approximated = true;
        return true;
    }
    PicaOp op;
    op.func = func;
    op.scale = scale;
    const int n = half == kRgb ? 3 : 1;
    const int k = p.count;
    int i = 0;
    for (const Opnd& o : srcs) {
        switch (o.kind) {
        case Opnd::kConst:
            if (op.hasConst) {
                for (int ch = 0; ch < n; ++ch)
                    if (std::fabs(op.c[ch] - o.c[ch]) > 1e-6f) return false;
            }
            op.hasConst = true;
            std::memcpy(op.c, o.c, sizeof(op.c));
            op.src[i] = GPU_CONSTANT;
            op.mod[i] = 0;
            break;
        case Opnd::kSource:
            op.src[i] = o.src;
            op.mod[i] = o.mod;
            break;
        case Opnd::kStage:
            if (o.stage == k - 1) {
                op.src[i] = GPU_PREVIOUS;
            } else if (ReserveBuffer(cc, o.valueHalf, o.stage, k)) {
                op.src[i] = GPU_PREVIOUS_BUFFER;
            } else {
                op.src[i] = GPU_PREVIOUS;  // the value is gone: the previous stage's output stands in
                cc.approximated = true;
            }
            op.mod[i] = o.mod;
            break;
        }
        ++i;
    }
    p.ops[p.count++] = op;
    return true;
}

Opnd StageOpnd(int stage, int valueHalf, int mod)
{
    Opnd o;
    o.kind = Opnd::kStage;
    o.stage = stage;
    o.valueHalf = valueHalf;
    o.mod = mod;
    return o;
}

// A register's value as an operand of `half`'s ops; `asAlpha`: the register's alpha (in a color op, broadcast).
Opnd RegOpnd(const Compiler& cc, int half, int reg, bool asAlpha)
{
    const int valueHalf = asAlpha ? kAlpha : kRgb;
    const Val& v = cc.regs[valueHalf][reg];
    if (v.isConst) {
        if (asAlpha) return ConstOpnd(v.c[0], v.c[0], v.c[0]);
        return ConstOpnd(v.c[0], v.c[1], v.c[2]);
    }
    const int mod = half == kRgb ? (asAlpha ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR) : GPU_TEVOP_A_SRC_ALPHA;
    return StageOpnd(v.stage, valueHalf, mod);
}

float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

int RgbModForSwap(const TevSwap& s)
{
    if (s.red == GX_CH_RED && s.green == GX_CH_GREEN && s.blue == GX_CH_BLUE) return GPU_TEVOP_RGB_SRC_COLOR;
    if (s.red == s.green && s.green == s.blue) {
        switch (s.red) {
        case GX_CH_RED: return GPU_TEVOP_RGB_SRC_R;
        case GX_CH_GREEN: return GPU_TEVOP_RGB_SRC_G;
        case GX_CH_BLUE: return GPU_TEVOP_RGB_SRC_B;
        default: return GPU_TEVOP_RGB_SRC_ALPHA;
        }
    }
    return GPU_TEVOP_RGB_SRC_COLOR;  // other swizzles are not expressible: approximated
}
int RgbModForChannel(GXTevColorChan ch)
{
    switch (ch) {
    case GX_CH_RED: return GPU_TEVOP_RGB_SRC_R;
    case GX_CH_GREEN: return GPU_TEVOP_RGB_SRC_G;
    case GX_CH_BLUE: return GPU_TEVOP_RGB_SRC_B;
    default: return GPU_TEVOP_RGB_SRC_ALPHA;
    }
}
int AlphaModForChannel(GXTevColorChan ch)
{
    switch (ch) {
    case GX_CH_RED: return GPU_TEVOP_A_SRC_R;
    case GX_CH_GREEN: return GPU_TEVOP_A_SRC_G;
    case GX_CH_BLUE: return GPU_TEVOP_A_SRC_B;
    default: return GPU_TEVOP_A_SRC_ALPHA;
    }
}

float KFraction(int sel) { return static_cast<float>(8 - sel) / 8.0f; }

Opnd KonstColor(GXTevKColorSel sel)
{
    const int s = static_cast<int>(sel);
    if (s <= 7) {
        const float f = KFraction(s);
        return ConstOpnd(f, f, f);
    }
    const auto& kc = g_gxState.kcolors;
    if (s >= GX_TEV_KCSEL_K0 && s <= GX_TEV_KCSEL_K0 + 3) {
        const auto& k = kc[s - GX_TEV_KCSEL_K0];
        return ConstOpnd(Clamp01(k[0]), Clamp01(k[1]), Clamp01(k[2]));
    }
    if (s >= GX_TEV_KCSEL_K0_R && s <= GX_TEV_KCSEL_K0_R + 15) {
        const int channel = (s - GX_TEV_KCSEL_K0_R) / 4, index = (s - GX_TEV_KCSEL_K0_R) % 4;
        const float v = Clamp01(kc[index][channel]);
        return ConstOpnd(v, v, v);
    }
    return ConstOpnd(1, 1, 1);
}

Opnd KonstAlpha(GXTevKAlphaSel sel)
{
    const int s = static_cast<int>(sel);
    if (s <= 7) return ConstOpnd(KFraction(s), 0, 0);
    if (s >= GX_TEV_KASEL_K0_R && s <= GX_TEV_KASEL_K0_R + 15) {
        const int channel = (s - GX_TEV_KASEL_K0_R) / 4, index = (s - GX_TEV_KASEL_K0_R) % 4;
        return ConstOpnd(Clamp01(g_gxState.kcolors[index][channel]), 0, 0);
    }
    return ConstOpnd(1, 0, 0);
}

struct StageContext {
    int unit = -1;            // PICA unit of this stage's texture, -1 = none
    bool rasZero = false;     // GX_COLOR_ZERO / no channel
    TevSwap rasSwap, texSwap;
};

Opnd ColorArg(const Compiler& cc, const TevStage& st, const StageContext& sc, GXTevColorArg arg)
{
    switch (arg) {
    case GX_CC_CPREV: return RegOpnd(cc, kRgb, 0, false);
    case GX_CC_APREV: return RegOpnd(cc, kRgb, 0, true);
    case GX_CC_C0: return RegOpnd(cc, kRgb, 1, false);
    case GX_CC_A0: return RegOpnd(cc, kRgb, 1, true);
    case GX_CC_C1: return RegOpnd(cc, kRgb, 2, false);
    case GX_CC_A1: return RegOpnd(cc, kRgb, 2, true);
    case GX_CC_C2: return RegOpnd(cc, kRgb, 3, false);
    case GX_CC_A2: return RegOpnd(cc, kRgb, 3, true);
    case GX_CC_TEXC:
    case GX_CC_TEXA: {
        if (sc.unit < 0) return ConstOpnd(1, 1, 1);
        Opnd o;
        o.kind = Opnd::kSource;
        o.src = static_cast<GPU_TEVSRC>(GPU_TEXTURE0 + sc.unit);
        o.mod = arg == GX_CC_TEXC ? RgbModForSwap(sc.texSwap) : RgbModForChannel(sc.texSwap.alpha);
        return o;
    }
    case GX_CC_RASC:
    case GX_CC_RASA: {
        if (sc.rasZero) return ConstOpnd(0, 0, 0);
        Opnd o;
        o.kind = Opnd::kSource;
        o.src = GPU_PRIMARY_COLOR;
        o.mod = arg == GX_CC_RASC ? RgbModForSwap(sc.rasSwap) : RgbModForChannel(sc.rasSwap.alpha);
        return o;
    }
    case GX_CC_ONE: return ConstOpnd(1, 1, 1);
    case GX_CC_HALF: return ConstOpnd(0.5f, 0.5f, 0.5f);
    case GX_CC_KONST: return KonstColor(st.kcSel);
    default: return ConstOpnd(0, 0, 0);
    }
}

Opnd AlphaArg(const Compiler& cc, const TevStage& st, const StageContext& sc, GXTevAlphaArg arg)
{
    switch (arg) {
    case GX_CA_APREV: return RegOpnd(cc, kAlpha, 0, true);
    case GX_CA_A0: return RegOpnd(cc, kAlpha, 1, true);
    case GX_CA_A1: return RegOpnd(cc, kAlpha, 2, true);
    case GX_CA_A2: return RegOpnd(cc, kAlpha, 3, true);
    case GX_CA_TEXA: {
        if (sc.unit < 0) return ConstOpnd(1, 0, 0);
        Opnd o;
        o.kind = Opnd::kSource;
        o.src = static_cast<GPU_TEVSRC>(GPU_TEXTURE0 + sc.unit);
        o.mod = AlphaModForChannel(sc.texSwap.alpha);
        return o;
    }
    case GX_CA_RASA: {
        if (sc.rasZero) return ConstOpnd(0, 0, 0);
        Opnd o;
        o.kind = Opnd::kSource;
        o.src = GPU_PRIMARY_COLOR;
        o.mod = AlphaModForChannel(sc.rasSwap.alpha);
        return o;
    }
    case GX_CA_KONST: return KonstAlpha(st.kaSel);
    default: return ConstOpnd(0, 0, 0);
    }
}

// A product of up to two factors.
struct Term {
    Opnd f[2];
    int factors = 1;
    bool negative = false;
};

// Compiles one GX stage half: out = (d +/- lerp(a, b, c) + bias) * scale into `half`'s program, and stores the
// result in GX register `outReg`.
void CompileHalf(Compiler& cc, int half, Opnd a, Opnd b, Opnd c, Opnd d, const TevOp& gxOp)
{
    const int n = half == kRgb ? 3 : 1;
    const bool subtract = gxOp.op == GX_TEV_SUB;
    if (gxOp.op != GX_TEV_ADD && gxOp.op != GX_TEV_SUB) {
        // Comparison ops: not expressible on the PICA; pass d through.
        cc.approximated = true;
        a = b = c = ConstOpnd(0, 0, 0);
    }
    float k[3] = {0, 0, 0};  // the constant part
    Term terms[4];
    int termCount = 0;
    auto addTerm = [&](Opnd x, Opnd y, bool twoFactors, bool negative) {
        if (twoFactors && x.kind == Opnd::kConst && y.kind == Opnd::kConst) {
            for (int i = 0; i < n; ++i) k[i] += (negative ? -1 : 1) * x.c[i] * y.c[i];
            return;
        }
        if (twoFactors && (IsConstValue(x, n, 0) || IsConstValue(y, n, 0))) return;
        if (twoFactors && IsConstValue(x, n, 1)) x = y, twoFactors = false;
        else if (twoFactors && IsConstValue(y, n, 1)) twoFactors = false;
        if (!twoFactors && x.kind == Opnd::kConst) {
            for (int i = 0; i < n; ++i) k[i] += (negative ? -1 : 1) * x.c[i];
            return;
        }
        Term& t = terms[termCount++];
        t.f[0] = x;
        t.f[1] = y;
        t.factors = twoFactors ? 2 : 1;
        t.negative = negative;
    };
    addTerm(d, d, false, false);
    addTerm(a, Invert(c, n), true, subtract);
    addTerm(b, c, true, subtract);
    if (gxOp.bias == GX_TB_ADDHALF) for (int i = 0; i < n; ++i) k[i] += 0.5f;
    if (gxOp.bias == GX_TB_SUBHALF) for (int i = 0; i < n; ++i) k[i] -= 0.5f;
    const float scaleValue = gxOp.scale == GX_CS_SCALE_2 ? 2.0f : gxOp.scale == GX_CS_SCALE_4 ? 4.0f
                             : gxOp.scale == GX_CS_DIVIDE_2 ? 0.5f : 1.0f;

    Val& out = cc.regs[half][std::min<int>(gxOp.outReg, 3)];
    if (termCount == 0) {  // all constant
        out.isConst = true;
        for (int i = 0; i < 3; ++i) out.c[i] = Clamp01(k[i] * scaleValue);
        return;
    }
    // A plain copy of a value: no op needed.
    const bool kZero = std::fabs(k[0]) < 1e-6f && std::fabs(k[1]) < 1e-6f && std::fabs(k[2]) < 1e-6f;
    if (termCount == 1 && kZero && scaleValue == 1.0f && !terms[0].negative && terms[0].factors == 1 &&
        terms[0].f[0].kind == Opnd::kStage && terms[0].f[0].valueHalf == half &&
        (terms[0].f[0].mod == 0)) {
        out.isConst = false;
        out.stage = terms[0].f[0].stage;
        return;
    }

    HalfProgram& p = cc.prog[half];
    const GPU_TEVSCALE picaScale = gxOp.scale == GX_CS_SCALE_2 ? GPU_TEVSCALE_2
                                   : gxOp.scale == GX_CS_SCALE_4 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_1;
    auto acc = [&]() {
        return StageOpnd(p.count - 1, half, half == kRgb ? GPU_TEVOP_RGB_SRC_COLOR : GPU_TEVOP_A_SRC_ALPHA);
    };

    // The common one-op shapes first: interpolate (a lerp with no d), multiply-add (b*c + d).
    bool done = false;
    if (kZero && termCount == 2 && !terms[0].negative && !terms[1].negative && terms[0].factors == 2 &&
        terms[1].factors == 2 && d.kind == Opnd::kConst && IsConstValue(d, n, 0)) {
        // terms are a*(1-c) and b*c: INTERPOLATE(b, a, c) = b*c + a*(1-c)
        done = Emit(cc, half, GPU_INTERPOLATE, {b, a, c}, picaScale);
    }
    if (!done) {
        // Positive terms first, then negative ones, accumulating in the previous stage's output.
        std::stable_sort(terms, terms + termCount, [](const Term& x, const Term& y) { return !x.negative && y.negative; });
        bool started = false;
        float kLeft[3] = {k[0], k[1], k[2]};
        for (int t = 0; t < termCount; ++t) {
            const Term& term = terms[t];
            if (!started) {
                if (term.negative) {
                    // Nothing positive: start from the constant (clamped at zero anyway).
                    Emit(cc, half, GPU_REPLACE, {ConstOpnd(Clamp01(kLeft[0]), Clamp01(kLeft[1]), Clamp01(kLeft[2]))});
                    kLeft[0] = kLeft[1] = kLeft[2] = 0;
                    started = true;
                } else {
                    const bool nextSingle = t + 1 < termCount && !terms[t + 1].negative && terms[t + 1].factors == 1;
                    if (term.factors == 2 && nextSingle &&
                        Emit(cc, half, GPU_MULTIPLY_ADD, {term.f[0], term.f[1], terms[t + 1].f[0]})) {
                        ++t;
                    } else if (term.factors == 1 && nextSingle &&
                               Emit(cc, half, GPU_ADD, {term.f[0], terms[t + 1].f[0]})) {
                        ++t;
                    } else if (term.factors == 2) {
                        Emit(cc, half, GPU_MODULATE, {term.f[0], term.f[1]});
                    } else {
                        Emit(cc, half, GPU_REPLACE, {term.f[0]});
                    }
                    started = true;
                    continue;
                }
            }
            if (!term.negative) {
                if (term.factors == 2) {
                    if (!Emit(cc, half, GPU_MULTIPLY_ADD, {term.f[0], term.f[1], acc()})) {
                        const Opnd before = acc();
                        Emit(cc, half, GPU_MODULATE, {term.f[0], term.f[1]});
                        Emit(cc, half, GPU_ADD, {acc(), before});
                    }
                } else {
                    Emit(cc, half, GPU_ADD, {acc(), term.f[0]});
                }
            } else {
                if (term.factors == 2) {
                    const Opnd before = acc();
                    Emit(cc, half, GPU_MODULATE, {term.f[0], term.f[1]});
                    Emit(cc, half, GPU_SUBTRACT, {before, acc()});
                } else {
                    Emit(cc, half, GPU_SUBTRACT, {acc(), term.f[0]});
                }
            }
        }
        const bool kPositive = kLeft[0] > 1e-6f || kLeft[1] > 1e-6f || kLeft[2] > 1e-6f;
        const bool kNegative = kLeft[0] < -1e-6f || kLeft[1] < -1e-6f || kLeft[2] < -1e-6f;
        if (kPositive && kNegative) cc.approximated = true;
        if (kPositive) {
            Emit(cc, half, GPU_ADD, {acc(), ConstOpnd(std::max(kLeft[0], 0.f), std::max(kLeft[1], 0.f), std::max(kLeft[2], 0.f))});
        } else if (kNegative) {
            Emit(cc, half, GPU_SUBTRACT, {acc(), ConstOpnd(std::max(-kLeft[0], 0.f), std::max(-kLeft[1], 0.f), std::max(-kLeft[2], 0.f))});
        }
        if (p.count > 0) p.ops[p.count - 1].scale = picaScale;
    }
    if (gxOp.scale == GX_CS_DIVIDE_2) Emit(cc, half, GPU_MODULATE, {acc(), ConstOpnd(0.5f, 0.5f, 0.5f)});
    out.isConst = false;
    out.stage = p.count - 1;
}

// Makes `half`'s final value (GX_TEVPREV) the output of its last op.
void Finish(Compiler& cc, int half)
{
    HalfProgram& p = cc.prog[half];
    const Val& v = cc.regs[half][0];
    if (v.isConst) {
        Emit(cc, half, GPU_REPLACE, {ConstOpnd(v.c[0], v.c[1], v.c[2])});
    } else if (v.stage != p.count - 1) {
        Emit(cc, half, GPU_REPLACE,
             {StageOpnd(v.stage, half, half == kRgb ? GPU_TEVOP_RGB_SRC_COLOR : GPU_TEVOP_A_SRC_ALPHA)});
    }
}

uint32_t PackConst(const float rgb[3], float a)
{
    auto b = [](float v) { return static_cast<uint32_t>(Clamp01(v) * 255.0f + 0.5f); };
    return b(rgb[0]) | b(rgb[1]) << 8 | b(rgb[2]) << 16 | b(a) << 24;
}

GPU_TESTFUNC TestFunc(GXCompare c)
{
    switch (c) {
    case GX_NEVER: return GPU_NEVER;
    case GX_LESS: return GPU_LESS;
    case GX_EQUAL: return GPU_EQUAL;
    case GX_LEQUAL: return GPU_LEQUAL;
    case GX_GREATER: return GPU_GREATER;
    case GX_NEQUAL: return GPU_NOTEQUAL;
    case GX_GEQUAL: return GPU_GEQUAL;
    default: return GPU_ALWAYS;
    }
}

GPU_BLENDFACTOR SrcFactor(GXBlendFactor f)
{
    switch (f) {
    case GX_BL_ZERO: return GPU_ZERO;
    case GX_BL_ONE: return GPU_ONE;
    case GX_BL_DSTCLR: return GPU_DST_COLOR;  // GX_BL_SRCCLR as a source factor means the destination color
    case GX_BL_INVDSTCLR: return GPU_ONE_MINUS_DST_COLOR;
    case GX_BL_SRCALPHA: return GPU_SRC_ALPHA;
    case GX_BL_INVSRCALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
    case GX_BL_DSTALPHA: return GPU_DST_ALPHA;
    default: return GPU_ONE_MINUS_DST_ALPHA;
    }
}
GPU_BLENDFACTOR DstFactor(GXBlendFactor f)
{
    switch (f) {
    case GX_BL_ZERO: return GPU_ZERO;
    case GX_BL_ONE: return GPU_ONE;
    case GX_BL_SRCCLR: return GPU_SRC_COLOR;
    case GX_BL_INVSRCCLR: return GPU_ONE_MINUS_SRC_COLOR;
    case GX_BL_SRCALPHA: return GPU_SRC_ALPHA;
    case GX_BL_INVSRCALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
    case GX_BL_DSTALPHA: return GPU_DST_ALPHA;
    default: return GPU_ONE_MINUS_DST_ALPHA;
    }
}
GPU_LOGICOP LogicOp(GXLogicOp op)
{
    static const GPU_LOGICOP kMap[16] = {
        GPU_LOGICOP_CLEAR, GPU_LOGICOP_AND, GPU_LOGICOP_AND_REVERSE, GPU_LOGICOP_COPY,
        GPU_LOGICOP_AND_INVERTED, GPU_LOGICOP_NOOP, GPU_LOGICOP_XOR, GPU_LOGICOP_OR,
        GPU_LOGICOP_NOR, GPU_LOGICOP_EQUIV, GPU_LOGICOP_INVERT, GPU_LOGICOP_OR_REVERSE,
        GPU_LOGICOP_COPY_INVERTED, GPU_LOGICOP_OR_INVERTED, GPU_LOGICOP_NAND, GPU_LOGICOP_SET,
    };
    return kMap[static_cast<int>(op) & 15];
}

// The fragment state last given to citro3d: setting it marks it dirty and re-emits it, so it is set only when it
// changes (the command buffer overflowed with everything re-sent for ~555 batches a frame).
struct FragmentState {
    bool alphaTest;
    int alphaFunc, alphaRef;
    int blendKind;  // 0 replace, 1 blend, 2 subtract, 3 logic
    int src, dst, logicOp;
    int depthFunc, writeMask, cull;
    bool operator==(const FragmentState& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
FragmentState g_lastFragment;
bool g_haveLastFragment = false;
C3D_TexEnv g_lastEnv[kMaxPicaStages];
bool g_haveLastEnv[kMaxPicaStages] = {};
int g_lastBufMask[2] = {-1, -1};

void ApplyPixelState(MaterialPlan& plan)
{
    FragmentState fs;
    std::memset(&fs, 0, sizeof(fs));
    // Alpha compare: the PICA has one test; GX has two joined by an op. The usual cases reduce to one.
    const auto& ac = g_gxState.alphaCompare;
    GXCompare comp = ac.comp0;
    u32 ref = ac.ref0;
    if (ac.op == GX_AOP_AND) {
        if (ac.comp0 == GX_ALWAYS) comp = ac.comp1, ref = ac.ref1;
    } else if (ac.op == GX_AOP_OR) {
        if (ac.comp0 == GX_NEVER) comp = ac.comp1, ref = ac.ref1;
        else if (ac.comp1 == GX_ALWAYS) comp = GX_ALWAYS;
    }
    fs.alphaTest = comp != GX_ALWAYS;
    fs.alphaFunc = TestFunc(comp);
    fs.alphaRef = static_cast<int>(ref & 0xFF);
    switch (g_gxState.blendMode) {
    case GX_BM_BLEND:
        fs.blendKind = 1;
        fs.src = SrcFactor(g_gxState.blendFacSrc);
        fs.dst = DstFactor(g_gxState.blendFacDst);
        break;
    case GX_BM_SUBTRACT: fs.blendKind = 2; break;
    case GX_BM_LOGIC:
        fs.blendKind = 3;
        fs.logicOp = LogicOp(g_gxState.blendOp);
        break;
    default: fs.blendKind = 0; break;
    }

    // GX depth (with this renderer's depth map, see ApplyFixedState) compares like the PICA's. With the compare
    // off GX neither tests nor updates.
    int writeMask = 0;
    if (g_gxState.colorUpdate) writeMask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if (g_gxState.alphaUpdate) writeMask |= GPU_WRITE_ALPHA;
    if (g_gxState.depthCompare && g_gxState.depthUpdate) writeMask |= GPU_WRITE_DEPTH;
    fs.depthFunc = g_gxState.depthCompare ? TestFunc(g_gxState.depthFunc) : GPU_ALWAYS;
    fs.writeMask = writeMask;

    // GX front faces are clockwise on screen; after the projection (y up) they are counter-clockwise.
    switch (g_gxState.cullMode) {
    case GX_CULL_BACK: fs.cull = GPU_CULL_BACK_CCW; break;
    case GX_CULL_FRONT: fs.cull = GPU_CULL_FRONT_CCW; break;
    case GX_CULL_ALL:
        fs.cull = GPU_CULL_NONE;
        plan.skip = true;
        ++g_statCullAll;
        break;
    default: fs.cull = GPU_CULL_NONE; break;
    }

    const FragmentState& last = g_lastFragment;
    const bool all = !g_haveLastFragment;
    if (all || fs.alphaTest != last.alphaTest || fs.alphaFunc != last.alphaFunc || fs.alphaRef != last.alphaRef)
        C3D_AlphaTest(fs.alphaTest, static_cast<GPU_TESTFUNC>(fs.alphaFunc), fs.alphaRef);
    if (all || fs.blendKind != last.blendKind || fs.src != last.src || fs.dst != last.dst || fs.logicOp != last.logicOp) {
        switch (fs.blendKind) {
        case 1:
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, static_cast<GPU_BLENDFACTOR>(fs.src),
                           static_cast<GPU_BLENDFACTOR>(fs.dst), static_cast<GPU_BLENDFACTOR>(fs.src),
                           static_cast<GPU_BLENDFACTOR>(fs.dst));
            break;
        case 2:
            C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
            break;
        case 3: C3D_ColorLogicOp(static_cast<GPU_LOGICOP>(fs.logicOp)); break;
        default: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); break;
        }
    }
    if (all || fs.depthFunc != last.depthFunc || fs.writeMask != last.writeMask)
        C3D_DepthTest(true, static_cast<GPU_TESTFUNC>(fs.depthFunc), static_cast<GPU_WRITEMASK>(fs.writeMask));
    if (all || fs.cull != last.cull) C3D_CullFace(static_cast<GPU_CULLMODE>(fs.cull));
    g_lastFragment = fs;
    g_haveLastFragment = true;
}
} // namespace

void ApplyMaterial(MaterialPlan& plan)
{
    plan = {};
    Compiler cc;
    for (int r = 0; r < 4; ++r) {
        const auto& reg = g_gxState.colorRegs[r];
        for (int i = 0; i < 3; ++i) cc.regs[kRgb][r].c[i] = Clamp01(reg[i]);
        cc.regs[kAlpha][r].c[0] = Clamp01(reg[3]);
    }

    const int stages = std::min<int>(g_gxState.numTevStages, MaxTevStages);
    for (int s = 0; s < stages; ++s) {
        const TevStage& st = g_gxState.tevStages[s];
        StageContext sc;
        const auto usesTex = [&] {
            const auto& c = st.colorPass;
            const auto& a = st.alphaPass;
            auto ct = [](GXTevColorArg x) { return x == GX_CC_TEXC || x == GX_CC_TEXA; };
            return ct(c.a) || ct(c.b) || ct(c.c) || ct(c.d) || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
                   a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
        }();
        const int texMap = static_cast<int>(st.texMapId);
        if (usesTex && texMap >= 0 && texMap < static_cast<int>(MaxTextures) && st.texCoordId != GX_TEXCOORD_NULL) {
            for (int u = 0; u < plan.unitCount; ++u)
                if (plan.units[u].texMap == texMap && plan.units[u].texCoord == st.texCoordId) sc.unit = u;
            if (sc.unit < 0) {
                if (plan.unitCount < kMaxUnits) {
                    sc.unit = plan.unitCount++;
                    plan.units[sc.unit].texMap = static_cast<uint8_t>(texMap);
                    plan.units[sc.unit].texCoord = static_cast<uint8_t>(st.texCoordId);
                } else {
                    sc.unit = 0;
                    ++g_statUnitOverflow;
                    cc.approximated = true;
                }
            }
        }
        sc.rasZero = st.channelId != GX_COLOR0A0 && st.channelId != GX_COLOR1A1;
        sc.rasSwap = g_gxState.tevSwapTable[st.tevSwapRas & 3];
        sc.texSwap = g_gxState.tevSwapTable[st.tevSwapTex & 3];

        const auto& cp = st.colorPass;
        const auto& ap = st.alphaPass;
        // Both halves read the registers as they were before this stage.
        const Opnd ca = ColorArg(cc, st, sc, cp.a), cb = ColorArg(cc, st, sc, cp.b), ccc = ColorArg(cc, st, sc, cp.c),
                   cd = ColorArg(cc, st, sc, cp.d);
        const Opnd aa = AlphaArg(cc, st, sc, ap.a), ab = AlphaArg(cc, st, sc, ap.b), ac = AlphaArg(cc, st, sc, ap.c),
                   ad = AlphaArg(cc, st, sc, ap.d);
        CompileHalf(cc, kRgb, ca, cb, ccc, cd, st.colorOp);
        CompileHalf(cc, kAlpha, aa, ab, ac, ad, st.alphaOp);
    }
    Finish(cc, kRgb);
    Finish(cc, kAlpha);

    // Zip the two halves into PICA stages.
    int total = std::max(cc.prog[kRgb].count, cc.prog[kAlpha].count);
    if (total > kMaxPicaStages) {
        ++g_statTruncated;
        cc.approximated = true;
        total = kMaxPicaStages;
    }
    for (int i = 0; i < kMaxPicaStages; ++i) {
        C3D_TexEnv envStorage;
        C3D_TexEnv* env = &envStorage;
        C3D_TexEnvInit(env);
        // Given to citro3d (which marks it dirty and re-emits it) only when it differs from the last one.
        struct Commit {
            int i;
            C3D_TexEnv* env;
            ~Commit()
            {
                if (!g_haveLastEnv[i] || std::memcmp(&g_lastEnv[i], env, sizeof(C3D_TexEnv)) != 0) {
                    C3D_SetTexEnv(i, env);
                    g_lastEnv[i] = *env;
                    g_haveLastEnv[i] = true;
                }
            }
        } commit{i, env};
        if (i >= total) continue;  // REPLACE(PREVIOUS)
        float rgb[3] = {1, 1, 1};
        float alpha = 1;
        for (int h = 0; h < 2; ++h) {
            const HalfProgram& p = cc.prog[h];
            const C3D_TexEnvMode mode = h == kRgb ? C3D_RGB : C3D_Alpha;
            if (i >= p.count) continue;  // this half is done: REPLACE(PREVIOUS)
            const PicaOp& op = p.ops[i];
            C3D_TexEnvSrc(env, mode, static_cast<GPU_TEVSRC>(op.src[0]), static_cast<GPU_TEVSRC>(op.src[1]),
                          static_cast<GPU_TEVSRC>(op.src[2]));
            if (h == kRgb) {
                C3D_TexEnvOpRgb(env, static_cast<GPU_TEVOP_RGB>(op.mod[0]), static_cast<GPU_TEVOP_RGB>(op.mod[1]),
                                static_cast<GPU_TEVOP_RGB>(op.mod[2]));
                if (op.hasConst) std::memcpy(rgb, op.c, sizeof(rgb));
            } else {
                C3D_TexEnvOpAlpha(env, static_cast<GPU_TEVOP_A>(op.mod[0]), static_cast<GPU_TEVOP_A>(op.mod[1]),
                                  static_cast<GPU_TEVOP_A>(op.mod[2]));
                if (op.hasConst) alpha = op.c[0];
            }
            C3D_TexEnvFunc(env, mode, op.func);
            C3D_TexEnvScale(env, mode, op.scale);
        }
        C3D_TexEnvColor(env, PackConst(rgb, alpha));
    }
    if (g_lastBufMask[kRgb] != (cc.bufferMask[kRgb] & 0xF) || g_lastBufMask[kAlpha] != (cc.bufferMask[kAlpha] & 0xF)) {
        C3D_TexEnvBufUpdate(C3D_RGB, cc.bufferMask[kRgb] & 0xF);
        C3D_TexEnvBufUpdate(C3D_Alpha, cc.bufferMask[kAlpha] & 0xF);
        g_lastBufMask[kRgb] = cc.bufferMask[kRgb] & 0xF;
        g_lastBufMask[kAlpha] = cc.bufferMask[kAlpha] & 0xF;
    }

    // The texture coordinates the units read: their GX texgen sources among TEX0..TEX7, at most two.
    for (int u = 0; u < plan.unitCount; ++u) {
        const auto& tcg = g_gxState.tcgs[plan.units[u].texCoord & 7];
        const int src = static_cast<int>(tcg.src);
        if (src >= GX_TG_TEX0 && src <= GX_TG_TEX0 + 7) {
            const int attr = GX_VA_TEX0 + (src - GX_TG_TEX0);
            if (plan.uvAttr[0] == attr || plan.uvAttr[1] == attr) continue;
            if (plan.uvAttr[0] < 0) plan.uvAttr[0] = attr;
            else if (plan.uvAttr[1] < 0) plan.uvAttr[1] = attr;
            else cc.approximated = true;
        }
    }

    ApplyPixelState(plan);

    ++g_statMaterials;
    g_statStagesSum += static_cast<uint64_t>(total);
    g_statGxStagesSum += static_cast<uint64_t>(stages);
    g_statMaxStages = std::max(g_statMaxStages, std::max(cc.prog[kRgb].count, cc.prog[kAlpha].count));
    if (cc.approximated) ++g_statApprox;
}

void WriteTevStats(std::FILE* f, unsigned frames)
{
    const double div = frames ? frames : 1;
    const double mats = g_statMaterials ? static_cast<double>(g_statMaterials) : 1.0;
    if (f) std::fprintf(f,
                 "materials: %.0f per frame, %.2f GX TEV stages -> %.2f PICA stages on average (most %d); "
                 "%.1f%% approximated, %llu over 6 stages, %llu over 3 textures, %llu cull-all\n",
                 g_statMaterials / div, g_statGxStagesSum / mats, g_statStagesSum / mats, g_statMaxStages,
                 100.0 * g_statApprox / mats, static_cast<unsigned long long>(g_statTruncated),
                 static_cast<unsigned long long>(g_statUnitOverflow), static_cast<unsigned long long>(g_statCullAll));
    g_statMaterials = g_statApprox = g_statTruncated = g_statStagesSum = g_statGxStagesSum = 0;
    g_statUnitOverflow = g_statCullAll = 0;
    g_statMaxStages = 0;
}

} // namespace brawl3ds_pica
