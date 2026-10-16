// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_vcache: the vertex arrays kept for GX to read by index. Each kind converted as the
// fetch widens it, to the bit (positions, texture coordinates of every format, colours); an array
// found again while unchanged, its source hashed once a frame, or answered by a write watch;
// rewritten once, converted again; rewritten twice, no longer kept; two ranges of an array met as
// one entry; what does not fit the budget evicting the least recently drawn, never one the frame drew.
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "wl/xbox/nv2a_color.h"
#include "wl/xbox/nv2a_vcache.h"

enum { ARRAY = 0x20000, STRIDE = 20, VERTICES = 64 };

static TGuest g_guest;

static WlNv2a *guest(void) {
    unsigned v;

    if (t_create(&g_guest, NULL) == NULL) {
        return NULL;
    }
    // (a position F3, a D3DCOLOR, two normalized shorts: 20 bytes a vertex)
    for (v = 0; v < VERTICES; v++) {
        uint32_t at = ARRAY + STRIDE * v;

        t_w32(&g_guest, at, t_f((float)v * 1.25f - 7.0f));
        t_w32(&g_guest, at + 4, t_f(1.0f / (float)(v + 3)));
        t_w32(&g_guest, at + 8, t_f(-(float)v * 0.001f));
        t_w32(&g_guest, at + 12, 0x80000000u | v * 0x00030507u);
        t_w32(&g_guest, at + 16, (uint32_t)(uint16_t)(int16_t)(v * 511 - 16000) |
                                     (uint32_t)(uint16_t)(int16_t)(32767 - v * 977) << 16);
    }
    return g_guest.dev;
}

// the attribute at byte `offset` of the vertices, for draws of indices first ..
static WlNv2aAttrib attrib(uint32_t offset, uint32_t first, uint8_t type, uint8_t size) {
    WlNv2aAttrib a;

    memset(&a, 0, sizeof a);
    a.phys = ARRAY + offset + first * STRIDE;
    a.data = g_guest.ram + a.phys;
    a.stride = STRIDE;
    a.type = type;
    a.size = size;
    return a;
}

static uint32_t be(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t bits(float f) {
    uint32_t b;

    memcpy(&b, &f, sizeof b);
    return b;
}

TEST each_kind_converted_as_the_fetch_widens_it(void) {
    WlNv2aVCache *cache = calloc(1, sizeof *cache);
    WlNv2aVCacheConfig cfg;
    WlNv2aAttrib position, color, coordinate;
    const uint8_t *p, *c, *t;
    unsigned v;

    ASSERT(cache != NULL && guest() != NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.budget = 1u << 20;
    wl_nv2a_vcache_init(cache, &cfg);
    position = attrib(0, 4, WL_NV2A_VTX_F, 3);
    color = attrib(12, 4, WL_NV2A_VTX_UB_D3D, 4);
    coordinate = attrib(16, 4, WL_NV2A_VTX_S1, 2);
    p = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 4, 40, WL_NV2A_VCACHE_POSITION);
    c = wl_nv2a_vcache_get(cache, g_guest.dev, &color, 4, 40, WL_NV2A_VCACHE_COLOR);
    t = wl_nv2a_vcache_get(cache, g_guest.dev, &coordinate, 4, 40, WL_NV2A_VCACHE_TEXCOORD);
    ASSERT(p != NULL && c != NULL && t != NULL);
    for (v = 4; v <= 40; v++) {
        float want[4];
        const uint8_t *src = g_guest.ram + ARRAY + STRIDE * v;

        wl_nv2a_attrib_decode(WL_NV2A_VTX_F, 3, src, want);
        ASSERT_EQ(bits(want[0]), be(p + 12 * (v - 4)));
        ASSERT_EQ(bits(want[1]), be(p + 12 * (v - 4) + 4));
        ASSERT_EQ(bits(want[2]), be(p + 12 * (v - 4) + 8));
        wl_nv2a_attrib_decode(WL_NV2A_VTX_UB_D3D, 4, src + 12, want);
        ASSERT_EQ(wl_nv2a_color_word(want), be(c + 4 * (v - 4)));
        wl_nv2a_attrib_decode(WL_NV2A_VTX_S1, 2, src + 16, want);
        ASSERT_EQ(bits(want[0]), be(t + 8 * (v - 4)));
        ASSERT_EQ(bits(want[1]), be(t + 8 * (v - 4) + 4));
    }
    // (a position that is not three floats, a coordinate of three components: not kept)
    position.size = 2;
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &position, 4, 40, WL_NV2A_VCACHE_POSITION) == NULL);
    coordinate = attrib(0, 4, WL_NV2A_VTX_F, 3);
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &coordinate, 4, 40, WL_NV2A_VCACHE_TEXCOORD) == NULL);
    wl_nv2a_vcache_clear(cache);
    t_destroy(&g_guest);
    free(cache);
    PASS();
}

