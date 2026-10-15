// SPDX-License-Identifier: MIT
// The on-console test runner: the platform-independent suites (tests/platform/suite_threads.c,
// suite_fs.c, suite_posix.c, tests/audio/suite_sseq.c) plus a GX/video smoke test and the NV2A backend
// on GX (tests/xbox/suite_nv2a.c), on the real Wii backends.
// Built for WL_PLATFORM=wii as wl_console_tests.dol; tools/dolphin_run.py runs it in Dolphin
// (ctest -L dolphin), and it runs the same from the Homebrew Channel on hardware.
//
// Every line goes through the log sink (OSREPORT, and sd:/wltest/logs when there is a card).
// The run ends with "WLT RESULT pass=N fail=M" and "WLT DONE"; a failed case is a line
// containing " FAIL <name>: <detail>". Afterwards the screen stays green (all passed) or red
// until HOME, START or the reset button.
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/backtrace.h"
#include "wl/platform/gx_target.h"
#include "wl/platform/heap_overflow.h"
#include "wl/platform/mem.h"
#include "wl/platform/net.h"
#include "wl/platform/platform.h"
#include "wl/platform/profile.h"
#include "wl/platform/tev.h"
#include "tests/platform/check.h"

#define TAG "wlt"

static int g_pass;
static int g_fail;

void wlt_report(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    WL_LOG_I(TAG, "%s", line);
}

void wlt_check(const char *name, bool ok, const char *detail) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        WL_LOG_E(TAG, " FAIL %s: %s", name, detail != NULL ? detail : "-");
    }
}

void wlt_suite_threads(void);
void wlt_suite_fs(void);
void wlt_suite_posix(const char *path);
void wlt_suite_sseq(void);
void wlt_suite_sqrt(void);
void wlt_suite_nv2a(void);

// Close enough for an EFB colour: the EFB keeps 6 or 8 bits a channel depending on the mode.
static bool near_rgb(uint32_t got, uint32_t want) {
    for (int shift = 8; shift < 32; shift += 8) {
        int a = (int)((got >> shift) & 0xffu);
        int b = (int)((want >> shift) & 0xffu);
        if (a - b > 8 || b - a > 8) {
            return false;
        }
    }
    return true;
}

static void check_video_gx(void) {
    wlt_check("video_init", wl_platform_video_init() == WL_OK, "wl_platform_video_init");
    WlVideoInfo info;
    wl_platform_video_info(&info);
    wlt_report("INFO video %ux%u (xfb %u) pal=%d progressive=%d widescreen=%d pillarbox=%d picture=%u+%u", (unsigned)info.fb_width,
               (unsigned)info.efb_height, (unsigned)info.xfb_height, info.pal, info.progressive,
               info.widescreen, info.pillarbox, (unsigned)info.picture_x, (unsigned)info.picture_width);
    {
        // (what the settings ask for, from the console's own configuration, against the mode in use)
        WlVideoInfo want;
        wl_platform_video_expected(&want);
        wlt_check("video_mode_as_set", want.fb_width == info.fb_width && want.efb_height == info.efb_height &&
                                           want.xfb_height == info.xfb_height && want.pal == info.pal &&
                                           want.progressive == info.progressive && want.pillarbox == info.pillarbox,
                  "wl_platform_video_expected against wl_platform_video_info");
        char line[200];
        wl_platform_video_describe(line, sizeof line);
        wlt_report("INFO video: %s", line);
    }
    wlt_check("gx_init", wl_gx_init() == WL_OK && wl_gx_ready(), "wl_gx_init");

    // a cleared frame, then a triangle over the centre: both read back from the EFB
    const uint32_t clear = 0x204080ffu;
    const uint32_t green = 0x20e040ffu;
    // (begin_frame's default state: the 2D canvas, y down, z -1..1, vertex colour)
    wl_gx_begin_frame(clear);
    float cx = (float)info.fb_width / 2.0f, cy = (float)info.efb_height / 2.0f;
    const WlGxVertex tri[3] = {
        {cx - 100.0f, cy + 80.0f, 0.0f, green, 0.0f, 0.0f},
        {cx, cy - 100.0f, 0.0f, green, 0.0f, 0.0f},
        {cx + 100.0f, cy + 80.0f, 0.0f, green, 0.0f, 0.0f},
    };
    wl_gx_draw(WL_GX_TRIANGLES, tri, 3);
    uint32_t corner = wl_gx_peek_rgba(4, 4);
    uint32_t centre = wl_gx_peek_rgba((int)cx, (int)cy);
    char detail[96];
    snprintf(detail, sizeof detail, "corner 0x%08x, want 0x%08x", (unsigned)corner, (unsigned)clear);
    wlt_check("gx_clear_readback", near_rgb(corner, clear), detail);
    snprintf(detail, sizeof detail, "centre 0x%08x, want 0x%08x", (unsigned)centre, (unsigned)green);
    wlt_check("gx_triangle_readback", near_rgb(centre, green), detail);
    wl_gx_end_frame();

    // presenting paces to the retrace
    uint32_t r0 = wl_platform_video_retrace_count();
    for (int i = 0; i < 10; i++) {
        wl_gx_begin_frame(clear);
        wl_gx_end_frame();
    }
    uint32_t retraces = wl_platform_video_retrace_count() - r0;
    snprintf(detail, sizeof detail, "%u retraces over 10 frames", (unsigned)retraces);
    wlt_check("video_present_paced", retraces >= 9u && retraces <= 30u, detail);

    // presenting does not wait for the display: with a free XFB, a frame's end returns as soon as
    // the GPU has drawn it (the retrace wait it replaced was most of a retrace right after one)
    uint64_t slowest = 0;
    for (int i = 0; i < 5; i++) {
        wl_platform_video_wait_shown(); // (returns just after a retrace)
        wl_gx_begin_frame(clear);
        uint64_t t0 = wl_platform_time_us();
        wl_gx_end_frame();
        uint64_t us = wl_platform_time_us() - t0;
        slowest = us > slowest ? us : slowest;
    }
    snprintf(detail, sizeof detail, "slowest end_frame %u us", (unsigned)slowest);
    wlt_check("video_present_returns_at_once", slowest < 4000u, detail);

    // an interval of two (30 Hz): frames given faster are held, each shown exactly two retraces,
    // in order, none skipped and none held longer
    WlVideoStats v0, v1;
    wl_platform_video_set_interval(2);
    wl_platform_video_wait_shown();
    wl_platform_video_stats(&v0);
    for (int i = 0; i < 10; i++) {
        wl_gx_begin_frame(clear);
        wl_gx_end_frame();
    }
    bool shown = wl_platform_video_wait_shown();
    wl_platform_video_stats(&v1);
    wl_platform_video_set_interval(1);
    snprintf(detail, sizeof detail, "%u shown, %u late, last gap %u, %u retraces", (unsigned)(v1.shown - v0.shown),
             (unsigned)(v1.late - v0.late), (unsigned)v1.last_gap, (unsigned)(v1.retraces - v0.retraces));
    wlt_check("video_interval_two", shown && v1.shown - v0.shown == 10u && v1.late == v0.late && v1.last_gap == 2u &&
                                        v1.retraces - v0.retraces >= 19u && v1.retraces - v0.retraces <= 23u,
              detail);

    // frame overlap: end_frame returns before the GPU finishes; the frame retires behind it
    wl_gx_set_frame_overlap(true);
    uint32_t index = wl_gx_frame_index();
    wl_gx_begin_frame(clear);
    wl_gx_draw(WL_GX_TRIANGLES, tri, 3);
    wl_gx_end_frame();
    bool waited = wl_gx_frame_wait(index);
    bool retired = wl_gx_frame_retired(index) && !wl_gx_frame_retired(wl_gx_frame_index());
    wl_gx_set_frame_overlap(false);
    wlt_check("gx_frame_overlap_retires", waited && retired, "frame_wait, then retired (the next not)");
    WlGxStats stats;
    wl_gx_stats(&stats);
    wlt_check("gx_no_timeouts", stats.gpu_timeouts == 0u, "the GPU finished every frame");
    wlt_check("gx_no_validation_errors", stats.validation_errors == 0u, "no invalid GX calls");
}

