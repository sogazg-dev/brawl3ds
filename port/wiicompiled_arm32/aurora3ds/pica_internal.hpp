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

// What one batch samples: up to three (GX texture map, GX texture coordinate) pairs, one per PICA unit.
struct UnitBinding {
    uint8_t texMap = 0xFF;    // GXTexMapID, 0xFF = unit unused
    uint8_t texCoord = 0xFF;  // GXTexCoordID
};

struct MaterialPlan {
    UnitBinding units[kMaxUnits];
    int unitCount = 0;
    // The GX texture-coordinate sources the vertex buffer must carry (GX_VA_TEX0..TEX7), at most two.
    int uvAttr[2] = {-1, -1};
    bool skip = false;  // GX_CULL_ALL: nothing to draw
};

// pica_tev_3ds.cpp: translate the current GX state (TEV, texture orders, alpha compare, blend, depth, cull) into
// citro3d state for the next batch, and say which textures and texture coordinates it uses.
void ApplyMaterial(MaterialPlan& plan);
void WriteTevStats(std::FILE* f, unsigned frames);

// pica_texture_3ds.cpp: bind GX texture map `texMap` to PICA unit `unit`; false when it has no usable texture
// (the unit then samples a 8x8 white texture). `scaleS/T` receive the factors that map GX texture coordinates
// onto the power-of-two PICA texture (1 unless the GX texture had to be padded).
bool BindTexture(int unit, int texMap, float& scaleS, float& scaleT);
void TexturesBeginFrame();
void WriteTextureStats(std::FILE* f, unsigned frames);

} // namespace brawl3ds_pica
