// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_kelvin: the 3D state. Every vertex array format decoded (D3DCOLOR's byte order,
// normalized and plain shorts, floats and FLOAT2H, bytes, the packed normal); draws from arrays by
// DRAW_ARRAYS, ARRAY_ELEMENT16 and ARRAY_ELEMENT32, from inline arrays and from immediate vertices
// (each kind of SET_VERTEX_DATA*), disabled attributes reading their current value; long batches
// handed over in whole primitives; the vertex program and constant windows; the viewport in
// c[58]/c[59]; the state groups dirty on a change of value; the combiners as psh's 57 words, and
// every method the pixel program reads dirtying it; the surface decoded. A draw's fetch plan
// against the fetch of each vertex alone, on random formats, data and attribute sets, bit for bit.
// Random method runs through the push buffer against each method given alone: the same 3D state.
#include <math.h>
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "wl/xbox/nv2a_internal.h"
#include "wl/xbox/nv2a_pipeline.h"

// ---- a backend that keeps each draw's vertices -----------------------------------------------

enum { KEPT_VERTICES = 4096, KEPT_DRAWS = 64 };

typedef struct Kept {
    unsigned draws;
    uint8_t primitive[KEPT_DRAWS], source[KEPT_DRAWS], continues[KEPT_DRAWS];
    uint32_t counts[KEPT_DRAWS];
    uint32_t total;
    float inputs[KEPT_VERTICES][WL_NV2A_ATTRIBS][4];
} Kept;

static Kept g_kept;

static uint32_t keep_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    Kept *kept = user;
    uint32_t v;

    if (kept->draws < KEPT_DRAWS) {
        kept->primitive[kept->draws] = draw->primitive;
        kept->source[kept->draws] = draw->source;
        kept->continues[kept->draws] = draw->continues;
        kept->counts[kept->draws] = draw->count;
    }
    kept->draws++;
    for (v = 0; v < draw->count; v++) {
        if (kept->total < KEPT_VERTICES) {
            wl_nv2a_draw_fetch(draw, k, v, kept->inputs[kept->total]);
        }
        kept->total++;
    }
    return 0;
}

static WlNv2aBackend keeping_backend(void) {
    WlNv2aBackend b;
    memset(&g_kept, 0, sizeof g_kept);
    memset(&b, 0, sizeof b);
    b.user = &g_kept;
    b.draw = keep_draw;
    return b;
}

static int near(float a, float b) {
    return fabsf(a - b) <= 1e-6f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f);
}

#define ASSERT_V4(v, x, y, z, w)                                                                          \
    do {                                                                                                  \
        ASSERT(near((v)[0], (x)));                                                                        \
        ASSERT(near((v)[1], (y)));                                                                        \
        ASSERT(near((v)[2], (z)));                                                                        \
        ASSERT(near((v)[3], (w)));                                                                        \
    } while (0)

static uint32_t format_word(uint32_t type, uint32_t size, uint32_t stride) {
    return stride << 8 | size << 4 | type;
}

// ---- attribute decoding -----------------------------------------------------------------------------

TEST every_array_format_decodes(void) {
    uint8_t data[16];
    float v[4];

    // D3DCOLOR: bytes B, G, R, A
    data[0] = 0x10, data[1] = 0x20, data[2] = 0x30, data[3] = 0xFF;
    wl_nv2a_attrib_decode(WL_NV2A_VTX_UB_D3D, 4, data, v);
    ASSERT_V4(v, 0x30 / 255.0f, 0x20 / 255.0f, 0x10 / 255.0f, 1.0f);
    // bytes in order, missing components 0 and w 1
    wl_nv2a_attrib_decode(WL_NV2A_VTX_UB_OGL, 2, data, v);
    ASSERT_V4(v, 0x10 / 255.0f, 0x20 / 255.0f, 0.0f, 1.0f);
    // normalized shorts: 32767 is 1, -32768 clamps to -1
    wl_wr_le16(data, 32767), wl_wr_le16(data + 2, 0x8000), wl_wr_le16(data + 4, 16384);
    wl_nv2a_attrib_decode(WL_NV2A_VTX_S1, 3, data, v);
    ASSERT_V4(v, 1.0f, -1.0f, 16384.0f / 32767.0f, 1.0f);
    // plain shorts
    wl_wr_le16(data, (uint16_t)-5), wl_wr_le16(data + 2, 300);
    wl_nv2a_attrib_decode(WL_NV2A_VTX_S32K, 2, data, v);
    ASSERT_V4(v, -5.0f, 300.0f, 0.0f, 1.0f);
    // floats, one to four
    wl_wr_le32(data, t_f(1.5f)), wl_wr_le32(data + 4, t_f(-2.0f)), wl_wr_le32(data + 8, t_f(3.25f)),
        wl_wr_le32(data + 12, t_f(0.5f));
    wl_nv2a_attrib_decode(WL_NV2A_VTX_F, 1, data, v);
    ASSERT_V4(v, 1.5f, 0.0f, 0.0f, 1.0f);
    wl_nv2a_attrib_decode(WL_NV2A_VTX_F, 4, data, v);
    ASSERT_V4(v, 1.5f, -2.0f, 3.25f, 0.5f);
    // FLOAT2H: x, y and w
    wl_nv2a_attrib_decode(WL_NV2A_VTX_F, 7, data, v);
    ASSERT_V4(v, 1.5f, -2.0f, 0.0f, 3.25f);
    ASSERT_EQ(12u, wl_nv2a_attrib_bytes(WL_NV2A_VTX_F, 7));
    // the packed normal: 11, 11 and 10 signed bits
    wl_wr_le32(data, (1023u) | (0x401u << 11) | (511u << 22));
    wl_nv2a_attrib_decode(WL_NV2A_VTX_CMP, 1, data, v);
    ASSERT_V4(v, 1.0f, -1.0f, 1.0f, 1.0f);
    ASSERT_EQ(4u, wl_nv2a_attrib_bytes(WL_NV2A_VTX_CMP, 1));
    ASSERT_EQ(0u, wl_nv2a_attrib_bytes(WL_NV2A_VTX_F, 0));
    PASS();
}

