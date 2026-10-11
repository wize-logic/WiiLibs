// SPDX-License-Identifier: MIT
// The display queue (wl/platform/present.c) against a simulated video interface: the back buffer
// is never one being scanned, a frame is shown only once its GPU work is done, frames reach the
// screen in order with none skipped, each stays at least the interval, and the CPU is told to
// wait only when every buffer is on the screen or waiting to be. A randomized run checks the same
// over many frames with every buffer count and interval.
#include "wl/platform/present.h"

#include <string.h>

#include "greatest.h"

// The video interface as libogc drives it: a buffer committed in the pre-retrace callback is
// written to the registers in the same retrace (unless `defer`), and scanned from then on.
typedef struct {
    WlPresentQueue q;
    int current;
    bool defer;
    uint32_t content[WL_PRESENT_MAX_BUFFERS]; // the frame each buffer holds (written at acquire)
    bool gpu_done[WL_PRESENT_MAX_BUFFERS];    // its copy finished
    uint32_t shown_frame;                     // the frame on the screen
    uint32_t shown_for;                       // retraces it has been there
    uint32_t violations;
} Sim;

static Sim *g_sim;

static bool done_test(uint32_t buffer) {
    return g_sim->gpu_done[buffer];
}

static void sim_init(Sim *v, uint32_t count, uint32_t interval) {
    memset(v, 0, sizeof *v);
    g_sim = v;
    v->current = 0;
    (void)wl_present_init(&v->q, count, interval, 0);
}

// Returns the buffer acquired (its content becomes `frame`), or -1.
static int sim_draw(Sim *v, uint32_t frame) {
    int i = wl_present_acquire(&v->q);
    if (i >= 0) {
        if (i == v->current || i == v->q.committed) {
            v->violations++; // drawing into what the display scans
        }
        v->content[i] = frame;
        v->gpu_done[i] = false;
    }
    return i;
}

static void sim_present(Sim *v) {
    (void)wl_present_submit(&v->q, done_test, (uint32_t)v->q.back);
}

static void sim_retrace(Sim *v) {
    int c = wl_present_retrace_begin(&v->q);
    if (c >= 0) {
        if (!v->gpu_done[c]) {
            v->violations++; // shown before the GPU finished it
        }
        if (!v->defer) {
            v->current = c;
        }
    }
    wl_present_retrace_end(&v->q, v->current);
    uint32_t f = v->content[v->current];
    if (f != v->shown_frame) {
        if (f < v->shown_frame) {
            v->violations++; // out of order
        }
        v->shown_frame = f;
        v->shown_for = 0;
    }
    v->shown_for++;
}

TEST init_validates_arguments(void) {
    WlPresentQueue q;
    ASSERT_EQ(WL_ERR_ARG, wl_present_init(NULL, 2, 1, 0));
    ASSERT_EQ(WL_ERR_ARG, wl_present_init(&q, 1, 1, 0));
    ASSERT_EQ(WL_ERR_ARG, wl_present_init(&q, 4, 1, 0));
    ASSERT_EQ(WL_ERR_ARG, wl_present_init(&q, 2, 1, 2));
    ASSERT_EQ(WL_OK, wl_present_init(&q, 3, 0, -1));
    ASSERT_EQ(1u, q.interval); // (clamped)
    wl_present_set_interval(&q, 99);
    ASSERT_EQ(15u, q.interval);
    ASSERT(wl_present_idle(&q));
    ASSERT(!wl_present_submit(&q, NULL, 0)); // nothing acquired
    PASS();
}

TEST double_buffering_waits_only_for_the_flip(void) {
    Sim v;
    sim_init(&v, 2, 1);
    ASSERT_EQ(1, sim_draw(&v, 1));
    ASSERT_EQ(1, wl_present_acquire(&v.q)); // (the same until presented)
    v.gpu_done[1] = true;
    sim_present(&v);
    ASSERT(!wl_present_idle(&v.q));
    ASSERT_EQ(-1, wl_present_acquire(&v.q)); // 0 on the screen, 1 waiting for its retrace
    sim_retrace(&v);
    ASSERT_EQ(1, v.current);
    ASSERT(wl_present_idle(&v.q));
    ASSERT_EQ(1u, v.q.latched);
    ASSERT_EQ(0, sim_draw(&v, 2)); // 0 left the screen at that retrace
    ASSERT_EQ(0u, v.violations);
    PASS();
}

