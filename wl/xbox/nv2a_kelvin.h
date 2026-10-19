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
#include "wl/xbox/vsh.h"

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

// what changed since the last draw (WlNv2aKelvin.dirty, cleared after each draw: a backend keeps its
// own if it needs more): a group is dirty when one of its values changes, not when a method writes
// the value it had (a title's Direct3D sends whole state blocks again), and the program and
// constants' generations move the same way
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
    // what the pixel program is made from (wl_nv2a_psh_words and wl_nv2a_psh_textures read): the
    // combiners and their stage programs, the fog's switch and colour
    WL_NV2A_DIRTY_PIXEL = 1u << 9,
    WL_NV2A_DIRTY_ALL = 0x3FFu
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
    // (the vertex arrays' layout kept between draws, looked at again when WL_NV2A_DIRTY_ARRAYS is set
    // and made again when layout_ok is 0: each enabled array's attribute, format, stride, bytes a vertex
    // and guest-physical start, layout_count of them in attribute order, from the formats in
    // layout_formats (the starts alone made again when they are the same); not part of the state a
    // capture keeps)
    struct {
        uint64_t start;
        uint32_t stride, bytes;
        uint8_t attrib, type, size;
    } layout[WL_NV2A_ATTRIBS];
    uint32_t layout_formats[WL_NV2A_ATTRIBS];
    uint8_t layout_count, layout_ok;
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
    // ARRAYS: the guest-physical address of that attribute (data's)
    uint32_t phys;
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
enum { WL_NV2A_PREFETCH_AREAS = 2 };
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
    // (the arrays' bytes a vertex reads, for wl_nv2a_fetch_prefetch: the streamed attributes of one
    // stride whose bytes lie within one stride of each other taken together, WL_NV2A_PREFETCH_AREAS at
    // most; `last` the offset of the last byte from data)
    uint8_t prefetch_count;
    struct {
        const uint8_t *data;
        uint32_t stride, last;
    } prefetch[WL_NV2A_PREFETCH_AREAS];
} WlNv2aFetchPlan;
void wl_nv2a_fetch_plan(const WlNv2aDraw *draw, uint32_t attributes, WlNv2aFetchPlan *plan);

// Broadway does not prefetch by itself, and the guest's vertex data and push buffer are first read
// where they are used: a miss for each 32-byte line. The vertex path asks for the lines ahead with
// dcbt (a hint: no fault on an address not mapped, and no change to what is read). PowerPC only; the
// host's build has none of it. wl_nv2a_set_prefetch(false) (the title folder's file "noprefetch",
// the replay's --no-prefetch) leaves them out, for an A/B on the console.
#if defined(WL_NV2A_PREFETCH_RECORD)
// (a test's build of nv2a_gx.c: each line asked for handed to the test, test_xbox_nv2a_prefetch)
#define WL_NV2A_PREFETCHES 1
void wl_nv2a_prefetch_record(const void *p);
#define WL_NV2A_PREFETCH(p) wl_nv2a_prefetch_record((const void *)(p))
#elif defined(__powerpc__) || defined(__PPC__)
#define WL_NV2A_PREFETCHES 1
// (in asm: GCC removes a loop of __builtin_prefetch alone as having no effect)
#define WL_NV2A_PREFETCH(p) __asm__ volatile("dcbt 0,%0" : : "r"((const void *)(p)))
#else
#define WL_NV2A_PREFETCHES 0
#define WL_NV2A_PREFETCH(p) ((void)(p))
#endif
enum {
    WL_NV2A_CACHE_LINE = 32,
    WL_NV2A_PUSH_AHEAD = 4 * WL_NV2A_CACHE_LINE // the push buffer's bytes asked for ahead of its reads
};
extern bool wl_nv2a_prefetch_on;
void wl_nv2a_set_prefetch(bool on);
// the pusher's header loop as before the method table's run kinds (true), for an A/B on the console
void wl_nv2a_set_old_push(bool on);
// the pusher's loop of wave 4 (true, the default: the one-word headers to plain methods alone in the
// loop, runs of plain methods and the load slots and begin/end straight, the index runs a line at a
// time, the windows' slots read in place); false: the loop as wave 3 left it (a file "oldpush2",
// WL_XB_OLDPUSH2), for an A/B
void wl_nv2a_set_push_cuts(bool on);
// true (the default): the vertex arrays' layout kept between draws (made again when their formats
// change, their starts alone when an offset or a vertex DMA does), the 16-bit index lines in assembly on
// Broadway, the viewport's words straight to their constants; false: as wave 4's first six left them (a file "oldpush3", WL_XB_OLDPUSH3), for an
// A/B. A span whose start passes 4 GiB is refused either way.
void wl_nv2a_set_push_cuts3(bool on);
static inline bool wl_nv2a_prefetching(void) {
    return WL_NV2A_PREFETCHES && wl_nv2a_prefetch_on;
}
// the lines of element `element`'s streamed bytes asked for (a vertex's element: wl_nv2a_fetch_element's)
static inline void wl_nv2a_fetch_prefetch(const WlNv2aFetchPlan *plan, uint32_t element) {
    unsigned a;

    for (a = 0; a < plan->prefetch_count; a++) {
        const uint8_t *data = plan->prefetch[a].data + (size_t)element * plan->prefetch[a].stride;

        WL_NV2A_PREFETCH(data);
        WL_NV2A_PREFETCH(data + plan->prefetch[a].last);
    }
}
// the current values' attributes into each of `count` input register sets
void wl_nv2a_fetch_constants(const WlNv2aFetchPlan *plan, const WlNv2aKelvin *k, float (*inputs)[WL_NV2A_ATTRIBS][4],
                             unsigned count);
void wl_nv2a_fetch_vertex(const WlNv2aFetchPlan *plan, const WlNv2aDraw *draw, uint32_t vertex,
                          float inputs[WL_NV2A_ATTRIBS][4]);
// the same for `count` vertices (vertices[n] into inputs[n]), an attribute at a time for up to
// WL_NV2A_FETCH_BATCH of them
enum { WL_NV2A_FETCH_BATCH = 32 };
void wl_nv2a_fetch_vertices(const WlNv2aFetchPlan *plan, const WlNv2aDraw *draw, const uint32_t *vertices,
                            uint32_t count, float (*inputs)[WL_NV2A_ATTRIBS][4]);
// a vertex's element: an array's index from the least (index_base), an inline or immediate vertex's
// number
static inline uint32_t wl_nv2a_fetch_element(const WlNv2aFetchPlan *plan, uint32_t vertex) {
    return plan->source == WL_NV2A_SOURCE_ARRAYS && plan->indices != NULL ? plan->indices[vertex] - plan->index_base
                                                                          : vertex;
}
// the same from the vertices' elements (wl_nv2a_fetch_element's), for a caller that has them
void wl_nv2a_fetch_elements(const WlNv2aFetchPlan *plan, const WlNv2aDraw *draw, const uint32_t *elements,
                            uint32_t count, float (*inputs)[WL_NV2A_ATTRIBS][4]);
// streamed attribute i's format as a fused batch reads it (WL_XBOX_VSH_FUSED_F1 ...;
// WL_XBOX_VSH_FUSED_FORMATS: one it cannot), and the tables the fetch converts by
unsigned wl_nv2a_fetch_fused_format(const WlNv2aFetchPlan *plan, unsigned i);
void wl_nv2a_fetch_tables(const float **byte_float, const float **cmp11, const float **cmp10);
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