// ---- draws -------------------------------------------------------------------------------------------

// positions (three floats) at 0x40000, colours (D3DCOLOR) at 0x41000
static void arrays_setup(TGuest *g, unsigned count) {
    unsigned i;
    for (i = 0; i < count; i++) {
        t_w32(g, 0x40000 + 12 * i, t_f((float)i));
        t_w32(g, 0x40000 + 12 * i + 4, t_f((float)(i * 2)));
        t_w32(g, 0x40000 + 12 * i + 8, t_f(0.5f));
        t_w32(g, 0x41000 + 4 * i, 0xFF000000u | i);
    }
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 0, 0x40000);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 0, format_word(WL_NV2A_VTX_F, 3, 12));
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 12, 0x41000);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 12, format_word(WL_NV2A_VTX_UB_D3D, 4, 4));
}

TEST draw_arrays_and_elements(void) {
    TGuest g;
    WlNv2aBackend backend = keeping_backend();
    ASSERT(t_create(&g, &backend) != NULL);
    t_setup_3d(&g);
    arrays_setup(&g, 16);
    // a current texture coordinate for the disabled attribute 9
    t_method(&g, WL_NV2A_K_SET_VERTEX_DATA4F_M + 16 * 9, 4);
    t_word(&g, t_f(0.25f));
    t_word(&g, t_f(0.75f));
    t_word(&g, t_f(0.0f));
    t_word(&g, t_f(1.0f));
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLE_STRIP);
    t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (3u << 24) | 2); // vertices 2..5
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    t_m1(&g, WL_NV2A_K_ARRAY_ELEMENT16, (7u << 16) | 9u);
    t_m1(&g, WL_NV2A_K_ARRAY_ELEMENT32, 11);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT_EQ(2u, g_kept.draws);
    ASSERT_EQ((uint8_t)WL_NV2A_PRIM_TRIANGLE_STRIP, g_kept.primitive[0]);
    ASSERT_EQ(4u, g_kept.counts[0]);
    ASSERT_EQ((uint8_t)WL_NV2A_SOURCE_ARRAYS, g_kept.source[0]);
    ASSERT_V4(g_kept.inputs[0][0], 2.0f, 4.0f, 0.5f, 1.0f);
    ASSERT_V4(g_kept.inputs[3][0], 5.0f, 10.0f, 0.5f, 1.0f);
    ASSERT_V4(g_kept.inputs[3][3], 0.0f, 0.0f, 5 / 255.0f, 1.0f);
    ASSERT_V4(g_kept.inputs[1][9], 0.25f, 0.75f, 0.0f, 1.0f);
    // the elements: 9, 7 (the first index in the low half), 11
    ASSERT_EQ(3u, g_kept.counts[1]);
    ASSERT_V4(g_kept.inputs[4][0], 9.0f, 18.0f, 0.5f, 1.0f);
    ASSERT_V4(g_kept.inputs[5][0], 7.0f, 14.0f, 0.5f, 1.0f);
    ASSERT_V4(g_kept.inputs[6][0], 11.0f, 22.0f, 0.5f, 1.0f);
    t_destroy(&g);
    PASS();
}