TEST found_again_checked_once_a_frame_rewritten_twice_not_kept(void) {
    WlNv2aVCache *cache = calloc(1, sizeof *cache);
    WlNv2aVCacheConfig cfg;
    WlNv2aVCacheStats s;
    WlNv2aAttrib position;
    const uint8_t *p, *again;

    ASSERT(cache != NULL && guest() != NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.budget = 1u << 20;
    wl_nv2a_vcache_init(cache, &cfg);
    position = attrib(0, 0, WL_NV2A_VTX_F, 3);
    p = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION);
    ASSERT(p != NULL);
    ASSERT(wl_nv2a_vcache_made(cache));
    ASSERT(!wl_nv2a_vcache_made(cache));
    // (the same frame, a range within it: as it was, not looked at)
    position = attrib(0, 8, WL_NV2A_VTX_F, 3);
    again = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 8, 20, WL_NV2A_VCACHE_POSITION);
    ASSERT_EQ(p + 12 * 8, again);
    wl_nv2a_vcache_stats(cache, &s);
    ASSERT_EQ(1u, s.converted);
    ASSERT_EQ(0u, s.checks);
    // the next frame: hashed, unchanged
    wl_nv2a_vcache_frame_done(cache);
    position = attrib(0, 0, WL_NV2A_VTX_F, 3);
    ASSERT_EQ(p, wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION));
    wl_nv2a_vcache_stats(cache, &s);
    ASSERT_EQ(1u, s.checks);
    ASSERT_EQ(0u, s.reconverted);
    ASSERT(!wl_nv2a_vcache_made(cache));
    // rewritten: converted again (in place: the frame has not drawn it yet)
    wl_nv2a_vcache_frame_done(cache);
    t_w32(&g_guest, ARRAY + STRIDE * 3, t_f(99.0f));
    ASSERT_EQ(p, wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION));
    ASSERT_EQ(bits(99.0f), be(p + 12 * 3));
    ASSERT(wl_nv2a_vcache_made(cache));
    // rewritten again: geometry the title rewrites, sent as before from then on
    wl_nv2a_vcache_frame_done(cache);
    t_w32(&g_guest, ARRAY + STRIDE * 3, t_f(98.0f));
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION) == NULL);
    wl_nv2a_vcache_frame_done(cache);
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION) == NULL);
    wl_nv2a_vcache_stats(cache, &s);
    ASSERT_EQ(1u, s.reconverted);
    ASSERT(s.refused >= 2u);
    ASSERT_EQ(0u, (unsigned)s.bytes);
    wl_nv2a_vcache_clear(cache);
    t_destroy(&g_guest);
    free(cache);
    PASS();
}