TEST triple_buffering_never_waits_for_the_display(void) {
    Sim v;
    sim_init(&v, 3, 1);
    ASSERT_EQ(1, sim_draw(&v, 1));
    v.gpu_done[1] = true;
    sim_present(&v);
    ASSERT_EQ(2, sim_draw(&v, 2)); // no retrace yet: the third buffer
    v.gpu_done[2] = true;
    sim_present(&v);
    ASSERT_EQ(-1, wl_present_acquire(&v.q)); // two frames ahead: now it waits
    sim_retrace(&v);
    ASSERT_EQ(1u, v.content[v.current]);
    ASSERT_EQ(0, sim_draw(&v, 3));
    sim_retrace(&v);
    ASSERT_EQ(2u, v.content[v.current]); // in order, none skipped
    ASSERT_EQ(2u, v.q.latched);
    ASSERT_EQ(0u, v.q.late);
    ASSERT_EQ(0u, v.violations);
    PASS();
}

TEST the_gpu_gates_the_flip(void) {
    Sim v;
    sim_init(&v, 2, 1);
    ASSERT_EQ(1, sim_draw(&v, 1));
    sim_present(&v); // the copy still running
    for (int r = 0; r < 5; r++) {
        sim_retrace(&v);
        ASSERT_EQ(0, v.current);
    }
    v.gpu_done[1] = true;
    sim_retrace(&v);
    ASSERT_EQ(1, v.current);
    ASSERT_EQ(0u, v.violations);
    PASS();
}

TEST an_interval_of_two_holds_early_frames(void) {
    Sim v;
    sim_init(&v, 2, 2);
    // the first frame goes at the first retrace (nothing latched yet to count from)
    ASSERT_EQ(1, sim_draw(&v, 1));
    v.gpu_done[1] = true;
    sim_present(&v);
    sim_retrace(&v);
    ASSERT_EQ(1, v.current);
    // a frame ready at once is held one retrace, then shown: two retraces each
    ASSERT_EQ(0, sim_draw(&v, 2));
    v.gpu_done[0] = true;
    sim_present(&v);
    sim_retrace(&v);
    ASSERT_EQ(1, v.current);
    sim_retrace(&v);
    ASSERT_EQ(0, v.current);
    ASSERT_EQ(2u, v.q.last_gap);
    // a frame three retraces after that one is late: shown at once, counted
    ASSERT_EQ(1, sim_draw(&v, 3));
    v.gpu_done[1] = true;
    sim_retrace(&v);
    sim_retrace(&v);
    sim_present(&v);
    sim_retrace(&v);
    ASSERT_EQ(1, v.current);
    ASSERT_EQ(3u, v.q.last_gap);
    ASSERT_EQ(1u, v.q.late);
    ASSERT_EQ(0u, v.violations);
    PASS();
}

TEST registers_deferred_by_libogc_commit_once(void) {
    Sim v;
    sim_init(&v, 3, 1);
    ASSERT_EQ(1, sim_draw(&v, 1));
    v.gpu_done[1] = true;
    sim_present(&v);
    ASSERT_EQ(2, sim_draw(&v, 2));
    v.gpu_done[2] = true;
    sim_present(&v);
    v.defer = true; // (a mode change pending: the registers wait a retrace)
    sim_retrace(&v);
    ASSERT_EQ(1, v.q.committed);
    ASSERT_EQ(0, v.current);
    ASSERT_EQ(-1, wl_present_retrace_begin(&v.q)); // the next frame does not overtake it
    wl_present_retrace_end(&v.q, 1);               // the interface took 1 at last
    v.current = 1;
    ASSERT_EQ(1, v.q.shown);
    ASSERT_EQ(-1, v.q.committed);
    ASSERT_EQ(0u, v.violations);
    PASS();
}