// Render targets (wl/platform/gx_target.h): a triangle in the back buffer, a target that takes the
// EFB and is cleared red, then the back buffer back. Read back: the target is red while it has the
// EFB; its image in its memory is red in GX's RGBA8 tiles; the back buffer has its clear colour where
// the target was drawn and its triangle, and the depth the triangle wrote (a second triangle at the
// same depth fails a LESS test only if the parked depth was written back through the Z texture).
static uint8_t g_target_image[64 * 64 * 4] __attribute__((aligned(32)));

static void check_render_target(void) {
    WlVideoInfo info;
    wl_platform_video_info(&info);
    const uint32_t clear = 0x204080ffu;
    const uint32_t green = 0x20e040ffu;
    const uint32_t blue = 0x2040e0ffu;
    const uint32_t red = 0xe02020ffu;
    char detail[128];

    memset(g_target_image, 0xEE, sizeof g_target_image);
    wl_gpu_buffer_commit(g_target_image, sizeof g_target_image);
    WlGxTarget target = {g_target_image, g_target_image, 64, 64, WL_GX_TEX_RGBA8, 7, false};

    float cx = (float)info.fb_width / 2.0f, cy = (float)info.efb_height / 2.0f;
    const WlGxVertex tri_green[3] = {
        {cx - 100.0f, cy + 80.0f, 0.0f, green, 0.0f, 0.0f},
        {cx, cy - 100.0f, 0.0f, green, 0.0f, 0.0f},
        {cx + 100.0f, cy + 80.0f, 0.0f, green, 0.0f, 0.0f},
    };
    WlGxVertex tri_blue[3];
    memcpy(tri_blue, tri_green, sizeof tri_blue);
    for (int i = 0; i < 3; i++) {
        tri_blue[i].rgba = blue;
    }

    wl_gx_begin_frame(clear);
    wl_gx_set_zmode(true, WL_GX_LEQUAL, true);
    wl_gx_draw(WL_GX_TRIANGLES, tri_green, 3);
    wl_gx_target_drawn();

    // the target takes the EFB (the back buffer is parked) and is cleared red at the EFB's origin
    const WlGxTarget *done = wl_gx_target_use(&target);
    wlt_check("gx_target_taking_resolves_nothing", done == NULL, "the back buffer left, no target resolved");
    wl_gx_target_clear(true, true, red, 1.0f, 0, 0, 64, 64);
    uint32_t inside = wl_gx_peek_rgba(10, 10);
    snprintf(detail, sizeof detail, "pixel 0x%08x, want 0x%08x", (unsigned)inside, (unsigned)red);
    wlt_check("gx_target_clear_readback", near_rgb(inside, red), detail);

    // the back buffer back: the target's image is copied out first
    done = wl_gx_target_use(NULL);
    wlt_check("gx_target_resolved_on_leaving", done != NULL && done->user == 7u && done->memory == g_target_image,
              "the target comes back with the caller's word");
    // RGBA8 tiles of 4 x 4: alpha and red of the 16 pixels, then green and blue. The copy is a
    // command the GPU has yet to run when wl_gx_target_use returns (the layer resolves for GX's
    // own reads, which follow it in the FIFO): the CPU reads it once the GPU is done, and past
    // its cache.
    (void)wl_gx_sync();
    wl_gpu_buffer_invalidate(g_target_image, sizeof g_target_image);
    snprintf(detail, sizeof detail, "a %02x r %02x g %02x b %02x, want ff e0 20 20", g_target_image[0],
             g_target_image[1], g_target_image[32], g_target_image[33]);
    wlt_check("gx_target_image_in_gx_format",
              near_rgb(((uint32_t)g_target_image[1] << 24) | ((uint32_t)g_target_image[32] << 16) |
                           ((uint32_t)g_target_image[33] << 8) | g_target_image[0],
                       red),
              detail);

    uint32_t corner = wl_gx_peek_rgba(10, 10);
    uint32_t centre = wl_gx_peek_rgba((int)cx, (int)cy);
    snprintf(detail, sizeof detail, "corner 0x%08x, want 0x%08x", (unsigned)corner, (unsigned)clear);
    wlt_check("gx_target_back_buffer_colour_restored", near_rgb(corner, clear), detail);
    snprintf(detail, sizeof detail, "centre 0x%08x, want 0x%08x", (unsigned)centre, (unsigned)green);
    wlt_check("gx_target_back_buffer_triangle_restored", near_rgb(centre, green), detail);

    // the depth: a triangle at the same depth fails a LESS test where the first one wrote it
    wl_gx_set_zmode(true, WL_GX_LESS, true);
    wl_gx_draw(WL_GX_TRIANGLES, tri_blue, 3);
    centre = wl_gx_peek_rgba((int)cx, (int)cy);
    snprintf(detail, sizeof detail, "centre 0x%08x, want 0x%08x (blue 0x%08x if the depth was lost)", (unsigned)centre,
             (unsigned)green, (unsigned)blue);
    wlt_check("gx_target_back_buffer_depth_restored", near_rgb(centre, green), detail);
    wl_gx_end_frame();

    // a target left with the EFB at the end of the frame is replaced by the back buffer
    wl_gx_begin_frame(clear);
    wl_gx_target_drawn();
    (void)wl_gx_target_use(&target);
    wl_gx_end_frame();
    wlt_check("gx_target_end_frame_restores_back_buffer", wl_gx_target_current() == NULL, "the back buffer owns the EFB");

    WlGxTargetStats st;
    wl_gx_target_stats(&st, true);
    snprintf(detail, sizeof detail, "%u parks, %u restores, %u resolves, %u skipped, %u clears", (unsigned)st.parks,
             (unsigned)st.restores, (unsigned)st.resolves, (unsigned)st.skipped, (unsigned)st.clears);
    wlt_check("gx_target_counts", st.parks == 2u && st.restores == 2u && st.resolves == 2u && st.skipped == 0u &&
                                      st.clears == 1u,
              detail);
    WlGxStats gs;
    wl_gx_stats(&gs);
    wlt_check("gx_target_no_validation_errors", gs.validation_errors == 0u, "no invalid GX calls");
}

