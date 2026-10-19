// SPDX-License-Identifier: MIT
// The GX backend's prefetch of a draw's vertices (nv2a_gx.c's draw_prefetch, Broadway's dcbt), built
// for this test with WL_NV2A_PREFETCH_RECORD: each line it asks for recorded. Every line asked for
// lies within the draw's own elements' bytes: a DRAW_ARRAYS from its start, an indexed draw from its
// least index (index_base) to its greatest; a small draw's lines each asked once; a large one's batch
// by batch; an indexed draw spread wide asks for its vertices' lines alone, none of the gap between
// them; none with the prefetch off.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "nv2a_frames.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/platform.h"
#include "wl/xbox/nv2a_gx.h"

// (the pusher's own lines ahead of its reads, nv2a_kelvin.c's, are those of the push buffer at T_PB:
// not recorded, PUSH_END and below)
// (a vertex's attributes: AREA bytes at a stride of g_stride)
enum { ARRAY = 0x80000, AREA = 24, MAX_RECORDS = 65536, PUSH_END = 0x40000 };
static uint32_t g_stride = AREA;

static const uint8_t *g_ram;
static uintptr_t g_records[MAX_RECORDS];
static unsigned g_record_count;

void wl_nv2a_prefetch_record(const void *p) {
    if (g_ram != NULL && (const uint8_t *)p >= g_ram && (const uint8_t *)p < g_ram + PUSH_END) {
        return;
    }
    if (g_record_count < MAX_RECORDS) {
        g_records[g_record_count] = (uintptr_t)p;
    }
    g_record_count++;
}

static WlNv2aGx *g_gx;

static WlNv2a *device(TGuest *g) {
    WlNv2aBackend backend;
    WlNv2aGxConfig gcfg;

    if (wl_gx_init() != WL_OK) {
        return NULL;
    }
    memset(&gcfg, 0, sizeof gcfg);
    g_gx = wl_nv2a_gx_create(&gcfg);
    if (g_gx == NULL) {
        return NULL;
    }
    wl_nv2a_gx_backend(g_gx, &backend);
    if (t_create(g, &backend) == NULL) {
        return NULL;
    }
    g_ram = g->ram;
    wl_nv2a_gx_attach(g_gx, g->dev);
    t_setup_3d(g);
    f_frame_setup(g);
    f_program(g, 0, f_screen_program, 3);
    f_clear(g, 0xFF000000u);
    t_kick(g);
    return g->dev;
}

static void device_done(TGuest *g) {
    wl_nv2a_gx_destroy(g_gx);
    g_gx = NULL;
    t_destroy(g);
    wl_gx_shutdown();
}

// `count` vertices at ARRAY (position F3, colour D3DCOLOR, texture coordinate F2: AREA bytes of `stride` a
// vertex), vertex v a corner of a small triangle at the screen's (v % 32) * 16, (v / 32 % 24) * 16
static void arrays(TGuest *g, uint32_t count, uint32_t stride) {
    uint32_t v;

    g_stride = stride;

    for (v = 0; v < count; v++) {
        uint32_t at = ARRAY + stride * v, cell = v / 3u;
        float x = (float)(cell % 32u * 16u + (v % 3u == 1u ? 12u : 0u));
        float y = (float)(cell / 32u % 24u * 16u + (v % 3u == 2u ? 12u : 0u));

        t_w32(g, at, t_f(x));
        t_w32(g, at + 4, t_f(y));
        t_w32(g, at + 8, t_f(0.5f * 16777215.0f));
        t_w32(g, at + 12, 0xFFFFFFFFu);
        t_w32(g, at + 16, t_f(0.0f));
        t_w32(g, at + 20, t_f(0.0f));
    }
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT, stride << 8 | 3u << 4 | WL_NV2A_VTX_F);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET, ARRAY);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * 3, stride << 8 | 4u << 4 | WL_NV2A_VTX_UB_D3D);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * 3, ARRAY + 12);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * 9, stride << 8 | 2u << 4 | WL_NV2A_VTX_F);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * 9, ARRAY + 16);
    t_kick(g);
}

// triangles of `count` vertices from `start` drawn from the arrays (a DRAW_ARRAYS word for each 240
// at most: its count is 8 bits, as Direct3D splits a draw)
static void draw_arrays(TGuest *g, uint32_t start, uint32_t count) {
    uint32_t done, n;

    g_record_count = 0;
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    for (done = 0; done < count; done += n) {
        n = count - done < 240u ? count - done : 240u;
        t_m1(g, WL_NV2A_K_DRAW_ARRAYS, (n - 1u) << 24 | (start + done));
    }
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(g);
}

// triangles of `count` vertices by their 32-bit indices
static void draw_indexed(TGuest *g, const uint32_t *indices, uint32_t count) {
    uint32_t i;

    g_record_count = 0;
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    for (i = 0; i < count; i++) {
        t_m1(g, WL_NV2A_K_ARRAY_ELEMENT32, indices[i]);
    }
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(g);
}

// the guest offset of a record
static uint32_t offset_of(unsigned i) {
    return (uint32_t)(g_records[i] - (uintptr_t)g_ram);
}

// the cache line of a host address (guest RAM is the host's calloc: its lines are the host's)
static uintptr_t line_of(uintptr_t p) {
    return p & ~(uintptr_t)(WL_NV2A_CACHE_LINE - 1);
}

