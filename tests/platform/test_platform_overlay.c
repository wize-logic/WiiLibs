// SPDX-License-Identifier: MIT
// The frame overlay (wl_gx_set_overlay) and its drawing helpers (wl/platform/overlay.h) on the host
// GX stub: the hook runs once a frame inside end_frame with the state reset around it, costs nothing
// when unset or removed, and the rectangles, outlines, gauges and text come out as the quads the font
// says.
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/overlay.h"
#include "wl/platform/platform.h"

static WlHostGxDraw g_draws[64];
static unsigned g_calls;
static uint32_t g_w, g_h;
static WlGxBlend g_blend_inside;
static bool g_ortho_inside;
static void (*g_draw)(void);
static bool g_user_ok;
static int g_text_width;

static void overlay(void *user, uint32_t w, uint32_t h) {
    g_calls++;
    g_w = w;
    g_h = h;
    g_blend_inside = wl_host_gx_state()->blend;
    g_ortho_inside = wl_host_gx_state()->ortho;
    g_user_ok = user == (void *)&g_calls;
    if (g_draw != NULL) {
        wl_overlay_begin();
        g_draw();
        wl_overlay_end();
    }
}

static void frame(void) {
    wl_gx_begin_frame(0);
    wl_gx_end_frame();
}

static void start(void) {
    wl_gx_set_overlay(NULL, NULL);
    (void)wl_gx_init();
    g_calls = 0;
    g_draw = NULL;
    memset(g_draws, 0, sizeof g_draws);
    wl_host_gx_record(g_draws, 64);
}

static void finish(void) {
    wl_gx_set_overlay(NULL, NULL);
    wl_host_gx_record(NULL, 0);
    wl_gx_shutdown();
}

TEST the_hook_runs_once_a_frame_with_the_state_reset_and_is_removable(void) {
    start();
    frame();
    ASSERT_EQ(0u, g_calls); // none set
    wl_gx_set_overlay(overlay, &g_calls);
    wl_gx_begin_frame(0);
    wl_gx_set_blend(WL_GX_BLEND_ADD); // (whatever the frame left)
    wl_gx_end_frame();
    ASSERT_EQ(1u, g_calls);
    ASSERT(g_user_ok);
    ASSERT_EQ(640u, g_w);
    ASSERT_EQ(480u, g_h);
    ASSERT_EQ(WL_GX_BLEND_OFF, g_blend_inside);
    ASSERT(g_ortho_inside);
    ASSERT_EQ(WL_GX_BLEND_OFF, wl_host_gx_state()->blend); // reset after too
    frame();
    frame();
    ASSERT_EQ(3u, g_calls);
    wl_gx_set_overlay(NULL, NULL);
    frame();
    ASSERT_EQ(3u, g_calls);
    finish();
    PASS();
}

static void draw_rects(void) {
    wl_overlay_rect(10, 20, 30, 40, 0x11223344u);
    wl_overlay_rect(0, 0, 0, 5, 0xFFFFFFFFu);        // (empty: nothing)
    wl_overlay_rect(0, 0, 5, 5, 0xFFFFFF00u);        // (transparent: nothing)
}

TEST a_rectangle_is_a_quad_and_empty_or_clear_ones_are_not_drawn(void) {
    start();
    g_draw = draw_rects;
    wl_gx_set_overlay(overlay, &g_calls);
    frame();
    ASSERT_EQ(1u, wl_host_gx_recorded());
    ASSERT_EQ(WL_GX_QUADS, g_draws[0].prim);
    ASSERT_EQ(4u, g_draws[0].count);
    ASSERT_EQ_FMT(10.0f, g_draws[0].x0, "%f");
    ASSERT_EQ_FMT(40.0f, g_draws[0].x1, "%f");
    ASSERT_EQ_FMT(20.0f, g_draws[0].y0, "%f");
    ASSERT_EQ_FMT(60.0f, g_draws[0].y1, "%f");
    ASSERT_EQ(0x11223344u, g_draws[0].rgba);
    ASSERT_EQ(WL_GX_RECIPE_VERTEX_COLOR, g_draws[0].recipe);
    finish();
    PASS();
}

static void draw_frame_and_gauge(void) {
    wl_overlay_frame(0, 0, 100, 50, 2, 0xFF0000FFu);
    wl_overlay_gauge(0, 60, 100, 10, 0.25f, 0x00FF00FFu, 0x000000FFu);
    wl_overlay_gauge(0, 80, 100, 10, 7.0f, 0x00FF00FFu, 0x000000FFu); // (clamped to full)
}

TEST an_outline_is_four_rectangles_and_a_gauge_fills_its_fraction(void) {
    start();
    g_draw = draw_frame_and_gauge;
    wl_gx_set_overlay(overlay, &g_calls);
    frame();
    ASSERT_EQ(1u, wl_host_gx_recorded());
    // 4 (outline) + 2 + 2 (gauges) quads, one batch
    ASSERT_EQ(32u, g_draws[0].count);
    ASSERT_EQ_FMT(0.0f, g_draws[0].x0, "%f");
    ASSERT_EQ_FMT(100.0f, g_draws[0].x1, "%f");
    ASSERT_EQ_FMT(90.0f, g_draws[0].y1, "%f");
    finish();
    PASS();
}