// A TEV program with no texture (konst colours, three stages, REG0 and REG1, a SUB with a bias and
// a scale of two) drawn on the real GX and read back from the EFB against the software evaluator
// (wl_tev_eval): the hardware and the model must agree, within the EFB's precision.
static void check_tev_program(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    int k0 = wl_tev_konst_slot(&p, 0x000000A0u);
    int k1 = wl_tev_konst_slot(&p, 0x80FF40C8u);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_COLOR0A0);
    s->kcsel = wl_tev_kcsel((uint32_t)k1, WL_TEV_K_RGB);
    s->kasel = wl_tev_kasel((uint32_t)k1, WL_TEV_K_A);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_RASC, WL_TEV_CC_KONST, WL_TEV_CC_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_REG0);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_RASA, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_REG0);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_COLOR0A0);
    s->kcsel = wl_tev_kcsel((uint32_t)k0, WL_TEV_K_A);
    s->kasel = wl_tev_kasel((uint32_t)k0, WL_TEV_K_A);
    wl_tev_stage_color(s, WL_TEV_CC_RASC, WL_TEV_CC_C0, WL_TEV_CC_KONST, WL_TEV_CC_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_ZERO, WL_TEV_SCALE_2, true, WL_TEV_REG1);
    wl_tev_stage_alpha(s, WL_TEV_CA_RASA, WL_TEV_CA_A0, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_REG1);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_color(s, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C1, WL_TEV_SUB,
                       WL_TEV_BIAS_ADDHALF, WL_TEV_SCALE_1, true, WL_TEV_PREV);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A1, WL_TEV_ADD,
                       WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
    WlTevReport rep;
    wlt_check("tev_program_valid", wl_tev_validate(&p, &rep) && rep.errors == 0u, "the test program validates");

    const uint32_t clear = 0x000000ffu;
    const uint32_t vertex = 0x806432ffu;
    WlTevPixel px;
    memset(&px, 0, sizeof px);
    px.ras[0][0] = 0x80;
    px.ras[0][1] = 0x64;
    px.ras[0][2] = 0x32;
    px.ras[0][3] = 0xFF;
    const uint32_t want = wl_tev_eval_rgba(&p, &px);

    WlVideoInfo info;
    wl_platform_video_info(&info);
    float cx = (float)info.fb_width / 2.0f, cy = (float)info.efb_height / 2.0f;
    const WlGxVertex quad[4] = {
        {cx - 60.0f, cy - 60.0f, 0.0f, vertex, 0.0f, 0.0f},
        {cx + 60.0f, cy - 60.0f, 0.0f, vertex, 0.0f, 0.0f},
        {cx + 60.0f, cy + 60.0f, 0.0f, vertex, 0.0f, 0.0f},
        {cx - 60.0f, cy + 60.0f, 0.0f, vertex, 0.0f, 0.0f},
    };
    wl_gx_begin_frame(clear);
    wl_gx_set_recipe(WL_GX_RECIPE_VERTEX_COLOR); // one colour channel, no texture coordinates
    WlResult applied = wl_tev_apply(&p);
    wl_gx_invalidate_shadow(); // (the program bypassed wl_gx's state)
    wl_gx_draw(WL_GX_QUADS, quad, 4);
    uint32_t got = wl_gx_peek_rgba((int)cx, (int)cy);
    wl_gx_invalidate_shadow(); // (so the next frame writes its recipe again)
    wl_gx_end_frame();
    char detail[96];
    wlt_check("tev_apply", applied == WL_OK, "wl_tev_apply");
    snprintf(detail, sizeof detail, "read back 0x%08x, evaluator 0x%08x", (unsigned)got, (unsigned)want);
    wlt_check("tev_readback_matches_evaluator", near_rgb(got, want), detail);
}

// 64-bit atomics (wl/platform/wii/atomic_wii.c: the compiler calls them, there being no
// 8-byte atomic instructions), between this thread and another adding at the same time
#define ATOMIC_ROUNDS 20000u
static _Atomic uint64_t g_atomic64;

static void *atomic_adder(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < ATOMIC_ROUNDS; i++) {
        atomic_fetch_add(&g_atomic64, 0x100000001ull);
    }
    return NULL;
}

static void check_atomics64(void) {
    atomic_store(&g_atomic64, 0xFFFFFFFFull);
    wlt_check("atomic64_store_load", atomic_load(&g_atomic64) == 0xFFFFFFFFull, "store then load");
    uint64_t expected = 0xFFFFFFFFull;
    bool swapped = atomic_compare_exchange_strong(&g_atomic64, &expected, 0x123456789ull);
    wlt_check("atomic64_cas", swapped && atomic_load(&g_atomic64) == 0x123456789ull, "compare-exchange that matches");
    expected = 1;
    swapped = atomic_compare_exchange_strong(&g_atomic64, &expected, 2);
    wlt_check("atomic64_cas_fail", !swapped && expected == 0x123456789ull, "one that does not reports the value");
    wlt_check("atomic64_exchange", atomic_exchange(&g_atomic64, 0) == 0x123456789ull, "exchange returns the old");

    void *stack = WL_MEM_ALLOC_GPU(WL_TAG_STACK, 16u * 1024u);
    WlPlatformThread thread;
    bool started = stack != NULL &&
                   wl_platform_thread_create(&thread, atomic_adder, NULL, stack, 16u * 1024u, WL_THREAD_PRIO_IO) == WL_OK;
    atomic_adder(NULL);
    if (started) {
        wl_platform_thread_join(thread, NULL);
    }
    uint64_t want = (started ? 2u : 1u) * (uint64_t)ATOMIC_ROUNDS * 0x100000001ull;
    wlt_check("atomic64_contended_add", atomic_load(&g_atomic64) == want, "every add from both threads counted");
    wl_mem_free(stack);
}