TEST reclaim_drops_a_frame_the_gpu_never_finished(void) {
    Sim v;
    sim_init(&v, 2, 1);
    ASSERT_EQ(1, sim_draw(&v, 1));
    sim_present(&v); // never done
    sim_retrace(&v);
    ASSERT_EQ(-1, wl_present_acquire(&v.q));
    ASSERT_EQ(1, wl_present_reclaim(&v.q));
    ASSERT_EQ(1u, v.q.reclaimed);
    ASSERT(wl_present_idle(&v.q));
    ASSERT_EQ(1, sim_draw(&v, 2));
    ASSERT_EQ(-1, wl_present_reclaim(&v.q)); // (nothing pending, nothing free)
    PASS();
}

TEST a_buffer_scanned_behind_the_queues_back_is_avoided(void) {
    Sim v;
    sim_init(&v, 2, 1);
    v.current = 1; // a text console put XFB 1 on the screen itself
    sim_retrace(&v);
    ASSERT_EQ(-1, wl_present_acquire(&v.q));
    ASSERT_EQ(1, wl_present_reclaim(&v.q)); // after a wait: drawn over after all
    ASSERT_EQ(1, wl_present_acquire(&v.q));
    PASS();
}

static uint32_t g_rng = 12345u;

static uint32_t rnd(uint32_t n) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (g_rng >> 8) % n;
}

// Retraces simulated in steps of a quarter retrace: the CPU takes a random time per frame, the GPU
// a random time to finish each copy.
TEST randomized_frames_keep_every_rule(void) {
    for (uint32_t count = 2; count <= 3; count++) {
        for (uint32_t interval = 1; interval <= 3; interval++) {
            Sim v;
            sim_init(&v, count, interval);
            uint32_t frame = 0, cpu_left = 0, waits = 0;
            uint32_t gpu_left[WL_PRESENT_MAX_BUFFERS] = {0};
            uint32_t presented_frames = 0, last_shown = 0, gaps_short = 0;
            for (uint32_t step = 0; step < 40000u; step++) {
                // the GPU
                for (uint32_t b = 0; b < count; b++) {
                    if (gpu_left[b] != 0 && --gpu_left[b] == 0) {
                        v.gpu_done[b] = true;
                    }
                }
                // the CPU: a frame drawn, then presented with its copy queued
                if (cpu_left != 0) {
                    cpu_left--;
                } else {
                    int b = sim_draw(&v, frame + 1);
                    if (b < 0) {
                        waits++;
                        // (told to wait: every buffer must be on the screen or waiting to be)
                        for (uint32_t i = 0; i < count; i++) {
                            if (v.q.state[i] == WL_XFB_FREE && (int)i != v.q.scanned) {
                                v.violations++;
                            }
                        }
                    } else {
                        frame++;
                        gpu_left[b] = 1u + rnd(6u);
                        sim_present(&v);
                        presented_frames++;
                        cpu_left = rnd(14u);
                    }
                }
                if (step % 4u == 3u) {
                    uint32_t before = v.shown_frame, held = v.shown_for;
                    sim_retrace(&v);
                    if (v.shown_frame != before) {
                        if (v.shown_frame != before + 1u) {
                            v.violations++; // a frame skipped
                        }
                        if (before != 0u && held < interval) {
                            gaps_short++;
                        }
                        last_shown = v.shown_frame;
                    }
                }
            }
            ASSERT_EQ(0u, v.violations);
            ASSERT_EQ(0u, gaps_short);
            ASSERT(presented_frames > 1000u);
            ASSERT(last_shown + count >= presented_frames); // (at most the queue behind)
            ASSERT_EQ(v.q.latched, last_shown);
            ASSERT(waits > 0u);
        }
    }
    PASS();
}

SUITE(present_suite) {
    RUN_TEST(init_validates_arguments);
    RUN_TEST(double_buffering_waits_only_for_the_flip);
    RUN_TEST(triple_buffering_never_waits_for_the_display);
    RUN_TEST(the_gpu_gates_the_flip);
    RUN_TEST(an_interval_of_two_holds_early_frames);
    RUN_TEST(registers_deferred_by_libogc_commit_once);
    RUN_TEST(reclaim_drops_a_frame_the_gpu_never_finished);
    RUN_TEST(a_buffer_scanned_behind_the_queues_back_is_avoided);
    RUN_TEST(randomized_frames_keep_every_rule);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(present_suite);
    GREATEST_MAIN_END();
}