TEST inline_arrays_follow_the_formats(void) {
    TGuest g;
    WlNv2aBackend backend = keeping_backend();
    unsigned i;
    ASSERT(t_create(&g, &backend) != NULL);
    t_setup_3d(&g);
    // a vertex: position (three floats), colour (D3DCOLOR), texture coordinate (two shorts)
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 0, format_word(WL_NV2A_VTX_F, 3, 20));
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 12, format_word(WL_NV2A_VTX_UB_D3D, 4, 20));
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 36, format_word(WL_NV2A_VTX_S32K, 2, 20));
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_QUADS);
    t_word(&g, WL_NV2A_CMD_NON_INCREASING | (20u << 18) | WL_NV2A_K_INLINE_ARRAY);
    for (i = 0; i < 4; i++) {
        t_word(&g, t_f((float)i));
        t_word(&g, t_f(1.0f));
        t_word(&g, t_f(2.0f));
        t_word(&g, 0x80402010u);
        t_word(&g, (uint32_t)(uint16_t)(i * 3) | (uint32_t)(uint16_t)-7 << 16);
    }
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT_EQ(1u, g_kept.draws);
    ASSERT_EQ((uint8_t)WL_NV2A_SOURCE_INLINE, g_kept.source[0]);
    ASSERT_EQ(4u, g_kept.counts[0]);
    ASSERT_V4(g_kept.inputs[2][0], 2.0f, 1.0f, 2.0f, 1.0f);
    ASSERT_V4(g_kept.inputs[2][3], 0x40 / 255.0f, 0x20 / 255.0f, 0x10 / 255.0f, 0x80 / 255.0f);
    ASSERT_V4(g_kept.inputs[3][9], 9.0f, -7.0f, 0.0f, 1.0f);
    t_destroy(&g);
    PASS();
}

TEST immediate_vertices_of_each_kind(void) {
    TGuest g;
    WlNv2aBackend backend = keeping_backend();
    ASSERT(t_create(&g, &backend) != NULL);
    t_setup_3d(&g);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLE_FAN);
    // a colour (four bytes in order), a texture coordinate (2F), then the position (2F: completes)
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA4UB + 4 * 3, 0xFF8040C0u);
    t_method(&g, WL_NV2A_K_SET_VERTEX_DATA2F_M + 8 * 9, 2);
    t_word(&g, t_f(0.5f));
    t_word(&g, t_f(0.25f));
    t_method(&g, WL_NV2A_K_SET_VERTEX_DATA2F_M, 2);
    t_word(&g, t_f(10.0f));
    t_word(&g, t_f(20.0f));
    // SET_VERTEX3F completes on z
    t_method(&g, WL_NV2A_K_SET_VERTEX3F, 3);
    t_word(&g, t_f(1.0f));
    t_word(&g, t_f(2.0f));
    t_word(&g, t_f(3.0f));
    // SET_VERTEX_DATA4F_M on w
    t_method(&g, WL_NV2A_K_SET_VERTEX_DATA4F_M, 4);
    t_word(&g, t_f(4.0f));
    t_word(&g, t_f(5.0f));
    t_word(&g, t_f(6.0f));
    t_word(&g, t_f(7.0f));
    // SET_VERTEX_DATA2S (one word)
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA2S, (uint32_t)(uint16_t)-3 | 8u << 16);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT_EQ(1u, g_kept.draws);
    ASSERT_EQ(4u, g_kept.counts[0]);
    ASSERT_EQ((uint8_t)WL_NV2A_SOURCE_IMMEDIATE, g_kept.source[0]);
    ASSERT_V4(g_kept.inputs[0][0], 10.0f, 20.0f, 0.0f, 1.0f);
    ASSERT_V4(g_kept.inputs[0][3], 0xC0 / 255.0f, 0x40 / 255.0f, 0x80 / 255.0f, 1.0f);
    ASSERT_V4(g_kept.inputs[0][9], 0.5f, 0.25f, 0.0f, 1.0f);
    ASSERT_V4(g_kept.inputs[1][0], 1.0f, 2.0f, 3.0f, 1.0f);
    ASSERT_V4(g_kept.inputs[2][0], 4.0f, 5.0f, 6.0f, 7.0f);
    ASSERT_V4(g_kept.inputs[3][0], -3.0f, 8.0f, 0.0f, 1.0f);
    // the colour carried to every vertex
    ASSERT_V4(g_kept.inputs[3][3], 0xC0 / 255.0f, 0x40 / 255.0f, 0x80 / 255.0f, 1.0f);
    t_destroy(&g);
    PASS();
}