// thread-local variables (wl/platform/wii/tls_wii.c) as clang's -femulated-tls reaches them: a
// control block per variable, and __emutls_get_address for this thread's copy. (devkitPPC's GCC
// makes _Thread_local native TLS, which the console cannot run: see platform.h.)
struct wlt_emutls_object {
    uintptr_t size, align;
    uintptr_t offset;
    void *templ;
};
void *__emutls_get_address(struct wlt_emutls_object *object);
static int g_tls_template = 7;
static struct wlt_emutls_object g_tls_object = {sizeof(int), sizeof(int), 0, &g_tls_template};
static int g_tls_seen_by_thread;

static void *tls_thread(void *arg) {
    (void)arg;
    int *value = __emutls_get_address(&g_tls_object);
    g_tls_seen_by_thread = *value; // the template's, not the main thread's
    *value = 99;
    wl_platform_tls_thread_exit();
    return NULL;
}

static void check_tls(void) {
    int *mine = __emutls_get_address(&g_tls_object);
    *mine = 42;
    void *stack = WL_MEM_ALLOC_GPU(WL_TAG_STACK, 16u * 1024u);
    WlPlatformThread thread;
    bool started = stack != NULL &&
                   wl_platform_thread_create(&thread, tls_thread, NULL, stack, 16u * 1024u, WL_THREAD_PRIO_IO) == WL_OK;
    if (started) {
        wl_platform_thread_join(thread, NULL);
    }
    wlt_check("tls_template", started && g_tls_seen_by_thread == 7, "a new thread starts with the template's value");
    wlt_check("tls_own_copy", *(int *)__emutls_get_address(&g_tls_object) == 42,
              "another thread's write is not this one's");
    wl_mem_free(stack);
}

// wl_platform_backtrace on the real stack: its addresses are the compiler's callers' (three
// calls that stay calls), they lie in the executable's MEM1, and the walk ends by itself.
static uint32_t g_bt_level2_returns_to;
static bool g_bt_matches;
static uint32_t g_bt_count;

__attribute__((noinline)) static void bt_level3(void) {
    uint32_t out[4] = {0, 0, 0, 0};
    g_bt_count = wl_platform_backtrace(out, 4, 0);
    g_bt_matches = out[1] == (uint32_t)(uintptr_t)__builtin_return_address(0) && out[2] == g_bt_level2_returns_to;
    uint32_t skipped[2] = {0, 0};
    g_bt_matches = g_bt_matches && wl_platform_backtrace(skipped, 2, 1) == 2 && skipped[0] == out[1] &&
                   skipped[1] == out[2];
}

__attribute__((noinline)) static void bt_level2(void) {
    g_bt_level2_returns_to = (uint32_t)(uintptr_t)__builtin_return_address(0);
    bt_level3();
    __asm__ volatile("");
}

__attribute__((noinline)) static void bt_level1(void) {
    bt_level2();
    __asm__ volatile("");
}

static void check_backtrace(void) {
    uint32_t out[64];
    bt_level1();
    wlt_check("backtrace_callers", g_bt_count == 4 && g_bt_matches, "return addresses match __builtin_return_address");
    uint32_t count = wl_platform_backtrace(out, 64, 0);
    bool ok = count >= 2 && count < 64; // (check_backtrace may be inlined into main: main, then the start-up code)
    for (uint32_t i = 0; i < count; i++) {
        ok = ok && out[i] >= 0x80000000u && out[i] < 0x81800000u && (out[i] & 3u) == 0;
    }
    wlt_check("backtrace_main_stack", ok, "the main thread's chain ends in MEM1 code, by itself");
    uint32_t sp;
    __asm__ volatile("mr %0, 1" : "=r"(sp));
    count = wl_platform_backtrace_from(sp, 0x80001234u, out, 4);
    wlt_check("backtrace_from", count >= 2 && out[0] == 0x80001234u, "a context's link register comes first");
    // a stack pointer out of MEM1 and MEM2 ends the walk at once
    wlt_check("backtrace_bad_sp", wl_platform_backtrace_from(0x00001000u, 0, out, 4) == 0 &&
                                      wl_platform_backtrace_from(0xc0000000u, 0, out, 4) == 0,
              "unreadable addresses are not followed");
    wl_platform_log_backtrace("wlt", "backtrace_log(1)", 0);
}


// The sampling profiler (profile.h) on the real decrementer: 1000 samples a second over a busy
// loop that does nothing else. The samples must land in the loop (its function's first 0x80
// bytes), the loop being a leaf, name its caller by the link register, arrive about a millisecond
// apart but not evenly, carry the thread they interrupted (a second thread runs while the main
// one sleeps), and stop when stopped. The dump goes to the log as WLPROF/WLPS lines, which
// `tools/wl_profile.py wl_console_tests.elf <log>` reads. Under Dolphin the time base is the
// emulated one, and a sample lands at the end of a translated block of code, not at the
// instruction that was running; the loop is a block of its own, so the check is the same.
#define PROF_STORAGE_SAMPLES 600u
#define PROF_DEPTH 6u
static uint32_t g_prof_storage[PROF_STORAGE_SAMPLES * (WL_PROFILE_HEADER_WORDS + PROF_DEPTH)];
static uint8_t g_prof_stack[16384] __attribute__((aligned(32)));
static volatile bool g_prof_worker_stop;
static volatile uint32_t g_prof_sink;

__attribute__((noinline)) static uint32_t prof_busy(uint32_t n) {
    uint32_t x = 1;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        __asm__ volatile("" : "+r"(x));
    }
    return x;
}

// (the worker's own loop: a function of its own, so its samples can be told from the main thread's)
__attribute__((noinline)) static uint32_t prof_busy_worker(uint32_t n) {
    uint32_t x = 3;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 22695477u + 1u;
        __asm__ volatile("" : "+r"(x));
    }
    return x;
}

__attribute__((noinline)) static uint32_t prof_outer(void) {
    uint32_t r = prof_busy(400000u); // (a few milliseconds a call)
    __asm__ volatile("" : "+r"(r));
    return r;
}

static void *prof_worker(void *arg) {
    (void)arg;
    while (!g_prof_worker_stop) {
        g_prof_sink += prof_busy_worker(100000u);
    }
    return NULL;
}

static bool prof_in(uint32_t address, const void *function) {
    uint32_t start = (uint32_t)(uintptr_t)function;
    return address >= start && address < start + 0x80u;
}

