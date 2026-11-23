#pragma once
// Shared between the PICA200 renderer's parts (renderer milestone 3):
//   pica_renderer_3ds.cpp  frame, vertex conversion, shader uniforms, draws
//   pica_texture_3ds.cpp   GX textures decoded and cached as PICA textures
//   pica_tev_3ds.cpp       GX TEV stages and pixel state translated to the PICA combiners and fragment state

#include "gx/gx.hpp"

#include <cstdint>
#include <cstdio>

#include <citro3d.h>

namespace brawl3ds_pica {

constexpr int kMaxUnits = 3;  // PICA texture units 0-2, each with its own texture coordinate output

// Testing switches (sdmc:/3ds/brawl3ds/debug_flags.txt, read by main_3ds.cpp), added up: 1 = fog off, 16 = fog only
// through the PICA's fog table (not per vertex), 2 = the second texture-coordinate set reads as the first, 4 =
// coordinates from normals read as the first set.
extern unsigned g_debugFlags;

// What one batch samples: up to three (GX texture map, GX texture coordinate) pairs, one per PICA unit.
struct UnitBinding {
    uint8_t texMap = 0xFF;    // GXTexMapID, 0xFF = unit unused
    uint8_t texCoord = 0xFF;  // GXTexCoordID
};

struct MaterialPlan {
    UnitBinding units[kMaxUnits];
    int unitCount = 0;
    // The GX texture-coordinate sources the vertex buffer must carry (GX_VA_TEX0..TEX7), at most two (set A and
    // set B); per unit, which set its texgen reads (0 = A, 1 = B; GX_TG_POS / GX_TG_NRM sources ignore it).
    int uvAttr[2] = {-1, -1};
    uint8_t unitSet[kMaxUnits] = {0, 0, 0};
    bool needsNormals = false;  // a unit generates coordinates from the normal (GX_TG_NRM)
    bool skip = false;  // GX_CULL_ALL: nothing to draw
    bool approximated = false;  // the combiners only approximate the GX TEV setup
    int picaStages = 0;
    bool fog = false;           // GX fog is on for this batch
    bool vertexFog = false;     // ... computed per vertex: unit 2 samples the fog ramp, the last stage blends
};

// pica_tev_3ds.cpp: translate the current GX state (TEV, texture orders, alpha compare, blend, depth, cull) into
// citro3d state for the next batch, and say which textures and texture coordinates it uses.
void ApplyMaterial(MaterialPlan& plan);
void WriteTevStats(std::FILE* f, unsigned frames);
// The fixed-function state of an EFB copy's clear (a quad of the vertex color): combiners pass the vertex color,
// no alpha test, blending or fog, depth always passes; `color`, `alpha` and `depth` pick what it writes. The next
// ApplyMaterial sends its whole state again.
void ApplyClearState(bool color, bool alpha, bool depth);
// GX's fog amount (0..1) at screen depth `d` with the current fog state (for the debug dump).
float FogAmountAt(float d);

// pica_renderer_3ds.cpp: citro3d state as pica_tev_3ds.cpp sets it, recorded on the game thread and applied by the
// GPU worker on core 2 (or at once, without the worker). Only changes are recorded (the callers keep what they
// last set). Blend kinds: 0 replace, 1 blend (src/dst factors), 2 subtract, 3 logic op.
void RecTexEnv(int stage, const C3D_TexEnv& env);
void RecTexEnvBufUpdate(int rgbMask, int alphaMask);
void RecAlphaTest(bool on, GPU_TESTFUNC func, int ref);
void RecBlend(int kind, int src, int dst, int logicOp);
void RecDepthTest(GPU_TESTFUNC func, int writeMask);
void RecCull(GPU_CULLMODE mode);
void RecFogMode(bool on);
void RecFogColor(uint32_t color);
void RecFogLut(const C3D_FogLut& lut);

// pica_texture_3ds.cpp (GPU worker only): bind GX texture object `obj` (with its palette, if paletted) to PICA unit
// `unit`; false when it has no usable texture (the unit then samples a 8x8 white texture). Textures are stretched
// to their power-of-two size, so GX coordinates apply unchanged.
bool BindTextureObj(int unit, const GXTexObj_& obj, const GXTlutObj_* tlut);
void TexturesBeginFrame();
// Per-vertex fog: the 8x8 alpha ramp on `unit`; false when it could not be made.
bool BindFogRamp(int unit);
void WriteTextureStats(std::FILE* f, unsigned frames);

} // namespace brawl3ds_pica