TEST long_batches_go_in_whole_primitives(void) {
    TGuest g;
    WlNv2aBackend backend = keeping_backend();
    unsigned i, total;
    ASSERT(t_create(&g, &backend) != NULL);
    t_setup_3d(&g);
    arrays_setup(&g, 16);
    // a strip of 10000 vertices (index i % 16) in DRAW_ARRAYS of 250: parts that each go on from
    // the last two vertices of the one before
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLE_STRIP);
    for (i = 0; i < 40; i++) {
        t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (249u << 24) | 0);
    }
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT(g_kept.draws >= 2);
    total = 0;
    for (i = 0; i < g_kept.draws && i < KEPT_DRAWS; i++) {
        ASSERT_EQ((uint8_t)WL_NV2A_PRIM_TRIANGLE_STRIP, g_kept.primitive[i]);
        if (i + 1 < g_kept.draws) {
            ASSERT_EQ(1u, (unsigned)g_kept.continues[i]);
            ASSERT_EQ(0u, g_kept.counts[i] & 1u); // even: the next part's winding is the strip's
        }
        total += g_kept.counts[i];
    }
    // every triangle once: the parts' triangles add up to the strip's
    {
        unsigned triangles = 0;
        for (i = 0; i < g_kept.draws && i < KEPT_DRAWS; i++) {
            triangles += g_kept.counts[i] - 2;
        }
        ASSERT_EQ(10000u - 2u, triangles);
    }
    (void)total;
    // a triangle list of 9000 indices: parts of whole triangles
    memset(&g_kept, 0, sizeof g_kept);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    for (i = 0; i < 4500; i++) {
        t_m1(&g, WL_NV2A_K_ARRAY_ELEMENT16, (i % 16) | ((i + 1) % 16) << 16);
    }
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    total = 0;
    for (i = 0; i < g_kept.draws; i++) {
        ASSERT_EQ(0u, g_kept.counts[i] % 3u);
        total += g_kept.counts[i];
    }
    ASSERT_EQ(9000u, total);
    t_destroy(&g);
    PASS();
}

TEST line_loops_close(void) {
    TGuest g;
    WlNv2aBackend backend = keeping_backend();
    ASSERT(t_create(&g, &backend) != NULL);
    t_setup_3d(&g);
    arrays_setup(&g, 4);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_LINE_LOOP);
    t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (3u << 24) | 0);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT_EQ(1u, g_kept.draws);
    ASSERT_EQ((uint8_t)WL_NV2A_PRIM_LINE_STRIP, g_kept.primitive[0]);
    ASSERT_EQ(5u, g_kept.counts[0]);
    ASSERT_V4(g_kept.inputs[4][0], 0.0f, 0.0f, 0.5f, 1.0f);
    t_destroy(&g);
    PASS();
}

// ---- the program, constants, viewport -------------------------------------------------------------

TEST program_and_constant_windows(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    const uint32_t *first;
    unsigned long count;
    unsigned i;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    // three instructions loaded at slot 10 through the window (the last with the end bit)
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD, 10);
    t_method(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM, 12);
    for (i = 0; i < 12; i++) {
        t_word(&g, i == 11 ? 1u : 0x1000u + (i << 4));
    }
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_START, 10);
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE, WL_NV2A_XF_PROGRAM);
    // constants 96 and 97 (Direct3D's c0, c1)
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, 96);
    t_method(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT, 8);
    for (i = 0; i < 8; i++) {
        t_word(&g, t_f((float)i + 0.5f));
    }
    // the viewport
    t_method(&g, WL_NV2A_K_SET_VIEWPORT_SCALE, 4);
    t_word(&g, t_f(320.0f));
    t_word(&g, t_f(-240.0f));
    t_word(&g, t_f(16777215.0f));
    t_word(&g, t_f(0.0f));
    t_method(&g, WL_NV2A_K_SET_VIEWPORT_OFFSET, 4);
    t_word(&g, t_f(320.0f));
    t_word(&g, t_f(240.0f));
    t_word(&g, t_f(0.0f));
    t_word(&g, t_f(0.0f));
    t_kick(&g);
    ASSERT_EQ(0x1040u, k->program[11][0]);
    ASSERT_EQ(13u, k->program_load);
    wl_nv2a_program(k, &first, &count);
    ASSERT_EQ(3ul, count);
    ASSERT(first == k->program[10]);
    ASSERT(wl_nv2a_program_mode(k));
    ASSERT(near(k->constants[96][0], 0.5f));
    ASSERT(near(k->constants[97][3], 7.5f));
    ASSERT_EQ(98u, k->constant_load);
    ASSERT(near(k->constants[WL_NV2A_CONSTANT_VIEWPORT_SCALE][1], -240.0f));
    ASSERT(near(k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET][0], 320.0f));
    t_destroy(&g);
    PASS();
}