static void check_profile(void) {
    WlProfileRing ring;
    WlPlatformThread worker;
    uint32_t last_threads[4] = {0};
    uint32_t thread_count = 0;
    uint32_t in_busy = 0, in_worker = 0, caller_named = 0, distinct_gaps = 0;
    uint32_t previous = 0, previous_gap = 0, gap_sum = 0, gaps = 0;
    uint32_t end_count;
    char detail[128];

    // what sampling costs: the same work, alone and with 10000 samples a second into an overwriting ring
    {
        uint64_t t = wl_platform_time_us();
        g_prof_sink += prof_busy(6000000u);
        uint64_t alone = wl_platform_time_us() - t;
        uint64_t sampled = 0;
        if (wl_profile_ring_init(&ring, g_prof_storage, (uint32_t)(sizeof g_prof_storage / 4u), PROF_DEPTH,
                                 WL_PROFILE_OVERWRITE) == WL_OK && wl_profile_start(&ring, 10000) == WL_OK) {
            t = wl_platform_time_us();
            g_prof_sink += prof_busy(6000000u);
            sampled = wl_platform_time_us() - t;
            wl_profile_stop();
        }
        wlt_report("INFO profile cost: %u us alone, %u us at 10000 samples a second (%u samples): %d.%d%% more",
                   (unsigned)alone, (unsigned)sampled, (unsigned)ring.total,
                   alone ? (int)(((int64_t)sampled - (int64_t)alone) * 1000 / (int64_t)alone) / 10 : 0,
                   alone ? (int)(((int64_t)sampled - (int64_t)alone) * 1000 / (int64_t)alone) % 10 : 0);
    }
    wlt_check("profile_init", wl_profile_ring_init(&ring, g_prof_storage, (uint32_t)(sizeof g_prof_storage / 4u),
                                                   PROF_DEPTH, WL_PROFILE_STOP_WHEN_FULL) == WL_OK, "wl_profile_ring_init");
    wlt_check("profile_bad_rate", wl_profile_start(&ring, 0) == WL_ERR_ARG && wl_profile_start(&ring, 10001) == WL_ERR_ARG &&
                                      !wl_profile_running(), "a rate of 0 or over 10000 is refused");
    wlt_check("profile_start", wl_profile_start(&ring, 1000) == WL_OK && wl_profile_running(), "wl_profile_start");
    wlt_check("profile_start_twice", wl_profile_start(&ring, 500) == WL_ERR_STATE, "one profile at a time");
    g_prof_worker_stop = false;
    bool worker_ok = wl_platform_thread_create(&worker, prof_worker, NULL, g_prof_stack, sizeof g_prof_stack,
                                               WL_THREAD_PRIO_IO) == WL_OK;
    wlt_check("profile_worker", worker_ok, "a worker below the main thread");
    // 300 ms of the loop, then 100 ms asleep, which lets the worker run
    uint64_t t0 = wl_platform_time_us();
    while (wl_platform_time_us() - t0 < 300000u) {
        g_prof_sink += prof_outer();
    }
    wl_platform_thread_sleep_ms(100);
    wl_profile_stop();
    wlt_check("profile_stopped", !wl_profile_running(), "wl_profile_stop");
    end_count = ring.count;
    wl_platform_thread_sleep_ms(30);
    wlt_check("profile_no_samples_after_stop", ring.count == end_count && ring.total == end_count, "stopped means stopped");
    g_prof_worker_stop = true;
    if (worker_ok) {
        wl_platform_thread_join(worker, NULL);
    }

    for (uint32_t i = 0; i < ring.count; i++) {
        const uint32_t *s = wl_profile_ring_get(&ring, i);
        uint32_t gap = i > 0 ? s[0] - previous : 0;
        bool known = false;
        if (prof_in(s[2], (const void *)prof_busy)) {
            in_busy++;
            if (prof_in(s[3], (const void *)prof_outer)) {
                caller_named++;
            }
        } else if (prof_in(s[2], (const void *)prof_busy_worker)) {
            in_worker++;
        }
        for (uint32_t t = 0; t < thread_count; t++) {
            known |= last_threads[t] == s[1];
        }
        if (!known && thread_count < 4u) {
            last_threads[thread_count++] = s[1];
        }
        if (i > 0) {
            gap_sum += gap;
            gaps++;
            distinct_gaps += gap != previous_gap && i > 1u;
        }
        previous = s[0];
        previous_gap = gap;
    }
    snprintf(detail, sizeof detail, "%u samples (%u dropped), %u in the busy loop, %u in the worker's, %u threads",
             (unsigned)ring.count, (unsigned)ring.dropped, (unsigned)in_busy, (unsigned)in_worker, (unsigned)thread_count);
    wlt_report("INFO profile %s", detail);
    wlt_check("profile_samples_taken", ring.count >= 150u && ring.count <= PROF_STORAGE_SAMPLES, detail);
    wlt_check("profile_lands_in_the_loop", in_busy * 10u >= ring.count * 6u,
              "most of the samples (the loop ran 300 of the 400 ms) are in the loop's function");
    wlt_check("profile_names_the_caller", in_busy > 0 && caller_named * 10u >= in_busy * 9u,
              "a leaf's link register is its caller's call site");
    wlt_check("profile_sees_both_threads", thread_count >= 2u && in_worker > 0,
              "the worker's samples carry another thread and its own function");
    if (gaps > 0) {
        uint32_t mean_us = (uint32_t)(((uint64_t)gap_sum * 1000000u / wl_profile_timebase_hz()) / gaps);
        snprintf(detail, sizeof detail, "mean gap %u us, %u of %u gaps differ from the one before", (unsigned)mean_us,
                 (unsigned)distinct_gaps, (unsigned)gaps);
        wlt_report("INFO profile %s", detail);
        wlt_check("profile_rate", mean_us >= 800u && mean_us <= 1400u, detail);
        wlt_check("profile_jitter", distinct_gaps * 4u >= gaps, detail);
    }
    // the dump, for the symbolizer; and a file when there is a card
    {
        WlProfileDumpInfo info = {.hz = 1000, .main_thread = 0, .note = "wl_console_tests check_profile"};
        uint32_t lines = wl_profile_dump_log(&ring, &info);
        wlt_check("profile_dump", lines == ring.count + 1u, "a header and a line a sample");
        if (wl_platform_fs_mounted(WL_DEVICE_SD) && wl_platform_fs_mkdir("sd:/wltest") == WL_OK) {
            WlResult r = wl_profile_dump_file(&ring, &info, "sd:/wltest/profile.txt");
            wlt_report("INFO profile file dump sd:/wltest/profile.txt: %s", wl_result_name(r));
            wlt_check("profile_dump_file", r == WL_OK, "wl_profile_dump_file on the card");
        }
    }
    wlt_report("INFO profile busy loop at %08x, its caller at %08x, the worker's loop at %08x",
               (unsigned)(uintptr_t)prof_busy, (unsigned)(uintptr_t)prof_outer, (unsigned)(uintptr_t)prof_busy_worker);
}

