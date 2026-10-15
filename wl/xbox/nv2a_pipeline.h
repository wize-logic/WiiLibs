// SPDX-License-Identifier: MIT
// What every NV2A backend does on the processor (nv2a_pipeline.c), shared by the GX backend and the
// software rasterizer so that both read the state the same way: the vertex program compiled (and
// kept, by its words), each vertex run through it (or through the fixed function's composite
// matrix when the title uses that), the fog factor of the NV2A's fog modes, and what psh needs to
// know of the bound textures.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/platform/gx_direct.h"
#include "wl/xbox/nv2a_kelvin.h"
#include "wl/xbox/nv2a_texcache.h"
#include "wl/xbox/psh.h"
#include "wl/xbox/vsh.h"

enum { WL_NV2A_PIPELINE_PROGRAMS = 16 };

typedef struct WlNv2aPipeline {
    // vertex programs compiled ahead of time (tools/xbox_vsh_codegen.c), or NULL
    const WlXboxVshNative *natives;
    unsigned long native_count;
    // the programs kept: by their words' hash and count, the least recently used replaced
    struct {
        unsigned long hash, count;
        uint32_t used;
        WlXboxVshProgram program;
    } programs[WL_NV2A_PIPELINE_PROGRAMS];
    uint32_t use;
    // the draw's (wl_nv2a_pipeline_begin)
    const WlXboxVshProgram *program; // NULL in the fixed-function mode
    uint32_t program_generation;
    float point_size;
    uint32_t compiled, found;
    // called for each program decoded (again when one evicted comes back), with whether it was among
    // the natives: a title's programs listed for compiling ahead of time (NULL: none)
    void (*decoded)(void *user, const uint32_t *words, unsigned long count, bool native);
    void *decoded_user;
    // the last answer of wl_nv2a_pipeline_inputs: for this program (and compile count) and live set
    const WlXboxVshProgram *inputs_program;
    uint32_t inputs_compiled, inputs;
    unsigned inputs_live;
} WlNv2aPipeline;

void wl_nv2a_pipeline_init(WlNv2aPipeline *p, const WlXboxVshNative *natives, unsigned long native_count);
// Ready for a draw with the state as it is: the program found or compiled. false when nothing can
// be run (a program mode with no instructions).
bool wl_nv2a_pipeline_begin(WlNv2aPipeline *p, const WlNv2aKelvin *k);
// A vertex through the program (the outputs in `live`, WL_XBOX_VSH_LIVE_*), or through the fixed
// function: the position by the composite matrix (which holds the viewport) and divided by w,
// the colours and texture coordinates as the vertex gives them (a texture matrix when enabled), w
// 1. `out->clip_captured` says whether out->clip is the clip-space position.
void wl_nv2a_pipeline_vertex(WlNv2aPipeline *p, const WlNv2aKelvin *k, const float inputs[WL_NV2A_ATTRIBS][4],
                             unsigned live, WlXboxVshOutput *out);
// The same for `count` vertices (inputs[n] into out[n]), the program's interpreted together
// (wl_xbox_vsh_run_batch, with `work` its scratch): the same results, to the bit.
void wl_nv2a_pipeline_vertices(WlNv2aPipeline *p, const WlNv2aKelvin *k, const float (*inputs)[WL_NV2A_ATTRIBS][4],
                               unsigned count, unsigned live, WlXboxVshOutput *out, WlXboxVshWork *work);
// The input registers the outputs in `live` read (bit n: v[n]): the others need not be fetched.
uint32_t wl_nv2a_pipeline_inputs(WlNv2aPipeline *p, unsigned live);

// The fog factor (1 no fog, 0 all fog) of a fog coordinate (the program's oFog) by SET_FOG_MODE
// and SET_FOG_PARAMS (bias, multiplier): linear bias - 1 + multiplier c, exp bias - 1.5 +
// 2^(16 multiplier c), exp2 bias - 1.5 + 2^(-32 (multiplier c)^2), the ABS modes of |c|; clamped
// to 0..1 (the parameters as nxdk_pgraph_tests sets them for Direct3D's fog).
float wl_nv2a_fog_factor(const WlNv2aKelvin *k, float coordinate);