// the state groups dirty, and the program and constants' generations moved, when a value changes,
// not when a method writes the value it had; the viewport's words compared with the constants
// they write (which the window may have written since)
TEST dirty_on_a_change_of_value(void) {
    TGuest g;
    WlNv2aKelvin *k;
    uint32_t program_generation, constants_generation;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = (WlNv2aKelvin *)wl_nv2a_kelvin(g.dev);
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD, 0);
    t_method(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM, 4);
    t_word(&g, 1);
    t_word(&g, 2);
    t_word(&g, 3);
    t_word(&g, 4);
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, 96);
    t_method(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT, 4);
    t_word(&g, t_f(1.0f));
    t_word(&g, t_f(2.0f));
    t_word(&g, t_f(3.0f));
    t_word(&g, t_f(4.0f));
    t_m1(&g, WL_NV2A_K_SET_VIEWPORT_OFFSET, t_f(320.0f));
    t_kick(&g);
    ASSERT(k->dirty & WL_NV2A_DIRTY_RASTER);
    // all of it again, as it was: nothing
    k->dirty = 0;
    program_generation = k->program_generation;
    constants_generation = k->constants_generation;
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD, 0);
    t_method(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM, 4);
    t_word(&g, 1);
    t_word(&g, 2);
    t_word(&g, 3);
    t_word(&g, 4);
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, 96);
    t_method(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT, 4);
    t_word(&g, t_f(1.0f));
    t_word(&g, t_f(2.0f));
    t_word(&g, t_f(3.0f));
    t_word(&g, t_f(4.0f));
    t_m1(&g, WL_NV2A_K_SET_VIEWPORT_OFFSET, t_f(320.0f));
    t_kick(&g);
    ASSERT_EQ(0u, k->dirty);
    ASSERT_EQ(program_generation, k->program_generation);
    ASSERT_EQ(constants_generation, k->constants_generation);
    ASSERT_EQ(1u, k->program_load);
    ASSERT_EQ(97u, k->constant_load);
    // one value each changed
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 0);
    t_kick(&g);
    ASSERT_EQ((uint32_t)WL_NV2A_DIRTY_RASTER, k->dirty);
    k->dirty = 0;
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD, 0);
    t_method(&g, WL_NV2A_K_SET_TRANSFORM_PROGRAM, 2);
    t_word(&g, 1);
    t_word(&g, 5);
    t_kick(&g);
    ASSERT_EQ((uint32_t)WL_NV2A_DIRTY_PROGRAM, k->dirty);
    ASSERT(k->program_generation != program_generation);
    k->dirty = 0;
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, 96);
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT, t_f(-1.0f));
    t_kick(&g);
    ASSERT_EQ((uint32_t)WL_NV2A_DIRTY_CONSTANTS, k->dirty);
    ASSERT(k->constants_generation != constants_generation);
    ASSERT(near(k->constants[96][0], -1.0f));
    // the viewport's offset overwritten through the window, then set again as it was: set
    k->dirty = 0;
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, WL_NV2A_CONSTANT_VIEWPORT_OFFSET);
    t_m1(&g, WL_NV2A_K_SET_TRANSFORM_CONSTANT, t_f(7.0f));
    t_m1(&g, WL_NV2A_K_SET_VIEWPORT_OFFSET, t_f(320.0f));
    t_kick(&g);
    ASSERT(k->dirty & WL_NV2A_DIRTY_VIEWPORT);
    ASSERT(near(k->constants[WL_NV2A_CONSTANT_VIEWPORT_OFFSET][0], 320.0f));
    t_destroy(&g);
    PASS();
}