// net.h over IOS: the network comes up (or does not: then the socket checks are skipped, as on a
// console with no access point), and this machine's own address carries a UDP datagram and a TCP
// stream, polled, as the host tests do over loopback.
static void check_net(void) {
    char text[16];

    wlt_check("net_start", wl_net_start() == WL_OK && wl_net_state() != WL_NET_IDLE, "wl_net_start");
    WlResult up = wl_net_wait(20000);
    wl_net_format_address(wl_net_local_address(), text);
    wlt_report("INFO net: %s, address %s", up == WL_OK ? "up" : wl_net_result_name(up), text);
    wlt_check("net_down_is_reported", up == WL_OK || wl_net_state() != WL_NET_UP, "state follows the wait");
    if (up != WL_OK) {
        WlSocket s;
        wlt_check("net_socket_when_down", wl_net_socket(WL_NET_UDP, &s) == WL_ERR_NET_DOWN,
                  "sockets say the network is down");
        return;
    }
    WlNetAddr self = {wl_net_local_address(), 0}, bound, from;
    WlSocket a = WL_SOCKET_INVALID, b = WL_SOCKET_INVALID;
    uint8_t out[4] = {1, 2, 3, 4}, in[8];
    bool ok = wl_net_socket(WL_NET_UDP, &a) == WL_OK && wl_net_socket(WL_NET_UDP, &b) == WL_OK &&
              wl_net_bind(a, &self) == WL_OK && wl_net_bind(b, &self) == WL_OK &&
              wl_net_local_addr(b, &bound) == WL_OK && bound.port != 0 && bound.address == self.address;
    wlt_check("net_udp_bind", ok, "two UDP sockets bound on this machine's address");
    if (ok) {
        WlNetAddr to = bound, from_a;
        WlNetPoll waiting = {b, WL_NET_POLL_READ, 0};
        uint16_t a_port = wl_net_local_addr(a, &from_a) == WL_OK ? from_a.port : 0;
        wlt_check("net_udp_send", wl_net_sendto(a, out, 4, &to) == 4, "sendto this machine");
        wlt_check("net_udp_poll", wl_net_poll(&waiting, 1, 2000) == 1 && (waiting.revents & WL_NET_POLL_READ),
                  "the datagram arrives");
        // (libogc's net_ioctl answers FIONBIO only: unsupported is the console's answer)
        uint32_t available = 0;
        WlResult counted = wl_net_bytes_available(b, &available);
        wlt_check("net_udp_available", counted == WL_ERR_UNSUPPORTED || (counted == WL_OK && available == 4),
                  "bytes available: unsupported on the console, or 4");
        memset(&from, 0, sizeof from);
        int32_t got = wl_net_recvfrom(b, in, sizeof in, &from);
        wlt_report("INFO net_udp_recv got=%d first=%02x from=%08x:%u self=%08x", (int)got, (unsigned)in[0],
                   (unsigned)from.address, (unsigned)from.port, (unsigned)self.address);
        wlt_check("net_udp_recv", got == 4 && memcmp(in, out, 4) == 0 && from.address == self.address &&
                                      from.port == a_port,
                  "the datagram, with its sender");
        wl_net_set_nonblocking(b, true);
        wlt_check("net_udp_again", wl_net_recvfrom(b, in, sizeof in, &from) == WL_ERR_AGAIN, "nothing more: try later");
    }
    if (a != WL_SOCKET_INVALID) {
        wl_net_close(a);
    }
    if (b != WL_SOCKET_INVALID) {
        wl_net_close(b);
    }
    WlSocket listener = WL_SOCKET_INVALID, client = WL_SOCKET_INVALID, server = WL_SOCKET_INVALID;
    WlNetAddr listening, peer;
    ok = wl_net_socket(WL_NET_TCP, &listener) == WL_OK && wl_net_bind(listener, &self) == WL_OK &&
         wl_net_listen(listener, 2) == WL_OK && wl_net_local_addr(listener, &listening) == WL_OK;
    wlt_check("net_tcp_listen", ok, "a TCP listener on this machine's address");
    if (ok) {
        ok = wl_net_socket(WL_NET_TCP, &client) == WL_OK && wl_net_set_option(client, WL_NET_OPT_NODELAY, 1) == WL_OK &&
             wl_net_connect(client, &listening) == WL_OK && wl_net_accept(listener, &server, &peer) == WL_OK &&
             peer.address == self.address;
        wlt_check("net_tcp_connect", ok, "connect and accept");
    }
    if (ok) {
        wlt_check("net_tcp_send", wl_net_send(client, "hello", 5) == 5, "five bytes out");
        int32_t got = wl_net_recv(server, in, sizeof in);
        wlt_check("net_tcp_recv", got == 5 && memcmp(in, "hello", 5) == 0, "five bytes in");
        wl_net_shutdown(client, WL_NET_SHUT_WRITE);
        wlt_check("net_tcp_eof", wl_net_recv(server, in, sizeof in) == 0, "the end of the stream");
    }
    for (int i = 0; i < 3; i++) {
        WlSocket close_me = i == 0 ? listener : i == 1 ? client : server;
        if (close_me != WL_SOCKET_INVALID) {
            wl_net_close(close_me);
        }
    }
    uint32_t localhost = 0;
    wlt_check("net_resolve_dotted", wl_net_resolve("10.9.8.7", &localhost) == WL_OK && localhost == WL_NET_ADDR(10, 9, 8, 7),
              "a dotted address needs no lookup");
}

// ---- malloc past what MEM1 (and the bottom of MEM2) holds: the overflow heap ---------------------
// The runner is linked with wl_platform_heap_overflow, so malloc is wrapped; with the tag armed,
// newlib's heap filling up sends the next blocks to the tag's heap in MEM2. Checks the addresses
// (inside that heap, in MEM2, 32-byte aligned), that contents survive realloc in both directions
// and free, and that nothing is left behind.
#define OVERFLOW_CHUNK (256u * 1024u)
#define OVERFLOW_MAX_BLOCKS 320u
static void *g_overflow_blocks[OVERFLOW_MAX_BLOCKS];

static void overflow_stamp(void *p, size_t n, uint8_t seed) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i += 4093u) { // (sparse: the point is the addresses, not the bandwidth)
        b[i] = (uint8_t)(seed + i);
    }
    b[n - 1u] = (uint8_t)(seed ^ 0xA5u);
}

static bool overflow_stamp_ok(const void *p, size_t n, uint8_t seed) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i += 4093u) {
        if (b[i] != (uint8_t)(seed + i)) {
            return false;
        }
    }
    return b[n - 1u] == (uint8_t)(seed ^ 0xA5u);
}

