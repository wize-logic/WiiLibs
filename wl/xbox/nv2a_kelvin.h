// SPDX-License-Identifier: MIT
// The NV2A's 3D state (nv2a_kelvin.c): what the methods of the Kelvin class (NV097) set, as the
// device (nv2a.h) hands them over, and the draws they make. The state is the methods' own words
// (raw[], by method / 4) with what is loaded through windows kept apart: the vertex program and its
// constants, the attributes' current values. A backend reads the state at each call and decodes it
// with the helpers below: the surface, the combiners as the 57 words of wl/xbox/psh.h, the
// vertices of a draw as the sixteen input registers of a vertex program.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_regs.h"
#include "wl/xbox/psh.h"

enum {
    WL_NV2A_ATTRIBS = 16,
    WL_NV2A_TEXTURES = 4,
    WL_NV2A_PROGRAM_SLOTS = 136,
    WL_NV2A_CONSTANTS = 192,
    // the constants the vertex program reads its viewport from (Direct3D's c[-38] and c[-37]):
    // SET_VIEWPORT_SCALE and SET_VIEWPORT_OFFSET are written there too
    WL_NV2A_CONSTANT_VIEWPORT_SCALE = 58,
    WL_NV2A_CONSTANT_VIEWPORT_OFFSET = 59
};

// a context DMA as the methods set it: its instance address and what its object says
typedef struct WlNv2aDma {
    uint32_t instance;
    uint32_t base, limit;
    uint32_t class_id;
    uint32_t valid; // the object was found in instance memory (else base 0, no limit)
} WlNv2aDma;

// what changed since the last draw (WlNv2aKelvin.dirty: the backend clears what it consumed)
enum {
    WL_NV2A_DIRTY_SURFACE = 1u << 0,
    WL_NV2A_DIRTY_RASTER = 1u << 1, // blend, depth, alpha test, cull, masks, stencil, fog
    WL_NV2A_DIRTY_COMBINERS = 1u << 2,
    WL_NV2A_DIRTY_TEXTURES = 1u << 3,
    WL_NV2A_DIRTY_PROGRAM = 1u << 4,
    WL_NV2A_DIRTY_CONSTANTS = 1u << 5,
    WL_NV2A_DIRTY_VIEWPORT = 1u << 6,
    WL_NV2A_DIRTY_ARRAYS = 1u << 7,
    WL_NV2A_DIRTY_TRANSFORM = 1u << 8, // the fixed function's matrices and modes
    WL_NV2A_DIRTY_ALL = 0x1FFu
};

struct WlNv2aKelvin {
    // every method's last parameter, by method / 4
    uint32_t raw[WL_NV2A_K_METHODS / 4];
    WlNv2aDma dma_notifies, dma_a, dma_b, dma_state, dma_color, dma_zeta, dma_vertex_a, dma_vertex_b,
        dma_semaphore, dma_report;
    // the vertex program's slots (four words an instruction, as SET_TRANSFORM_PROGRAM loads them)
    // and constants (c[0] .. c[191]: Direct3D's c[-96] .. c[95])
    uint32_t program[WL_NV2A_PROGRAM_SLOTS][4];
    float constants[WL_NV2A_CONSTANTS][4];
    // where the next SET_TRANSFORM_PROGRAM / _CONSTANT word goes: slot and word
    uint32_t program_load, program_word, constant_load, constant_word;
    // each attribute's current value (immediate mode; what a vertex reads of a disabled array)
    float current[WL_NV2A_ATTRIBS][4];
    // the pixels the draws covered since CLEAR_REPORT_VALUE, while SET_ZPASS_PIXEL_COUNT_ENABLE
    uint32_t zpass_pixels;
    // the flip queue's pointers (SET_FLIP_*, FLIP_INCREMENT_WRITE)
    uint32_t flip_read, flip_write, flip_modulo;
    // counters: draws handed to the backend, FLIP_INCREMENT_WRITEs, program and constant changes
    uint32_t draw_serial, frames, program_generation, constants_generation;
    // the begin/end open (WL_NV2A_PRIM_*; 0 none)
    uint32_t primitive;
    // WL_NV2A_DIRTY_*
    uint32_t dirty;
};