TEST combiners_as_the_pixel_shader_words(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    uint32_t words[WL_XBOX_PSH_WORDS];
    unsigned i;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    for (i = 0; i < 8; i++) {
        t_m1(&g, WL_NV2A_K_SET_COMBINER_ALPHA_ICW + 4 * i, 0xA0000000u + i);
        t_m1(&g, WL_NV2A_K_SET_COMBINER_FACTOR0 + 4 * i, 0xC0000000u + i);
        t_m1(&g, WL_NV2A_K_SET_COMBINER_FACTOR1 + 4 * i, 0xC1000000u + i);
        t_m1(&g, WL_NV2A_K_SET_COMBINER_ALPHA_OCW + 4 * i, 0xA1000000u + i);
        t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_ICW + 4 * i, 0xB0000000u + i);
        t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_OCW + 4 * i, 0xB1000000u + i);
    }
    t_m1(&g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW0, 0xF0F0F0F0u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW1, 0xF1F1F1F1u);
    t_m1(&g, WL_NV2A_K_SET_SPECULAR_FOG_FACTOR, 0xE0E0E0E0u);
    t_m1(&g, WL_NV2A_K_SET_SPECULAR_FOG_FACTOR + 4, 0xE1E1E1E1u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_CONTROL, 0x00011102u);
    t_m1(&g, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM, 0x00000021u);
    t_m1(&g, WL_NV2A_K_SET_FOG_COLOR, 0x00332211u); // ABGR
    t_kick(&g);
    wl_nv2a_psh_words(k, words);
    ASSERT_EQ(0xA0000003u, words[WL_XBOX_PSH_ALPHA_INPUTS0 + 3]);
    ASSERT_EQ(0xC0000007u, words[WL_XBOX_PSH_CONSTANT0_0 + 7]);
    ASSERT_EQ(0xC1000000u, words[WL_XBOX_PSH_CONSTANT1_0]);
    ASSERT_EQ(0xA1000005u, words[WL_XBOX_PSH_ALPHA_OUTPUTS0 + 5]);
    ASSERT_EQ(0xB0000001u, words[WL_XBOX_PSH_RGB_INPUTS0 + 1]);
    ASSERT_EQ(0xB1000006u, words[WL_XBOX_PSH_RGB_OUTPUTS0 + 6]);
    ASSERT_EQ(0xF0F0F0F0u, words[WL_XBOX_PSH_FINAL_INPUTS_ABCD]);
    ASSERT_EQ(0xF1F1F1F1u, words[WL_XBOX_PSH_FINAL_INPUTS_EFG]);
    ASSERT_EQ(0xE0E0E0E0u, words[WL_XBOX_PSH_FINAL_CONSTANT0]);
    ASSERT_EQ(0xE1E1E1E1u, words[WL_XBOX_PSH_FINAL_CONSTANT1]);
    ASSERT_EQ(0x00011102u, words[WL_XBOX_PSH_COMBINER_COUNT]);
    ASSERT_EQ(0x00000021u, words[54]);
    ASSERT_EQ(0x00112233u, wl_nv2a_fog_rgb(k));
    t_destroy(&g);
    PASS();
}

// (the pixel program's inputs as the backend reads them: the combiners' words and the textures,
// every stage bound)
static void pixel_inputs(const WlNv2aKelvin *k, uint32_t words[WL_XBOX_PSH_WORDS],
                         WlXboxPshTextures *textures) {
    static WlNv2aTexEntry entries[WL_NV2A_TEXTURES];
    const WlNv2aTexEntry *bound[WL_NV2A_TEXTURES];
    unsigned stage;

    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        entries[stage].contents = 1;
        bound[stage] = &entries[stage];
    }
    wl_nv2a_psh_words(k, words);
    wl_nv2a_psh_textures(k, bound, textures);
}

TEST every_input_of_the_pixel_program_dirties_it(void) {
    TGuest g;
    WlNv2aKelvin *k;
    uint32_t method, before[WL_XBOX_PSH_WORDS], after[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures_before, textures_after;
    unsigned inputs = 0;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = (WlNv2aKelvin *)wl_nv2a_kelvin(g.dev);
    // (every stage a 2D texture, so that the textures are read)
    t_m1(&g, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM, 0x00008421u);
    t_kick(&g);
    // each method whose value the inputs read, found by changing its value alone, then sent with
    // that value: the pixel program dirty, and nothing else of it read
    for (method = 0; method < WL_NV2A_K_METHODS; method += 4) {
        uint32_t old = k->raw[method >> 2];
        bool read;

        pixel_inputs(k, before, &textures_before);
        k->raw[method >> 2] = ~old;
        pixel_inputs(k, after, &textures_after);
        k->raw[method >> 2] = old;
        read = memcmp(before, after, sizeof before) != 0 ||
               memcmp(&textures_before, &textures_after, sizeof textures_before) != 0;
        if (!read) {
            continue;
        }
        inputs++;
        k->dirty = 0;
        t_m1(&g, method, ~old);
        t_kick(&g);
        if (!(k->dirty & WL_NV2A_DIRTY_PIXEL)) {
            FAILm("an input of the pixel program does not dirty it");
        }
        pixel_inputs(k, after, &textures_after);
        ASSERT(memcmp(before, after, sizeof before) != 0 ||
               memcmp(&textures_before, &textures_after, sizeof textures_before) != 0);
        t_m1(&g, method, old);
        t_kick(&g);
    }
    // psh's 57 words (the stage programs among them), the fog's switch and colour
    ASSERT_EQ(59u, inputs);
    // and a value written as it was: not dirty
    k->dirty = 0;
    t_m1(&g, WL_NV2A_K_SET_COMBINER_FACTOR0, k->raw[WL_NV2A_K_SET_COMBINER_FACTOR0 >> 2]);
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1u ^ k->raw[WL_NV2A_K_SET_BLEND_ENABLE >> 2]);
    t_kick(&g);
    ASSERT_EQ((uint32_t)WL_NV2A_DIRTY_RASTER, k->dirty);
    t_destroy(&g);
    PASS();
}