// every record within the elements lo .. hi's bytes, from the line of lo's first; the lines of lo's
// first byte and hi's last among them
static enum greatest_test_res within(uint32_t lo, uint32_t hi) {
    uintptr_t first = (uintptr_t)(g_ram + ARRAY + g_stride * lo);
    uintptr_t last = (uintptr_t)(g_ram + ARRAY + g_stride * hi + AREA - 1u);
    bool saw_first = false, saw_last = false;
    unsigned i;

    ASSERT(g_record_count > 0u && g_record_count <= MAX_RECORDS);
    for (i = 0; i < g_record_count; i++) {
        ASSERT(g_records[i] >= line_of(first));
        ASSERT(g_records[i] <= last);
        saw_first |= line_of(g_records[i]) == line_of(first);
        saw_last |= line_of(g_records[i]) == line_of(last);
    }
    ASSERT(saw_first);
    ASSERT(saw_last);
    PASS();
}

// no line asked for twice
static enum greatest_test_res each_line_once(void) {
    unsigned i, j;

    for (i = 0; i < g_record_count; i++) {
        for (j = i + 1u; j < g_record_count; j++) {
            ASSERT(line_of(g_records[i]) != line_of(g_records[j]));
        }
    }
    PASS();
}

TEST a_draw_of_the_arrays_asks_for_its_own_vertices_lines(void) {
    TGuest g;

    ASSERT(device(&g) != NULL);
    arrays(&g, 64, AREA);
    // vertices 10 .. 21: the bytes 240 .. 527 of the array (its lines 224 .. 512)
    draw_arrays(&g, 10, 12);
    CHECK_CALL(within(10, 21));
    CHECK_CALL(each_line_once());
    device_done(&g);
    PASS();
}

TEST an_indexed_draw_asks_from_its_least_index_to_its_greatest(void) {
    static const uint32_t indices[] = {45, 40, 47, 51, 44, 49, 42, 50, 41, 46, 48, 43};
    TGuest g;

    ASSERT(device(&g) != NULL);
    arrays(&g, 64, AREA);
    draw_indexed(&g, indices, 12);
    CHECK_CALL(within(40, 51));
    CHECK_CALL(each_line_once());
    device_done(&g);
    PASS();
}

TEST an_indexed_draw_spread_wide_asks_for_its_vertices_lines_alone(void) {
    static const uint32_t indices[] = {0, 1, 2, 3000, 3001, 3002};
    TGuest g;
    unsigned i;

    ASSERT(device(&g) != NULL);
    arrays(&g, 3003, AREA);
    draw_indexed(&g, indices, 6);
    CHECK_CALL(within(0, 3002));
    // (none in the gap: 72 KB that no vertex of the draw reads)
    for (i = 0; i < g_record_count; i++) {
        uint32_t at = offset_of(i);

        ASSERT(at < ARRAY + AREA * 3u || g_records[i] >= line_of((uintptr_t)(g_ram + ARRAY + AREA * 3000u)));
    }
    ASSERT(g_record_count <= 4u * 6u);
    device_done(&g);
    PASS();
}

// (a draw of more bytes than L1 is to hold at once, PREFETCH_WHOLE: asked for a batch at a time,
// each batch's lines from its least element to its greatest; the device hands every draw of the
// arrays over with its indices, DRAW_ARRAYS's too, so the plain draw's range is never taken)
TEST a_large_draw_asks_batch_by_batch_within_its_elements(void) {
    static uint32_t indices[960];
    TGuest g;
    uint32_t i, start = 7;

    ASSERT(device(&g) != NULL);
    arrays(&g, 2000, AREA);
    // (the draw's last byte the last of a line: a vertex asked for beyond it is a line beyond)
    while ((uintptr_t)(g_ram + ARRAY + AREA * (start + 960u)) % WL_NV2A_CACHE_LINE != 0u && start < 11u) {
        start++;
    }
    draw_arrays(&g, start, 960);
    CHECK_CALL(within(start, start + 959u));
    // (each line once but those two batches share)
    ASSERT(g_record_count <= (960u * AREA) / 32u + 2u + 960u / 16u);
    for (i = 0; i < 960u; i++) {
        indices[i] = 1000u + (i ^ 5u);
    }
    draw_indexed(&g, indices, 960);
    CHECK_CALL(within(1000, 1959));
    ASSERT(g_record_count <= (960u * AREA) / 32u + 2u + 960u / 16u);
    device_done(&g);
    PASS();
}

TEST none_asked_with_the_prefetch_off(void) {
    TGuest g;

    ASSERT(device(&g) != NULL);
    arrays(&g, 64, AREA);
    wl_nv2a_set_prefetch(false);
    draw_arrays(&g, 10, 12);
    wl_nv2a_set_prefetch(true);
    ASSERT_EQ(0u, g_record_count);
    device_done(&g);
    PASS();
}

SUITE(prefetch_suite) {
    RUN_TEST(a_draw_of_the_arrays_asks_for_its_own_vertices_lines);
    RUN_TEST(an_indexed_draw_asks_from_its_least_index_to_its_greatest);
    RUN_TEST(an_indexed_draw_spread_wide_asks_for_its_vertices_lines_alone);
    RUN_TEST(a_large_draw_asks_batch_by_batch_within_its_elements);
    RUN_TEST(none_asked_with_the_prefetch_off);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(prefetch_suite);
    GREATEST_MAIN_END();
}
