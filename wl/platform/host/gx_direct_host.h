// SPDX-License-Identifier: MIT
// Host stub of the direct GX layer (gx_direct.h): what was asked, kept for tests; nothing is drawn
// here, but a renderer set with wl_host_gx_set_renderer (gx_renderer_host.h) draws each primitive
// with the state kept.
#pragma once

#include "wl/platform/gx_direct.h"

enum { WL_HOST_GX_DIRECT_KEPT = 256 };
enum { WL_HOST_GX_MAP_NONE = 0, WL_HOST_GX_MAP_TEXTURE, WL_HOST_GX_MAP_RAMP };

typedef struct WlHostGxDirect {
    WlGxDirectLayout layout;
    // the transform unit's matrices (wl_gx_direct_position_matrix, wl_gx_direct_texgen): set or
    // the identity
    bool position_matrix_set;
    float position_matrix[3][4];
    struct {
        bool set;
        uint8_t source; // WlGxDirectTexSource
        float m[2][4];
    } texgen[8];
    // the lit channels' light (wl_gx_direct_lighting), applied to the vertices as they arrive
    WlGxDirectLighting lighting;
    float projection[4][4];
    bool orthographic;
    float viewport[4];
    uint32_t scissor[4];
    WlGxBlendMode blend_mode;
    WlGxBlendFactor blend_source, blend_destination;
    bool z_test, z_write, z_early;
    WlGxCompare z_func, alpha_func;
    uint8_t alpha_ref;
    bool color_update, alpha_update;
    WlGxCull cull;
    const WlGxTexObj *textures[8];
    WlGxSampler samplers[8];
    // each texture map as GX holds it: what was loaded last (WL_HOST_GX_MAP_*), a copy of the
    // object, and a colour-indexed one's TLUT
    uint8_t map_kind[8];
    WlGxTexObj map_objects[8];
    WlGxTlut tluts[8];
    bool tlut_loaded[8];
    uint32_t begins, invalidates;
    // primitives that got fewer vertices than they said
    uint32_t short_draws;
    // vertices whose bytes differ from what the layout's vertex descriptor makes GX read (on the
    // console the command processor then reads vertex data as commands, or commands as vertex data:
    // a hung GPU), and the first such vertex's draw (s.stats.draws then), layout, bytes sent and read
    uint32_t wire_mismatches, wire_first_draw, wire_first_sent, wire_first_read;
    // the bytes of the vertices sent inside primitives (what the Wii's layer writes to the FIFO for them),
    // counted alike in both modes (wl_host_gx_direct_count_only)
    uint64_t wire_bytes;
    // primitives with each finding of wl_gx_check_draw (by its bit's number), in the state the Wii's
    // layer would give GX (wl_gx_direct_layout_outputs, the last TEV program, the maps loaded), and
    // the first such primitive (s.stats.draws then)
    uint32_t check_draws[WL_GX_CHECKS], check_first_draw[WL_GX_CHECKS];
    WlGxDirectLayout wire_first_layout;
    // the last draw's primitive, and its first vertices (up to WL_HOST_GX_DIRECT_KEPT)
    WlGxDirectPrim prim;
    uint32_t count;
    WlGxDirectVertex vertices[WL_HOST_GX_DIRECT_KEPT];
    WlGxDirectStats stats;
    // the indexed attributes' arrays (wl_gx_direct_array: WL_GX_DIRECT_ARRAY_*), the vertices sent
    // through indices, and how often the arrays were said to have changed
    const void *arrays[12];
    uint32_t array_strides[12];
    uint32_t indexed_vertices, arrays_changed;
    // a renderer's own vertex formats: its primitives begun (wl_gx_direct_begin_format), the last's
    // GX primitive, format and count, and the bytes of the last flush sent before one
    uint32_t format_begins;
    uint8_t format_primitive, format;
    uint16_t format_count;
    uint32_t flush_bytes;
} WlHostGxDirect;

const WlHostGxDirect *wl_host_gx_direct(void);
void wl_host_gx_direct_reset(void);
// a hook called with every draw (NULL: none), for a test that wants each one
void wl_host_gx_direct_hook(void (*hook)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices,
                                         uint32_t count),
                            void *user);
// the vertex descriptor GX would hold (GX_GetVtxDescv's list), for a test of the flush before a
// renderer's own vertex formats: the host has no GX_SetVtxDesc
void wl_host_gx_direct_descriptor(const WlGxVtxDesc *desc, unsigned count);
// The count-only mode: the vertices sent are counted (wire_bytes, stats, indexed_vertices, short_draws),
// checked for shape (wire_mismatches) and written as the Wii's layer writes them to the FIFO (the same
// loops, gx_pipe_words.h, into one word of memory), but not decoded back into WlGxDirectVertex, so no
// hook or renderer is called and `vertices` is not kept. Decoding every vertex is a quarter of a ppc-be
// count of a title where the Wii only writes the FIFO's words; in this mode the count is the Wii's work.
// Off by default (gxsoft, the goldens and the tests read the vertices); on from the first primitive
// when the environment has WL_XB_GXSTUB=count, or set here (a test). wl_host_gx_direct_reset keeps it.
void wl_host_gx_direct_count_only(bool on);
bool wl_host_gx_direct_counting(void);