TEST surface_decoded(void) {
    TGuest g;
    WlNv2aSurface s;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL, 640u << 16);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_CLIP_VERTICAL, 480u << 16 | 4u);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_FORMAT,
         WL_NV2A_SURFACE_COLOR_A8R8G8B8 | WL_NV2A_SURFACE_ZETA_Z24S8 << 4 | WL_NV2A_SURFACE_TYPE_PITCH << 8);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_PITCH, 2560u << 16 | 2560u);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET, 0x200000);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_ZETA_OFFSET, 0x300000);
    t_kick(&g);
    wl_nv2a_surface(wl_nv2a_kelvin(g.dev), &s);
    ASSERT_EQ(640u, s.clip_width);
    ASSERT_EQ(480u, s.clip_height);
    ASSERT_EQ(4u, s.clip_y);
    ASSERT_EQ(0x200000u, s.color_phys);
    ASSERT_EQ(0x300000u, s.zeta_phys);
    ASSERT_EQ(4u, s.color_bytes);
    ASSERT_EQ(4u, s.zeta_bytes);
    ASSERT_EQ(2560u, s.color_pitch);
    ASSERT_EQ(0u, s.swizzled);
    ASSERT_EQ(1u, s.samples_x);
    ASSERT(near(s.depth_max, 16777215.0f));
    t_destroy(&g);
    PASS();
}

// ---- the fetch decided once a draw -------------------------------------------------------------------

static uint32_t g_rng = 0x51ED270Bu;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

enum { SLOTS = 16 };

TEST fetch_plan_matches_the_fetch_of_each_vertex(void) {
    static const uint8_t types[] = {WL_NV2A_VTX_UB_D3D, WL_NV2A_VTX_S1,   WL_NV2A_VTX_F,
                                    WL_NV2A_VTX_UB_OGL, WL_NV2A_VTX_S32K, WL_NV2A_VTX_CMP};
    static uint8_t data[64 * 1024];
    static WlNv2aKelvin k;
    static uint32_t indices[64];
    static float want[SLOTS][WL_NV2A_ATTRIBS][4], got[SLOTS][WL_NV2A_ATTRIBS][4];
    WlNv2aDraw draw;
    WlNv2aFetchPlan plan;
    unsigned trial, a, v, c;

    for (trial = 0; trial < 2000; trial++) {
        uint32_t attributes = rnd() & 0xFFFFu;

        for (c = 0; c < sizeof data; c++) {
            data[c] = (uint8_t)rnd();
        }
        for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
            for (c = 0; c < 4; c++) {
                k.current[a][c] = (float)(int)(rnd() % 2001u - 1000u) / 8.0f;
            }
        }
        memset(&draw, 0, sizeof draw);
        draw.source = (uint8_t)(rnd() % 2u == 0 ? WL_NV2A_SOURCE_ARRAYS : WL_NV2A_SOURCE_INLINE);
        draw.count = 1u + rnd() % 64u;
        draw.streamed = rnd() & 0xFFFFu;
        draw.index_base = rnd() % 16u;
        draw.indices = rnd() % 2u == 0 ? indices : NULL;
        for (v = 0; v < 64; v++) {
            indices[v] = draw.index_base + rnd() % 128u;
        }
        draw.inline_data = data;
        draw.inline_stride = 64;
        for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
            WlNv2aAttrib *attrib = &draw.attribs[a];

            attrib->type = types[rnd() % sizeof types];
            attrib->size = (uint8_t)(rnd() % 5u);
            if (attrib->type == WL_NV2A_VTX_F && rnd() % 4u == 0) {
                attrib->size = 7; // (FLOAT2H)
            }
            attrib->stride = 16u + 4u * (rnd() % 60u);
            attrib->data = data + 4u * (rnd() % 64u);
            attrib->inline_offset = (uint16_t)(4u * (rnd() % 12u));
        }
        wl_nv2a_fetch_plan(&draw, attributes, &plan);
        wl_nv2a_fetch_constants(&plan, &k, got, SLOTS);
        // (some vertices fetched together: the first SLOTS, in an order of their own)
        {
            uint32_t some[SLOTS], m = draw.count < SLOTS ? draw.count : SLOTS;

            for (v = 0; v < m; v++) {
                some[v] = (v * 7u) % m;
            }
            wl_nv2a_fetch_vertices(&plan, &draw, some, m, got);
            for (v = 0; v < m; v++) {
                wl_nv2a_draw_fetch_some(&draw, &k, some[v], attributes, want[v]);
                for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
                    if (attributes & (1u << a)) {
                        ASSERT_MEM_EQm("an attribute, fetched together", want[v][a], got[v][a], sizeof want[v][a]);
                    }
                }
            }
        }
        for (v = 0; v < draw.count; v++) {
            unsigned slot = v % SLOTS;

            wl_nv2a_draw_fetch_some(&draw, &k, v, attributes, want[slot]);
            wl_nv2a_fetch_vertex(&plan, &draw, v, got[slot]);
            for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
                if (attributes & (1u << a)) {
                    ASSERT_MEM_EQm("an attribute", want[slot][a], got[slot][a], sizeof want[slot][a]);
                }
            }
        }
    }
    PASS();
}

