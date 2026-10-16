// SPDX-License-Identifier: MIT
// The Xbox's pixel shaders on GX's texture environment (psh.c): the NV2A register combiners an
// Xbox (Direct3D 8) pixel shader sets become a TEV program (wl/platform/tev.h).
//
// The combiners are given as the words of the Xbox's pixel shader render states, in their order
// (WL_XBOX_PSH_*: the alpha inputs of the eight stages, the final combiner's inputs, the stages'
// constants, ...), with what the caller's device knows of the textures (WlXboxPshTextures). The
// device draws the result: it binds the textures to the maps the stages name, makes the vertices'
// texture coordinates and colours the program reads (texcoords, channels, vertices), sets GX's
// indirect unit for bumped lookups (indirect) and applies the program (wl_xbox_psh_apply).
//
// A program the caller recognizes can be made by hand instead (WlXboxPshRecipe: a game's own
// shaders, where an exact program needs what the general translation cannot see).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/platform/tev.h"

// The combiner state: words in the order of the Xbox's pixel shader render states (each a
// register of the NV2A's combiners). Words 54 to 56 (texture modes, dot mapping, input texture)
// are not read: WlXboxPshTextures says what the translation needs of them.
enum {
    WL_XBOX_PSH_ALPHA_INPUTS0 = 0,     // eight: each stage's alpha inputs A, B, C, D
    WL_XBOX_PSH_FINAL_INPUTS_ABCD = 8, // the final combiner's A, B, C, D
    WL_XBOX_PSH_FINAL_INPUTS_EFG = 9,  // its E, F, G and flags
    WL_XBOX_PSH_CONSTANT0_0 = 10,      // eight: each stage's c0 (ARGB)
    WL_XBOX_PSH_CONSTANT1_0 = 18,      // eight: each stage's c1
    WL_XBOX_PSH_ALPHA_OUTPUTS0 = 26,   // eight: each stage's alpha outputs
    WL_XBOX_PSH_RGB_INPUTS0 = 34,      // eight: each stage's colour inputs
    WL_XBOX_PSH_COMPARE_MODE = 42,
    WL_XBOX_PSH_FINAL_CONSTANT0 = 43, // the final combiner's c0
    WL_XBOX_PSH_FINAL_CONSTANT1 = 44, // its c1
    WL_XBOX_PSH_RGB_OUTPUTS0 = 45,    // eight: each stage's colour outputs
    WL_XBOX_PSH_COMBINER_COUNT = 53,  // the stage count, and the per-stage constants' flags
    WL_XBOX_PSH_WORDS = 57
};

enum {
    WL_XBOX_PSH_MAX_STAGES = 16,
    WL_XBOX_PSH_TEXTURES = 4,
    /* the fog ramp's texture map, as the translation names it: GX's map 7,
    read at texture coordinate 4 (the vertex's fog factor) */
    WL_XBOX_PSH_FOG_MAP = 8,
    WL_XBOX_PSH_FOG_TEXMAP = 7,
    WL_XBOX_PSH_FOG_TEXCOORD = 4,
    /* a bumped lookup's offset (WlXboxPshTextures.bumped): the bump map read by
    GX's indirect unit as map 5 (stage 0's texels through a TLUT of its
    normals' x and y), at texture coordinate 0, and the two terms of the
    offset, the sphere map's coordinates moved by the normal's x and by its
    y, at texture coordinates 1 and 2 */
    WL_XBOX_PSH_BUMP_TEXMAP = 5,
    WL_XBOX_PSH_BUMP_TEXCOORD_S = 1,
    WL_XBOX_PSH_BUMP_TEXCOORD_T = 2,
    /* D0's alpha from a texture (WlXboxPshTextures.d0_alpha_map): the map's
    alpha, read at texture coordinate 5; as the translation names it, the
    map plus WL_XBOX_PSH_D0_ALPHA_MAP */
    WL_XBOX_PSH_D0_ALPHA_TEXCOORD = 5,
    WL_XBOX_PSH_D0_ALPHA_MAP = 16
};

/* a stage's part in a bumped lookup (WlXboxPshProgram.indirect): GX's
environment-mapped bump (GX_SetTevIndBumpST), the coordinate of each lookup
made over the two stages before it */
enum {
    WL_XBOX_PSH_INDIRECT_NONE,
    /* texture coordinate 1 times the bump's x (no texture read) */
    WL_XBOX_PSH_INDIRECT_S,
    /* plus texture coordinate 2 times the bump's y (no texture read) */
    WL_XBOX_PSH_INDIRECT_T,
    /* plus the stage's own coordinate: the lookup */
    WL_XBOX_PSH_INDIRECT_LOOKUP,
    /* the stage before's coordinate again: a lookup right after one */
    WL_XBOX_PSH_INDIRECT_REPEAT
};

/* what the device puts in the vertices for a program (WlXboxPshProgram.vertices) */
enum {
    /* what the vertex program made */
    WL_XBOX_PSH_VERTICES_PROGRAM,
    /* a bumped reflection (DOT_RFLCT_DIFF and DOT_RFLCT_SPEC): texture
    coordinates 1 and 2 the bump's two terms, 3 the sphere map's coordinates
    of the reflection of the flat normal, and colour 1 that normal, (n + 1) / 2
    (all from texture coordinates 1 to 3: the tangent frame's rows, the eye in
    their w); a flat one's (WlXboxPshTextures.reflected) the same */
    WL_XBOX_PSH_VERTICES_BUMPED_REFLECTION,
    /* the first of a recipe's own (WlXboxPshRecipe): what they mean is the
    caller's */
    WL_XBOX_PSH_VERTICES_RECIPE
};