static bool in_mem2(const void *p) {
    return (uintptr_t)p >= 0x90000000u && (uintptr_t)p < 0x94000000u;
}

static void check_heap_overflow(WlMemTag tag) {
    char detail[128];
    wlt_check("heap_overflow_enable", wl_heap_overflow_enable(tag) == WL_OK, "wl_heap_overflow_enable");
    const void *base = NULL;
    size_t size = 0;
    wlt_check("heap_overflow_range", wl_mem_tag_range(tag, &base, &size) && in_mem2(base) && size >= OVERFLOW_CHUNK * 4u,
              "the overflow tag's heap is in MEM2 and holds a few chunks");

    // a small block from before the heap is full: the realloc test's primary block
    uint8_t *small = (uint8_t *)malloc(1000u);
    wlt_check("heap_overflow_small_primary", small != NULL && !wl_heap_overflow_owns(small), "malloc is newlib's while it has room");
    overflow_stamp(small, 1000u, 0x11u);

    unsigned count = 0, spilled = 0, bad_address = 0, bad_alignment = 0;
    unsigned first_spill = 0;
    while (count < OVERFLOW_MAX_BLOCKS && spilled < 4u) {
        void *p = malloc(OVERFLOW_CHUNK);
        if (p == NULL) {
            break;
        }
        g_overflow_blocks[count] = p;
        overflow_stamp(p, OVERFLOW_CHUNK, (uint8_t)count);
        if (wl_heap_overflow_owns(p)) {
            if (spilled == 0u) {
                first_spill = count;
            }
            spilled++;
            if (!in_mem2(p) || (uintptr_t)p < (uintptr_t)base || (uintptr_t)p + OVERFLOW_CHUNK > (uintptr_t)base + size) {
                bad_address++;
            }
            if (((uintptr_t)p & 31u) != 0u) {
                bad_alignment++;
            }
        }
        count++;
    }
    snprintf(detail, sizeof detail, "%u blocks of 256 KiB, %u in the overflow heap (the first was number %u)", count, spilled, first_spill);
    wlt_report("INFO heap_overflow %s", detail);
    wlt_check("heap_overflow_reached", spilled == 4u, detail);
    wlt_check("heap_overflow_addresses", bad_address == 0u, "every overflow block lies inside the tag's heap, in MEM2");
    wlt_check("heap_overflow_aligned", bad_alignment == 0u, "every overflow block is 32-byte aligned");
    wlt_check("heap_overflow_order", count == 0u || !wl_heap_overflow_owns(g_overflow_blocks[0]),
              "newlib's heap is used first");
    bool intact = true;
    for (unsigned i = 0; i < count; i++) {
        intact = intact && overflow_stamp_ok(g_overflow_blocks[i], OVERFLOW_CHUNK, (uint8_t)i);
    }
    wlt_check("heap_overflow_contents", intact, "every block keeps what was written to it");

    // calloc and memalign while newlib's heap is full
    uint8_t *zeroed = (uint8_t *)calloc(1u, OVERFLOW_CHUNK);
    bool zero_ok = zeroed != NULL;
    for (size_t i = 0; zero_ok && i < OVERFLOW_CHUNK; i += 509u) {
        zero_ok = zeroed[i] == 0u;
    }
    wlt_check("heap_overflow_calloc", zero_ok && wl_heap_overflow_owns(zeroed), "calloc comes cleared from the overflow heap");
    void *aligned = memalign(256u, OVERFLOW_CHUNK);
    wlt_check("heap_overflow_memalign", aligned != NULL && wl_heap_overflow_owns(aligned) && ((uintptr_t)aligned & 255u) == 0u,
              "memalign(256) from the overflow heap");
    wlt_check("heap_overflow_usable_size", malloc_usable_size(g_overflow_blocks[count - 1u]) >= OVERFLOW_CHUNK,
              "malloc_usable_size on an overflow block");

    // realloc of a primary block that cannot grow: it moves to the overflow heap with its contents
    uint8_t *moved = (uint8_t *)realloc(small, 2u * OVERFLOW_CHUNK);
    wlt_check("heap_overflow_realloc_up", moved != NULL && wl_heap_overflow_owns(moved) && overflow_stamp_ok(moved, 1000u, 0x11u),
              "a newlib block moves to the overflow heap, contents kept");

    // newlib's blocks freed: room again, so an overflow block that has to grow moves back
    for (unsigned i = 0; i < first_spill; i++) {
        free(g_overflow_blocks[i]);
        g_overflow_blocks[i] = NULL;
    }
    void *down = g_overflow_blocks[first_spill];
    uint8_t seed = (uint8_t)first_spill;
    uint8_t *back = (uint8_t *)realloc(down, OVERFLOW_CHUNK + 64u * 1024u);
    wlt_check("heap_overflow_realloc_back", back != NULL && !wl_heap_overflow_owns(back) && overflow_stamp_ok(back, OVERFLOW_CHUNK, seed),
              "an overflow block that grows moves to newlib's heap when it has room again");
    g_overflow_blocks[first_spill] = back;

    WlOverflowStats stats;
    wl_heap_overflow_stats(&stats);
    snprintf(detail, sizeof detail, "allocs %u moved_in %u moved_out %u fails %u bad_frees %u",
             (unsigned)stats.allocs, (unsigned)stats.moved_in, (unsigned)stats.moved_out, (unsigned)stats.fails,
             (unsigned)stats.bad_frees);
    wlt_report("INFO heap_overflow %s", detail);
    wl_heap_overflow_log_usage();

    // everything back
    free(moved);
    free(zeroed);
    free(aligned);
    for (unsigned i = 0; i < count; i++) {
        free(g_overflow_blocks[i]);
        g_overflow_blocks[i] = NULL;
    }
    wl_heap_overflow_stats(&stats);
    wlt_check("heap_overflow_freed", stats.live == 0u && wl_mem_live_blocks(tag) == 0u && stats.bad_frees == 0u,
              "no overflow block is left, none was freed twice");
    wlt_check("heap_overflow_heap_sound", wl_mem_check_all(), "wl_mem_check_all");
    void *again = malloc(1000u);
    wlt_check("heap_overflow_primary_again", again != NULL && !wl_heap_overflow_owns(again), "malloc is newlib's again");
    free(again);
}