// ---- the push buffer's runs against a method at a time -----------------------------------------------

// random headers (increasing and not) over plain methods, the program and constant windows, their
// load slots, the viewport and the program's start and mode, kicked at random words (runs cut at
// PUT): the pusher's runs leave the 3D state as wl_nv2a_kelvin_method given each word alone does
TEST pushed_runs_match_a_method_at_a_time(void) {
    static const struct {
        uint16_t base, words;
    } ranges[] = {
        {0x0260, 12}, {0x02A0, 16}, {0x0300, 48},        {0x0440, 64}, {WL_NV2A_K_SET_VIEWPORT_OFFSET, 4},
        {0x0A60, 32}, {WL_NV2A_K_SET_VIEWPORT_SCALE, 4}, {0x0B00, 32}, {0x0B80, 32},
        {0x1720, 32}, {0x1B00, 64}, {0x1E40, 16},        {WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE, 1},
        {WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD, 1},       {WL_NV2A_K_SET_TRANSFORM_PROGRAM_START, 1},
        {WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, 1},
    };
    static const uint32_t some[] = {0, 1, 0x3F800000u, 0xFFFFFFFFu};
    TGuest a, b;
    unsigned header, n;

    ASSERT(t_create(&a, NULL) != NULL);
    ASSERT(t_create(&b, NULL) != NULL);
    t_setup_3d(&a);
    t_setup_3d(&b);
    for (header = 0; header < 4000; header++) {
        unsigned r = rnd() % (sizeof ranges / sizeof ranges[0]);
        uint32_t offset = rnd() % ranges[r].words, method = ranges[r].base + 4u * offset;
        uint32_t count = 1u + rnd() % 24u, non_increasing = rnd() % 4u == 0;

        if (!non_increasing && count > ranges[r].words - offset) {
            count = ranges[r].words - offset;
        }
        t_word(&a, (non_increasing ? 0x40000000u : 0u) | (count << 18) | method);
        for (n = 0; n < count; n++) {
            uint32_t m = non_increasing ? method : method + 4u * n, param;

            if (m == WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD || m == WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD) {
                param = rnd() % 200u;
            } else {
                param = rnd() % 3u == 0 ? some[rnd() % 4u] : rnd();
            }
            t_word(&a, param);
            wl_nv2a_kelvin_method(b.dev, m, param);
            if (rnd() % 16u == 0) {
                t_kick(&a);
            }
        }
        if (rnd() % 4u == 0) {
            t_kick(&a);
        }
    }
    t_kick(&a);
    ASSERT_EQ(a.put, t_get(&a));
    ASSERT_MEM_EQm("the 3D state", wl_nv2a_kelvin(b.dev), wl_nv2a_kelvin(a.dev), sizeof(WlNv2aKelvin));
    t_destroy(&a);
    t_destroy(&b);
    PASS();
}

SUITE(kelvin_suite) {
    RUN_TEST(every_array_format_decodes);
    RUN_TEST(draw_arrays_and_elements);
    RUN_TEST(inline_arrays_follow_the_formats);
    RUN_TEST(immediate_vertices_of_each_kind);
    RUN_TEST(long_batches_go_in_whole_primitives);
    RUN_TEST(line_loops_close);
    RUN_TEST(program_and_constant_windows);
    RUN_TEST(dirty_on_a_change_of_value);
    RUN_TEST(combiners_as_the_pixel_shader_words);
    RUN_TEST(every_input_of_the_pixel_program_dirties_it);
    RUN_TEST(surface_decoded);
    RUN_TEST(fetch_plan_matches_the_fetch_of_each_vertex);
    RUN_TEST(pushed_runs_match_a_method_at_a_time);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(kelvin_suite);
    GREATEST_MAIN_END();
}