// What psh needs of the textures: the stages whose program samples a texture GX can (a 2D or
// projected lookup of a converted texture, a cube map's sphere map), their contents and separate
// alphas, the stages that sample what GX cannot (read as 0), and the fog. `entries[stage]` the
// converted texture of each stage (NULL: none).
void wl_nv2a_psh_textures(const WlNv2aKelvin *k, const WlNv2aTexEntry *const entries[WL_NV2A_TEXTURES],
                          WlXboxPshTextures *out);
// the stages psh's program reads a texture of and their GX coordinates: a stage's texture
// coordinate after the transform for its texture (a linear texture's in texels made a fraction;
// a cube map's direction made its sphere map's place; a projected lookup divided by q)
void wl_nv2a_texcoord(const WlNv2aTexEntry *entry, unsigned stage_program, const float t[4], float st[2]);

// ---- the raster state as GX takes it (the GX backend and the software reference alike) ----------
// (nv2a_raster.c: the functions of values alone need nothing else of wl_xbox; an Xbox Direct3D
// device's render states are these values, D3DCMP_*, D3DBLEND_*, D3DBLENDOP_*, D3DCULL_*)
// a compare function (the NV2A's are OpenGL's 0x200..0x207, in GX's order)
WlGxCompare wl_nv2a_gx_compare(uint32_t func);
// GX's blending for the NV2A's: true when exact (GX has no constant colour, no MIN or MAX, no
// source minus destination, and its colour factors are the other side's)
bool wl_nv2a_gx_blend(const WlNv2aKelvin *k, WlGxBlendMode *mode, WlGxBlendFactor *source,
                      WlGxBlendFactor *destination);
// the same of the values: SET_BLEND_ENABLE, _EQUATION, _FUNC_SFACTOR and _FUNC_DFACTOR's
bool wl_nv2a_gx_blend_of(bool enable, uint32_t equation, uint32_t sfactor, uint32_t dfactor, WlGxBlendMode *mode,
                         WlGxBlendFactor *source, WlGxBlendFactor *destination);
// and with SET_BLEND_COLOR's ARGB (D3DRS_BLENDCOLOR), which the constant factors read: ONE or ZERO, exact
// where the constant gives 1 or 0 in every channel (without it, a constant factor is ONE, approximated)
bool wl_nv2a_gx_blend_color_of(bool enable, uint32_t equation, uint32_t sfactor, uint32_t dfactor, uint32_t color,
                               WlGxBlendMode *mode, WlGxBlendFactor *source, WlGxBlendFactor *destination);
// what culling discards, by the NV2A's rule: the front face is the winding SET_FRONT_FACE names
// as the screen shows it, its window coordinates running down (CW: clockwise on the screen, a
// positive area in its y-down coordinates; Direct3D's D3DCULL_CCW sets front CW and culls the
// back, so a quad wound clockwise on the screen is drawn), the face culled SET_CULL_FACE's
enum { WL_NV2A_CULLS_NONE, WL_NV2A_CULLS_POSITIVE, WL_NV2A_CULLS_NEGATIVE, WL_NV2A_CULLS_ALL };
unsigned wl_nv2a_cull(const WlNv2aKelvin *k);
// the same of the values: SET_CULL_FACE_ENABLE, SET_CULL_FACE and SET_FRONT_FACE's
unsigned wl_nv2a_cull_of(bool enable, uint32_t face, uint32_t front);
// what Direct3D's cull mode culls (D3DRS_CULLMODE: D3DCULL_NONE 0, D3DCULL_CW 0x900, D3DCULL_CCW 0x901,
// the faces of that winding on the screen; Direct3D writes it as the NV2A's front face against it and
// the back culled)
unsigned wl_nv2a_cull_d3d(uint32_t cull_mode);
// GX's cull mode for what is culled, given the GX mode that culls a triangle of positive area
// (measured: GX's names for its windings are not the screen's)
WlGxCull wl_nv2a_gx_cull(unsigned culls, WlGxCull cull_positive);
// a clear's colour (0xRRGGBBAA, from SET_COLOR_CLEAR_VALUE in the surface's format) and depth
// (0..1, from SET_ZSTENCIL_CLEAR_VALUE)
uint32_t wl_nv2a_clear_rgba(const WlNv2aKelvin *k, const WlNv2aSurface *s);
float wl_nv2a_clear_depth(const WlNv2aKelvin *k, const WlNv2aSurface *s);