int main(void) {
    wl_platform_system_init();
    wl_platform_log_init(NULL);
    wlt_report("WLT START WiiLibs console tests");
    // a fixed MEM2 claim, as an image holding absolute pointers needs: the region starts above it
    {
        uint8_t *fixed = (uint8_t *)(uintptr_t)0x90100000u;
        const size_t fixed_size = 4u * 1024u * 1024u;
        bool ok = wl_platform_mem_claim_fixed(WL_MEM2, fixed, fixed_size) == WL_OK;
        wlt_check("mem_claim_fixed", ok, "4 MiB at 0x90100000");
        if (ok) {
            memset(fixed, 0x5A, fixed_size);
            wlt_check("mem_claim_fixed_usable", fixed[0] == 0x5A && fixed[fixed_size - 1u] == 0x5A,
                      "the claimed range reads back");
        }
        wlt_check("mem_claim_fixed_order", wl_platform_mem_claim_fixed(WL_MEM2, fixed, 32) == WL_ERR_RANGE,
                  "a claim below the last one fails");
    }
    // and one at the top of MEM1: the region ends below it
    {
        uint8_t *fixed = (uint8_t *)(uintptr_t)0x81700000u;
        const size_t fixed_size = 1024u * 1024u;
        bool ok = wl_platform_mem_claim_fixed(WL_MEM1, fixed, fixed_size) == WL_OK;
        wlt_check("mem1_claim_fixed", ok, "1 MiB at 0x81700000");
        if (ok) {
            memset(fixed, 0xC3, fixed_size);
            wlt_check("mem1_claim_fixed_usable", fixed[0] == 0xC3 && fixed[fixed_size - 1u] == 0xC3,
                      "the claimed range reads back");
        }
    }
    // MEM1: only the budgets' need, from the top, so malloc keeps the rest; MEM2: 40 MiB of its top
    {
        // the built-in tags (the asset heap smaller) and one for the overflow heap
        static WlMemTagDef tags[WL_TAG_USER + 1u];
        memcpy(tags, wl_mem_builtin_tags(), WL_TAG_BUILTIN_COUNT * sizeof tags[0]);
        tags[WL_TAG_ASSET].budget = 20u * 1024u * 1024u;
        tags[WL_TAG_USER] = (WlMemTagDef){"overflow", WL_MEM2, 6u * 1024u * 1024u};
        WlMemConfig cfg;
        wl_mem_config_default(&cfg);
        cfg.tags = tags;
        cfg.tag_count = WL_TAG_USER + 1u;
        cfg.claim[WL_MEM1] = WL_MEM_CLAIM_NEEDED;
        cfg.claim[WL_MEM2] = 40u * 1024u * 1024u;
        wlt_check("mem_init", wl_mem_init(&cfg) == WL_OK, "wl_mem_init, limited claims");
        void *base = NULL;
        size_t size = 0;
        wlt_check("mem1_claim_needed",
                  wl_platform_mem_region(WL_MEM1, &base, &size) == WL_OK && size == 9u * 1024u * 1024u,
                  "MEM1's claim is the 9 MiB of budgets and arenas");
        void *big = malloc(8u * 1024u * 1024u);
        wlt_check("mem1_malloc_room", big != NULL && (uintptr_t)big < (uintptr_t)base,
                  "malloc gets 8 MiB of MEM1 below the claim");
        free(big);
        wlt_check("mem2_claim_top",
                  wl_platform_mem_region(WL_MEM2, &base, &size) == WL_OK && size >= 40u * 1024u * 1024u &&
                      size < 40u * 1024u * 1024u + 32u && (uintptr_t)base >= 0x90500000u,
                  "MEM2's claim is the top 40 MiB of its arena");
    }
    {
        void *base = NULL;
        size_t size = 0;
        wlt_check("mem1_region_below_claim",
                  wl_platform_mem_region(WL_MEM1, &base, &size) == WL_OK &&
                      (uintptr_t)base + size <= 0x81700000u,
                  "MEM1's heaps end below the fixed claim");
    }
    {
        void *base = NULL;
        size_t size = 0;
        wlt_check("mem_region_above_claim",
                  wl_platform_mem_region(WL_MEM2, &base, &size) == WL_OK && (uintptr_t)base >= 0x90500000u,
                  "MEM2's heaps start above the fixed claim");
        wlt_check("mem_claim_after_init", wl_platform_mem_claim_fixed(WL_MEM2, (void *)(uintptr_t)0x93000000u, 32) ==
                  WL_ERR_STATE, "no claims once MEM2 is claimed");
    }
    for (int region = WL_MEM1; region <= WL_MEM2; region++) {
        void *base = NULL;
        size_t size = 0;
        if (wl_platform_mem_region((WlMemRegion)region, &base, &size) == WL_OK) {
            wlt_report("INFO mem%d %p..%p (%u KiB)", region + 1, base, (void *)((char *)base + size),
                       (unsigned)(size / 1024u));
        }
    }
    wlt_report("WLT SUITE heap_overflow");
    check_heap_overflow(WL_TAG_USER);
    wlt_check("input_init", wl_platform_input_init() == WL_OK, "wl_platform_input_init");

    check_video_gx();
    check_render_target();
    check_tev_program();
    // the NV2A backend on GX: synthetic push buffers, the EFB read back (tests/xbox/suite_nv2a.c)
    wlt_suite_nv2a();
    check_backtrace();
    check_profile();
    check_atomics64();
    check_tls();
    wlt_report("WLT SUITE threads");
    wlt_suite_threads();
    wlt_report("WLT SUITE fs");
    wlt_suite_fs();
    // (after the fs suite: its IOS reload would drop the network's handles)
    wlt_report("WLT SUITE net");
    check_net();
    wlt_report("WLT SUITE posix");
    wlt_check("posix_mkdir", wl_platform_fs_mkdir("sd:/wltest") == WL_OK, "sd:/wltest");
    wlt_suite_posix("sd:/wltest/posix.tmp");
    wlt_report("WLT SUITE sseq");
    wlt_suite_sseq();
    wlt_report("WLT SUITE sqrt");
    wlt_suite_sqrt();

    wlt_report("WLT RESULT pass=%d fail=%d", g_pass, g_fail);
    wlt_report("WLT DONE");
    wl_platform_log_flush();

    const uint32_t colour = g_fail == 0 ? 0x109030ffu : 0xb02020ffu;
    for (;;) {
        WlRawInput input;
        wl_platform_input_poll(&input);
        bool home = false;
        for (uint32_t i = 0; i < WL_INPUT_MAX_REMOTES; i++) {
            home |= input.remotes[i].connected && (input.remotes[i].buttons & WL_WM_HOME) != 0u;
        }
        for (uint32_t i = 0; i < WL_INPUT_MAX_PADS; i++) {
            home |= input.pads[i].connected && (input.pads[i].buttons & WL_PAD_START) != 0u;
        }
        if (home || wl_platform_system_events() != 0u) {
            wl_platform_exit(WL_EXIT_LOADER);
        }
        wl_gx_begin_frame(colour);
        wl_gx_end_frame();
    }
}