// gpu_lag 1 (frames overlapped): an array drawn in the frame before may still be read by the GPU,
// so its rewrite goes elsewhere, and the old one is given back only a frame later
TEST overlapped_frames_keep_the_frame_befores_arrays(void) {
    WlNv2aVCache *cache = calloc(1, sizeof *cache);
    WlNv2aVCacheConfig cfg;
    WlNv2aAttrib position;
    const uint8_t *p, *moved;

    ASSERT(cache != NULL && guest() != NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.budget = 1u << 20;
    cfg.gpu_lag = 1;
    wl_nv2a_vcache_init(cache, &cfg);
    position = attrib(0, 0, WL_NV2A_VTX_F, 3);
    p = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION);
    ASSERT(p != NULL);
    wl_nv2a_vcache_frame_done(cache);
    // rewritten: the GPU may still be drawing the frame before from it
    t_w32(&g_guest, ARRAY + STRIDE * 3, t_f(99.0f));
    moved = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION);
    ASSERT(moved != NULL && moved != p);
    ASSERT_EQ(bits(99.0f), be(moved + 12 * 3));
    ASSERT_EQ(bits(-7.0f + 1.25f * 3.0f), be(p + 12 * 3));
    ASSERT_EQ(1u, cache->pending_count);
    wl_nv2a_vcache_frame_done(cache);
    ASSERT_EQ(1u, cache->pending_count); // (the GPU may be drawing the frame that read it)
    wl_nv2a_vcache_frame_done(cache);
    ASSERT_EQ(0u, cache->pending_count);
    wl_nv2a_vcache_clear(cache);
    t_destroy(&g_guest);
    free(cache);
    PASS();
}

// a write watch whose pages are written when the test says
static uint32_t g_epoch = 1, g_written_epoch;

static uint32_t watch(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    (void)phys;
    (void)bytes;
    return g_epoch;
}

static bool written(void *user, uint32_t phys, uint32_t bytes, uint32_t stamp) {
    (void)user;
    (void)phys;
    (void)bytes;
    return g_written_epoch > stamp;
}

TEST the_watch_answers_for_an_array_not_written(void) {
    WlNv2aVCache *cache = calloc(1, sizeof *cache);
    WlNv2aVCacheConfig cfg;
    WlNv2aVCacheStats s;
    WlNv2aAttrib coordinate;
    const uint8_t *t;

    ASSERT(cache != NULL && guest() != NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.budget = 1u << 20;
    cfg.watch = watch;
    cfg.written = written;
    wl_nv2a_vcache_init(cache, &cfg);
    coordinate = attrib(16, 0, WL_NV2A_VTX_S1, 2);
    t = wl_nv2a_vcache_get(cache, g_guest.dev, &coordinate, 0, 63, WL_NV2A_VCACHE_TEXCOORD);
    ASSERT(t != NULL);
    wl_nv2a_vcache_frame_done(cache);
    ASSERT_EQ(t, wl_nv2a_vcache_get(cache, g_guest.dev, &coordinate, 0, 63, WL_NV2A_VCACHE_TEXCOORD));
    wl_nv2a_vcache_stats(cache, &s);
    ASSERT_EQ(1u, s.watched);
    ASSERT_EQ(1u, s.hashed); // (its making's alone)
    // pages written (the bytes the same): hashed, as it was
    g_written_epoch = ++g_epoch;
    g_epoch++;
    wl_nv2a_vcache_frame_done(cache);
    ASSERT_EQ(t, wl_nv2a_vcache_get(cache, g_guest.dev, &coordinate, 0, 63, WL_NV2A_VCACHE_TEXCOORD));
    wl_nv2a_vcache_stats(cache, &s);
    ASSERT_EQ(1u, s.watched);
    ASSERT_EQ(2u, s.hashed);
    ASSERT_EQ(0u, s.reconverted);
    wl_nv2a_vcache_clear(cache);
    t_destroy(&g_guest);
    free(cache);
    PASS();
}

TEST two_ranges_met_as_one_and_the_least_recent_evicted(void) {
    WlNv2aVCache *cache = calloc(1, sizeof *cache);
    WlNv2aVCacheConfig cfg;
    WlNv2aVCacheStats s;
    WlNv2aAttrib low, high, color;
    const uint8_t *a, *b;

    ASSERT(cache != NULL && guest() != NULL);
    memset(&cfg, 0, sizeof cfg);
    // (room for one position array of 64 vertices and a little: 768 + 32 bytes)
    cfg.budget = 64u * 12u + 32u;
    wl_nv2a_vcache_init(cache, &cfg);
    low = attrib(0, 0, WL_NV2A_VTX_F, 3);
    high = attrib(0, 40, WL_NV2A_VTX_F, 3);
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &low, 0, 9, WL_NV2A_VCACHE_POSITION) != NULL);
    wl_nv2a_vcache_frame_done(cache);
    // (another range of the same array: one entry over both, 0 .. 63)
    b = wl_nv2a_vcache_get(cache, g_guest.dev, &high, 40, 63, WL_NV2A_VCACHE_POSITION);
    ASSERT(b != NULL);
    a = wl_nv2a_vcache_get(cache, g_guest.dev, &low, 0, 9, WL_NV2A_VCACHE_POSITION);
    ASSERT_EQ(a + 12 * 40, b);
    wl_nv2a_vcache_stats(cache, &s);
    ASSERT_EQ(2u, s.converted);
    ASSERT_EQ(1u, s.evicted);
    // a colour array does not fit beside it while the frame draws it: refused, nothing evicted
    color = attrib(12, 0, WL_NV2A_VTX_UB_D3D, 4);
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &color, 0, 63, WL_NV2A_VCACHE_COLOR) == NULL);
    ASSERT_EQ(a, wl_nv2a_vcache_get(cache, g_guest.dev, &low, 0, 9, WL_NV2A_VCACHE_POSITION));
    // the next frame: the positions, not drawn yet, make room
    wl_nv2a_vcache_frame_done(cache);
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &color, 0, 63, WL_NV2A_VCACHE_COLOR) != NULL);
    wl_nv2a_vcache_stats(cache, &s);
    ASSERT_EQ(2u, s.evicted);
    ASSERT(s.bytes <= cfg.budget);
    // (beyond 16-bit indices: not kept)
    ASSERT(wl_nv2a_vcache_get(cache, g_guest.dev, &low, 0, 70000, WL_NV2A_VCACHE_POSITION) == NULL);
    wl_nv2a_vcache_clear(cache);
    t_destroy(&g_guest);
    free(cache);
    PASS();
}