// a method's word, and as a float
static inline uint32_t wl_nv2a_k(const WlNv2aKelvin *k, uint32_t method) {
    return k->raw[(method & (WL_NV2A_K_METHODS - 1)) >> 2];
}
static inline float wl_nv2a_kf(const WlNv2aKelvin *k, uint32_t method) {
    uint32_t word = wl_nv2a_k(k, method);
    float value;
    memcpy(&value, &word, sizeof value);
    return value;
}

// ---- the surface ---------------------------------------------------------------------------
typedef struct WlNv2aSurface {
    // guest-physical (the context DMA's base plus the offset); 0 when its DMA or offset is unset
    uint32_t color_phys, zeta_phys;
    uint32_t clip_x, clip_y, clip_width, clip_height;
    uint32_t color_pitch, zeta_pitch;
    uint8_t color_format, zeta_format; // WL_NV2A_SURFACE_COLOR_*, WL_NV2A_SURFACE_ZETA_*
    uint8_t swizzled;
    uint8_t log2_width, log2_height; // a swizzled surface's
    uint8_t color_bytes, zeta_bytes; // bytes a pixel (0: none)
    // the samples a pixel has across and down (multisampling: the clip and the screen
    // coordinates are in samples)
    uint8_t samples_x, samples_y;
    // the depth buffer's largest value (65535 or 16777215: the viewport scales z to it); 0 when
    // the format has no depth
    float depth_max;
} WlNv2aSurface;
void wl_nv2a_surface(const WlNv2aKelvin *k, WlNv2aSurface *out);

// ---- the combiners --------------------------------------------------------------------------
// The combiners as wl_xbox_psh_translate takes them: WL_XBOX_PSH_WORDS words in the order of the
// Xbox's pixel shader render states, from the registers that hold them (the texture modes, dot
// mapping and input texture in words 54-56 too).
void wl_nv2a_psh_words(const WlNv2aKelvin *k, uint32_t words[WL_XBOX_PSH_WORDS]);
// the fog colour as psh takes it (0x00RRGGBB) from SET_FOG_COLOR (ABGR)
uint32_t wl_nv2a_fog_rgb(const WlNv2aKelvin *k);

// ---- the vertex program ----------------------------------------------------------------------
// the instructions from SET_TRANSFORM_PROGRAM_START to the one with the end bit (at most the slots
// left): *count of them at *first
void wl_nv2a_program(const WlNv2aKelvin *k, const uint32_t **first, unsigned long *count);
// the transform runs the program (else the fixed function)
static inline bool wl_nv2a_program_mode(const WlNv2aKelvin *k) {
    return (wl_nv2a_k(k, WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE) & 3u) == WL_NV2A_XF_PROGRAM;
}

// ---- a draw's vertices -----------------------------------------------------------------------
enum {
    WL_NV2A_SOURCE_ARRAYS,    // the vertex arrays, by index (DRAW_ARRAYS, ARRAY_ELEMENT16/32)
    WL_NV2A_SOURCE_INLINE,    // INLINE_ARRAY's words
    WL_NV2A_SOURCE_IMMEDIATE  // vertices of the attributes' current values (SET_VERTEX3F, ...)
};

// an attribute as a draw reads it
typedef struct WlNv2aAttrib {
    // ARRAYS: the host address of the attribute of vertex index_base (NULL: disabled or not in
    // guest memory: the attribute's current value); INLINE: unused
    const uint8_t *data;
    uint32_t stride;
    uint8_t type, size; // WL_NV2A_VTX_*, components (0: disabled)
    // INLINE: the attribute's byte offset in a vertex
    uint16_t inline_offset;
} WlNv2aAttrib;

