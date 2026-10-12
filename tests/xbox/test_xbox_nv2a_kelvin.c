// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_kelvin: the 3D state. Every vertex array format decoded (D3DCOLOR's byte order,
// normalized and plain shorts, floats and FLOAT2H, bytes, the packed normal); draws from arrays by
// DRAW_ARRAYS, ARRAY_ELEMENT16 and ARRAY_ELEMENT32, from inline arrays and from immediate vertices
// (each kind of SET_VERTEX_DATA*), disabled attributes reading their current value; long batches
// handed over in whole primitives; the vertex program and constant windows; the viewport in
// c[58]/c[59]; the combiners as psh's 57 words; the surface decoded.
#include <math.h>
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"

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

SUITE(kelvin_suite) {
    RUN_TEST(every_array_format_decodes);
    RUN_TEST(draw_arrays_and_elements);
    RUN_TEST(inline_arrays_follow_the_formats);
    RUN_TEST(immediate_vertices_of_each_kind);
    RUN_TEST(long_batches_go_in_whole_primitives);
    RUN_TEST(line_loops_close);
    RUN_TEST(program_and_constant_windows);
    RUN_TEST(combiners_as_the_pixel_shader_words);
    RUN_TEST(surface_decoded);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(kelvin_suite);
    GREATEST_MAIN_END();
}