// gpu_lag 1 with a write watch, and no room for the rewrite beside the array the GPU may still read:
// not kept this time, and never the old contents afterwards (the check's watch is not trusted then)
TEST a_rewrite_without_room_is_not_answered_stale(void) {
    WlNv2aVCache *cache = calloc(1, sizeof *cache);
    WlNv2aVCacheConfig cfg;
    WlNv2aVCacheStats s;
    WlNv2aAttrib position;
    const uint8_t *p;

    ASSERT(cache != NULL && guest() != NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.budget = 1u << 20;
    cfg.gpu_lag = 1;
    cfg.watch = watch;
    cfg.written = written;
    wl_nv2a_vcache_init(cache, &cfg);
    position = attrib(0, 0, WL_NV2A_VTX_F, 3);
    p = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION);
    ASSERT(p != NULL);
    wl_nv2a_vcache_stats(cache, &s);
    cache->cfg.budget = s.bytes; // (room for this array alone)
    wl_nv2a_vcache_frame_done(cache);
    t_w32(&g_guest, ARRAY + STRIDE * 3, t_f(99.0f));
    g_written_epoch = ++g_epoch;
    g_epoch++;
    ASSERT_EQ(NULL, wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION));
    // (again in the frame, and in the next: the watch has seen no store since the check)
    p = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION);
    ASSERT(p == NULL || be(p + 12 * 3) == bits(99.0f));
    wl_nv2a_vcache_frame_done(cache);
    p = wl_nv2a_vcache_get(cache, g_guest.dev, &position, 0, 31, WL_NV2A_VCACHE_POSITION);
    ASSERT(p == NULL || be(p + 12 * 3) == bits(99.0f));
    wl_nv2a_vcache_clear(cache);
    t_destroy(&g_guest);
    free(cache);
    PASS();
}

SUITE(vcache_suite) {
    RUN_TEST(each_kind_converted_as_the_fetch_widens_it);
    RUN_TEST(found_again_checked_once_a_frame_rewritten_twice_not_kept);
    RUN_TEST(overlapped_frames_keep_the_frame_befores_arrays);
    RUN_TEST(the_watch_answers_for_an_array_not_written);
    RUN_TEST(a_rewrite_without_room_is_not_answered_stale);
    RUN_TEST(two_ranges_met_as_one_and_the_least_recent_evicted);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vcache_suite);
    GREATEST_MAIN_END();
}