TEST glyphs_are_the_font_and_lower_case_folds_to_capitals(void) {
    const uint8_t *a = wl_overlay_glyph('A');
    ASSERT_EQ(0x0E, a[0]);
    ASSERT_EQ(0x1F, a[3]);
    ASSERT(wl_overlay_glyph('a') == a);
    const uint8_t *unknown = wl_overlay_glyph('~');
    ASSERT_EQ(0x1F, unknown[0]);
    ASSERT_EQ(0x11, unknown[3]);
    // every letter and digit differs from the box, and from every other
    for (char c = '0'; c <= 'Z'; c++) {
        if (c > '9' && c < 'A') {
            continue;
        }
        const uint8_t *g = wl_overlay_glyph(c);
        ASSERT(memcmp(g, unknown, 7) != 0);
        for (char d = c + 1; d <= 'Z'; d++) {
            if (d > '9' && d < 'A') {
                continue;
            }
            ASSERT(memcmp(g, wl_overlay_glyph(d), 7) != 0);
        }
        for (int row = 0; row < 7; row++) {
            ASSERT(g[row] < 32); // five columns
        }
    }
    PASS();
}

TEST text_is_drawable_when_every_character_has_a_glyph(void) {
    ASSERT(wl_overlay_text_drawable(""));
    ASSERT(wl_overlay_text_drawable("A: CHOOSE   B OR HOME: RESUME"));
    ASSERT(wl_overlay_text_drawable("lower case is folded\nand a newline is fine (100%)"));
    ASSERT(!wl_overlay_text_drawable("a tilde ~ is a box"));
    ASSERT(!wl_overlay_text_drawable("{port}"));
    ASSERT(!wl_overlay_text_drawable("tab\there"));
    ASSERT(!wl_overlay_text_drawable("\xC3\xA9")); // UTF-8 is not in the font
    PASS();
}

TEST text_measures_by_cell_and_scale(void) {
    ASSERT_EQ(0, wl_overlay_text_width(2, ""));
    ASSERT_EQ(5, wl_overlay_text_width(1, "A"));               // 5 pixels of the cell's 6
    ASSERT_EQ(11, wl_overlay_text_width(1, "AB"));
    ASSERT_EQ(22, wl_overlay_text_width(2, "AB"));
    ASSERT_EQ(22, wl_overlay_text_width(2, "AB\nA"));          // the longest line
    ASSERT_EQ(7, wl_overlay_text_height(1, "A"));
    ASSERT_EQ(16, wl_overlay_text_height(1, "A\nB"));          // 9 pixels a line
    ASSERT_EQ(wl_overlay_text_width(8, "A"), wl_overlay_text_width(99, "A")); // (the scale is clamped)
    ASSERT_EQ(wl_overlay_text_width(1, "A"), wl_overlay_text_width(-3, "A"));
    PASS();
}

static void draw_text_i(void) {
    g_text_width = wl_overlay_text(10, 20, 2, 0xFFFFFFFFu, "I");
}

TEST text_is_runs_of_pixels_with_a_shadow_beneath(void) {
    start();
    g_draw = draw_text_i;
    wl_gx_set_overlay(overlay, &g_calls);
    frame();
    ASSERT_EQ(1u, wl_host_gx_recorded());
    ASSERT_EQ(5 * 2, g_text_width);
    // I is a run of three, five of one and a run of three: seven quads, twice (shadow and text)
    ASSERT_EQ(7u * 2u * 4u, g_draws[0].count);
    ASSERT_EQ_FMT(10.0f + 2.0f, g_draws[0].x0, "%f");            // the glyph's first column is empty on row 1
    ASSERT_EQ_FMT(10.0f + 4.0f * 2.0f + 2.0f, g_draws[0].x1, "%f"); // ...and the shadow reaches 2 further
    ASSERT_EQ_FMT(20.0f, g_draws[0].y0, "%f");
    ASSERT_EQ_FMT(20.0f + 7.0f * 2.0f + 2.0f, g_draws[0].y1, "%f");
    ASSERT_EQ(0x0000007Fu, g_draws[0].rgba);                      // the shadow first: black at half the opacity
    finish();
    PASS();
}

static void draw_many(void) {
    char line[64];
    memset(line, 'W', sizeof line - 1u);
    line[sizeof line - 1u] = '\0';
    wl_overlay_text(0, 0, 1, 0xFFFFFFFFu, line);
}

TEST a_full_batch_is_sent_and_the_rest_follows(void) {
    start();
    g_draw = draw_many;
    wl_gx_set_overlay(overlay, &g_calls);
    frame();
    // 63 W: 4 runs of ... a few quads a row: more than one batch of 128 quads
    ASSERT(wl_host_gx_recorded() >= 2u);
    uint32_t quads = 0;
    for (uint32_t i = 0; i < wl_host_gx_recorded() && i < 64u; i++) {
        ASSERT_EQ(WL_GX_QUADS, g_draws[i].prim);
        ASSERT(g_draws[i].count <= 128u * 4u);
        ASSERT_EQ(0u, g_draws[i].count % 4u);
        quads += g_draws[i].count / 4u;
    }
    // W: rows 10001 (two runs) x3, 10101 (three) x3, 01010 (two): 3*2 + 3*3 + 2 = 17 quads, two passes
    ASSERT_EQ(63u * 17u * 2u, quads);
    finish();
    PASS();
}

SUITE(overlay_suite) {
    RUN_TEST(the_hook_runs_once_a_frame_with_the_state_reset_and_is_removable);
    RUN_TEST(a_rectangle_is_a_quad_and_empty_or_clear_ones_are_not_drawn);
    RUN_TEST(an_outline_is_four_rectangles_and_a_gauge_fills_its_fraction);
    RUN_TEST(glyphs_are_the_font_and_lower_case_folds_to_capitals);
    RUN_TEST(text_is_drawable_when_every_character_has_a_glyph);
    RUN_TEST(text_measures_by_cell_and_scale);
    RUN_TEST(text_is_runs_of_pixels_with_a_shadow_beneath);
    RUN_TEST(a_full_batch_is_sent_and_the_rest_follows);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(overlay_suite);
    GREATEST_MAIN_END();
}