struct WlNv2aDraw {
    uint8_t primitive; // WL_NV2A_PRIM_*
    uint8_t source;    // WL_NV2A_SOURCE_*
    // the batch goes on in the next draw (a long begin/end handed over in parts; a strip's or
    // fan's vertices repeated as they need)
    uint8_t continues;
    uint32_t count;
    // ARRAYS: each vertex's index (NULL: index_base + i); index_base is the least
    const uint32_t *indices;
    uint32_t index_base, index_last;
    // INLINE: the vertices, inline_stride bytes each (little-endian words)
    const uint8_t *inline_data;
    uint32_t inline_stride;
    // IMMEDIATE: the vertices' registers
    const float (*immediate)[WL_NV2A_ATTRIBS][4];
    WlNv2aAttrib attribs[WL_NV2A_ATTRIBS];
    // the attributes that come from the vertices (bit n: v[n]); the others are the current values
    uint32_t streamed;
    uint32_t serial;
};

// vertex `vertex` of the draw (0 .. count - 1) as the program's input registers: what the
// arrays, the inline data or the immediate vertex give, a disabled attribute's current value
void wl_nv2a_draw_fetch(const WlNv2aDraw *draw, const WlNv2aKelvin *k, uint32_t vertex,
                        float inputs[WL_NV2A_ATTRIBS][4]);
// the same for the attributes in `attributes` alone (bit n: v[n]); the others' registers are left as
// they were (an immediate vertex's are all copied)
void wl_nv2a_draw_fetch_some(const WlNv2aDraw *draw, const WlNv2aKelvin *k, uint32_t vertex, uint32_t attributes,
                             float inputs[WL_NV2A_ATTRIBS][4]);
// A draw's fetch decided once for its vertices: the attributes in `attributes` (bit n: v[n]) that
// come from the vertices, each with its address, stride and format, and those that are current
// values (the same for every vertex: wl_nv2a_fetch_constants writes them). wl_nv2a_fetch_vertex then
// gives what wl_nv2a_draw_fetch_some gives, to the bit, for the streamed ones (an immediate vertex's
// registers all copied).
typedef struct WlNv2aFetchPlan {
    uint8_t source; // WL_NV2A_SOURCE_*
    uint8_t count;  // streamed attributes
    struct {
        const uint8_t *data; // vertex 0's (ARRAYS: index index_base's; INLINE: the first vertex's)
        uint32_t stride;
        uint8_t attribute, format; // v[n]; WL_NV2A_FETCH_*
        uint8_t type, size;
    } streamed[WL_NV2A_ATTRIBS];
    uint32_t constants; // the current values' attributes
    const uint32_t *indices;
    uint32_t index_base;
} WlNv2aFetchPlan;
void wl_nv2a_fetch_plan(const WlNv2aDraw *draw, uint32_t attributes, WlNv2aFetchPlan *plan);
// the current values' attributes into each of `count` input register sets
void wl_nv2a_fetch_constants(const WlNv2aFetchPlan *plan, const WlNv2aKelvin *k, float (*inputs)[WL_NV2A_ATTRIBS][4],
                             unsigned count);
void wl_nv2a_fetch_vertex(const WlNv2aFetchPlan *plan, const WlNv2aDraw *draw, uint32_t vertex,
                          float inputs[WL_NV2A_ATTRIBS][4]);
// one attribute's value from its data (an array element or inline), as the NV2A widens it: the
// components the format leaves out are 0, w 1
void wl_nv2a_attrib_decode(uint8_t type, uint8_t size, const uint8_t *data, float out[4]);
// the bytes an attribute of this format takes (0: disabled)
uint32_t wl_nv2a_attrib_bytes(uint8_t type, uint8_t size);

// the primitive's GX equivalent and its vertex count rules, for the backends
typedef struct WlNv2aPrimInfo {
    uint8_t first;   // vertices of the first primitive
    uint8_t step;    // vertices each next primitive adds
    uint8_t lines, points, strip, fan;
} WlNv2aPrimInfo;
void wl_nv2a_prim_info(uint32_t primitive, WlNv2aPrimInfo *out);