/* what a texture stage's texture holds */
enum {
    /* all four channels */
    WL_XBOX_PSH_TEXTURE_ALL,
    /* colour only: its alpha reads as 1 (luminance) */
    WL_XBOX_PSH_TEXTURE_COLOR,
    /* alpha only: its colour reads as 1 */
    WL_XBOX_PSH_TEXTURE_ALPHA
};

/* what the device knows of the textures. Cleared before it is filled: the
cache (psh_cache.h) compares it whole, padding and all. */
typedef struct WlXboxPshTextures {
    /* the stages that sample a texture (bound, in a 2D mode) */
    unsigned present;
    /* the stages whose alpha is in the texture map four after (DXT3, DXT5) */
    unsigned separate_alpha;
    unsigned char contents[WL_XBOX_PSH_TEXTURES];
    /* the stages whose texture GX cannot sample (a cube map, a dependent or
    dot product mode): read as 0, but a dot product with one is taken as 1
    (a bumped reflection's lookups, unlit) */
    unsigned unavailable;
    /* the stages whose cube map is sampled as a sphere map, as any other
    texture (a normalization cube map's direction, for a dot product with a
    bump map, among them) */
    unsigned cube;
    /* the stages whose lookup GX's indirect unit makes, offset by stage 0's
    bump map (a bumped reflection's DOT_RFLCT_SPEC: its cube map's sphere map,
    at the flat normal's reflection moved by the bump; read as any texture) */
    unsigned bumped;
    /* the stages read as colour 1, which the device makes what the stage's
    lookup would give of the flat normal (DOT_RFLCT_DIFF's normal) */
    unsigned vertex_color;
    /* the stages whose lookup (a DOT_RFLCT_SPEC whose bump GX cannot
    follow: of a bump map drawn each frame) is the cube map's sphere map at
    the flat normal's reflection, its coordinates the device's (texture
    coordinate 3, as a bumped reflection's): read as any texture */
    unsigned reflected;
    /* fog (the fog render state on, its ramp bound) and its colour (RGB) */
    unsigned char fog;
    uint32_t fog_color;
    /* D0's alpha read from this texture map (4 to 7, one the stages leave
    free) rather than the rasterized colour, 0 as it is: a fog the device
    makes of the position, say */
    unsigned char d0_alpha_map;
} WlXboxPshTextures;

typedef struct WlXboxPshProgram {
    /* the stages, konst colours, preloaded registers and swap tables
    (wl/platform/tev.h) */
    WlTevProgram tev;
    /* what the stages read of the vertices: the texture coordinates (bit n:
    GX_TEXCOORDn) and the colour channels (bit 0 COLOR0A0, bit 1 COLOR1A1) */
    unsigned char texcoords, channels;
    /* each stage's part in a bumped lookup (WL_XBOX_PSH_INDIRECT_*: the device
    sets GX's indirect unit by it), and what the device puts in the vertices
    (WL_XBOX_PSH_VERTICES_*) */
    unsigned char indirect[WL_XBOX_PSH_MAX_STAGES];
    unsigned char vertices;
    /* why it was not translated */
    const char *reason;
    /* something approximated: more constants than GX holds, a dot product
    or mapping that did not fit */
    bool approximate;
    const char *approximation;
    /* a value worked out from two constants (a dot product's, or a constant's
    channels summed) and held as such: the program changes with its constants
    beyond the konst colours that hold them (psh_cache.h) */
    bool folded;
} WlXboxPshProgram;

/* A program made by hand for combiners the caller recognizes, tried before
the translation: true when make() made *program (from nothing: it clears it
first), false to translate the combiners. The translation then sets the
program's texcoords and channels from its stages; the rest (vertices,
indirect, approximate) is the recipe's. */
typedef struct WlXboxPshRecipe {
    bool (*make)(const uint32_t *state, const WlXboxPshTextures *textures,
                 WlXboxPshProgram *program, void *user);
    void *user;
} WlXboxPshRecipe;

/* the program for the combiner state (WL_XBOX_PSH_WORDS words) and the
textures, through the recipe first when there is one (NULL: none). false
when the combiners need more than GX has (the program is then not usable;
its reason says why). */
bool wl_xbox_psh_translate(const uint32_t *state, const WlXboxPshTextures *textures,
                           const WlXboxPshRecipe *recipe, WlXboxPshProgram *program);
/* The same, by the way (*way) a translation of the same combiners and textures took before.
A translation tries the combiners several ways and takes the first exact program, else the
first approximated one; which way that is turns on the combiners, the textures and which
constants are 0, 1 or equal, not on the constants' other values (except a program folded,
which a caller does not ask this of). *way < 0: the translation tries every way, as
wl_xbox_psh_translate, and sets *way to the one taken; else that way alone is tried, and every
way only when it is not usable or not as exact as before (*way is then set again). */
bool wl_xbox_psh_translate_by_way(const uint32_t *state, const WlXboxPshTextures *textures,
                                  const WlXboxPshRecipe *recipe, WlXboxPshProgram *program,
                                  int *way);
/* the stages to GX (their swap tables, konst colours and preloads too),
without WiiLibs' shadow of GX's state (wl_tev_apply_unchecked) */
void wl_xbox_psh_apply(const WlXboxPshProgram *program);
