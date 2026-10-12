// SPDX-License-Identifier: MIT
// The hardware counters' engine (wl/platform/hwcount.h) on every host, against a fake machine whose
// clock, performance monitor and GPU counters the test sets: the register encodings, the modulo
// arithmetic, medians, the control file, the verdict on each kind of frame, the rotation of
// groups through the frames, waits left out of the processor's counts, the marks' timeline, the
// self-test that finds counters that do not count (Dolphin's) and the log block.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "greatest.h"
#include "wl/platform/hwcount.h"

#define TB 60750000ull
#define FRAME_TICKS (TB / 30u) // 33.3 ms

// --- the fake machine ---------------------------------------------------------------------------
typedef struct Scenario {
    uint32_t ipc_m;          // instructions completed per 1000 cycles
    uint32_t l1d_miss_pki;
    uint32_t mispredict_pki;
    uint32_t dc_stall_pm;    // per mille of cycles
    uint32_t dc_load_pm;
    uint32_t br_stall_pm;
    uint32_t xf_in_pm, xf_out_pm, ras_pm, xfrm_pm, lit_pm, bot_pm, txstall_pm, txidle_pm, vcstall_pm;
    uint32_t verts, tris, quads, texels, tc_miss;
    uint32_t vc_check, vc_miss, vc_stall_pm;
    uint32_t fifo;           // bytes
} Scenario;

typedef struct Fake {
    uint64_t now;
    Scenario sc;
    bool cpu_counts;         // false: the counters read zero (Dolphin)
    bool gp_counts;
    bool have_cpu, have_gp;
    // the performance monitor
    uint8_t sel[4];
    uint64_t cpu_start_at;
    // the GPU
    WlHwgpGroup group;
    uint64_t gp_start_at;
    bool gp_started;
    uint32_t vertex_slot0, vertex_slot1; // where a vertex count shows up in the self-test
    uint32_t tokens;
    WlHwcountTokenFn hook;
    void *hook_arg;
    uint32_t fifo_hz;
    char lines[64][256];
    uint32_t line_count;
} Fake;

static uint64_t f_ticks(void *u) {
    return ((Fake *)u)->now;
}

static void f_cpu_start(void *u, const uint8_t sel[4], uint32_t threshold) {
    Fake *f = u;

    (void)threshold;
    memcpy(f->sel, sel, 4);
    f->cpu_start_at = f->now;
}

static uint32_t event_count(const Fake *f, uint32_t pmc, uint8_t event, uint64_t cycles) {
    const Scenario *s = &f->sc;
    uint64_t instr = cycles * s->ipc_m / 1000u;

    switch (event) {
    case 1:
        return (uint32_t)cycles;
    case 2:
        return (uint32_t)instr;
    case 4:
        return (uint32_t)(instr * 11u / 10u);
    case 5:
        return pmc == 3 ? (uint32_t)(instr * s->l1d_miss_pki / 1000u) : (uint32_t)(cycles / 50u); // PMC4: L2 castouts
    case 6:
        return (uint32_t)(cycles / 100u);
    case 7:
        return (uint32_t)(cycles / 200u);
    case 8:
        return (uint32_t)(instr * s->mispredict_pki / 1000u);
    case 10:
        return (uint32_t)(cycles * s->dc_stall_pm / 1000u);
    case 11:
        return (uint32_t)(instr / 10u);
    case 12:
        return pmc == 1 ? (uint32_t)(cycles * s->br_stall_pm / 1000u) : 7u;
    case 13:
        return (uint32_t)(instr / 2u);
    case 15:
        return (uint32_t)(cycles * s->dc_load_pm / 1000u);
    case 17:
        return (uint32_t)(cycles / 100u);
    default:
        return 0;
    }
}

static void f_cpu_read(void *u, uint32_t pmc[4]) {
    Fake *f = u;
    uint64_t cycles = (f->now - f->cpu_start_at) * 12u;
    uint32_t i;

    for (i = 0; i < 4; i++) {
        pmc[i] = f->cpu_counts ? event_count(f, i + 1u, f->sel[i], cycles) : 0u;
    }
}

static void f_cpu_stop(void *u) {
    (void)u;
}

static void f_gp_start(void *u, const WlHwgpGroup *g, bool vcache) {
    Fake *f = u;

    (void)vcache;
    f->group = *g;
    f->gp_start_at = f->now;
    f->gp_started = true;
}

static uint32_t gp_value(const Fake *f, uint8_t perf, bool second, uint64_t clocks) {
    const Scenario *s = &f->sc;

    if (!second) {
        switch (perf) {
        case 3:
            return (uint32_t)(clocks * s->xf_in_pm / 1000u);
        case 5:
            return (uint32_t)(clocks * s->xfrm_pm / 1000u);
        case 6:
            return (uint32_t)(clocks * s->lit_pm / 1000u);
        case 7:
            return (uint32_t)(clocks * s->bot_pm / 1000u);
        case 13:
            return s->tris;
        case 28:
            return s->quads;
        case 0:
            return s->verts; // GX_PERF0_VERTICES (the self-test's)
        default:
            return 0;
        }
    }
    switch (perf) {
    case 16:
        return s->verts;
    case 0:
        return s->texels;
    case 3:
        return (uint32_t)(clocks * s->txstall_pm / 1000u);
    case 1:
        return (uint32_t)(clocks * s->txidle_pm / 1000u);
    case 8:
        return s->tc_miss;
    case 15:
        return (uint32_t)(clocks * s->vcstall_pm / 1000u);
    default:
        return 0;
    }
}

static void f_gp_read(void *u, uint32_t slot[4], uint32_t vc[3]) {
    Fake *f = u;
    uint64_t clocks = (f->now - f->gp_start_at) * 4u;

    memset(slot, 0, 4 * sizeof slot[0]);
    memset(vc, 0, 3 * sizeof vc[0]);
    if (!f->gp_counts || !f->gp_started) {
        return;
    }
    if (f->group.kind == WL_HWGP_XFRAS) {
        slot[0] = (uint32_t)(clocks * f->sc.ras_pm / 1000u);
        slot[1] = (uint32_t)clocks;
        slot[2] = (uint32_t)(clocks * f->sc.xf_in_pm / 1000u);
        slot[3] = (uint32_t)(clocks * f->sc.xf_out_pm / 1000u);
    } else if (strcmp(f->group.name, "selftest") == 0) {
        slot[f->vertex_slot0] = f->sc.verts;
        slot[f->vertex_slot1] = f->sc.verts;
    } else {
        slot[0] = gp_value(f, f->group.perf0, false, clocks);
        slot[2] = gp_value(f, f->group.perf1, true, clocks);
    }
    vc[0] = f->sc.vc_check;
    vc[1] = f->sc.vc_miss;
    vc[2] = (uint32_t)(clocks * f->sc.vc_stall_pm / 1000u);
}

static void f_gp_stop(void *u) {
    ((Fake *)u)->gp_started = false;
}

static uint32_t f_fifo_fill(void *u) {
    return ((Fake *)u)->sc.fifo;
}

static uint32_t f_mark_token(void *u) {
    Fake *f = u;

    f->tokens++;
    return f->tokens;
}

static void f_set_token_hook(void *u, WlHwcountTokenFn fn, void *arg) {
    Fake *f = u;

    f->hook = fn;
    f->hook_arg = arg;
}

static void f_fifo_sampler(void *u, uint32_t hz, WlHwcountTickFn fn, void *arg) {
    (void)fn;
    (void)arg;
    ((Fake *)u)->fifo_hz = hz;
}

static void f_log(void *u, const char *line) {
    Fake *f = u;

    if (getenv("WL_HWCOUNT_TEST_LOG") != NULL) {
        printf("[hw] %s\n", line); // (to see the blocks: WL_HWCOUNT_TEST_LOG=1 test_platform_hwcount)
    }
    if (f->line_count < 64) {
        snprintf(f->lines[f->line_count], sizeof f->lines[0], "%s", line);
    }
    f->line_count++;
}

static WlHwcountHw hw_of(Fake *f) {
    WlHwcountHw hw;

    memset(&hw, 0, sizeof hw);
    hw.tb_hz = TB;
    hw.cpu_clock_per_tick = 12;
    hw.gp_clock_per_tick = 4;
    hw.ticks = f_ticks;
    if (f->have_cpu) {
        hw.cpu_start = f_cpu_start;
        hw.cpu_read = f_cpu_read;
        hw.cpu_stop = f_cpu_stop;
    }
    if (f->have_gp) {
        hw.gp_start = f_gp_start;
        hw.gp_read = f_gp_read;
        hw.gp_stop = f_gp_stop;
    }
    hw.fifo_fill = f_fifo_fill;
    hw.mark_token = f_mark_token;
    hw.set_token_hook = f_set_token_hook;
    hw.fifo_sampler = f_fifo_sampler;
    hw.log = f_log;
    hw.user = f;
    return hw;
}

static Scenario cpu_bound_scenario(void) {
    Scenario s;

    memset(&s, 0, sizeof s);
    s.ipc_m = 520;
    s.l1d_miss_pki = 18;
    s.mispredict_pki = 6;
    s.dc_stall_pm = 310;
    s.dc_load_pm = 180;
    s.br_stall_pm = 40;
    s.xf_in_pm = 720;
    s.xf_out_pm = 10;
    s.ras_pm = 150;
    s.xfrm_pm = 80;
    s.lit_pm = 20;
    s.bot_pm = 30;
    s.txstall_pm = 20;
    s.txidle_pm = 400;
    s.vcstall_pm = 10;
    s.verts = 30000;
    s.tris = 10000;
    s.quads = 50000;
    s.texels = 400000;
    s.tc_miss = 8000;
    s.vc_check = 30000;
    s.vc_miss = 3000;
    s.vc_stall_pm = 10;
    s.fifo = 4096;
    return s;
}

static void fake_init(Fake *f) {
    memset(f, 0, sizeof *f);
    f->now = 1000;
    f->cpu_counts = true;
    f->gp_counts = true;
    f->have_cpu = true;
    f->have_gp = true;
    f->vertex_slot0 = 0;
    f->vertex_slot1 = 2;
    f->sc = cpu_bound_scenario();
}

// Runs `frames` frames of `frame_ticks` each, with `gpu_wait` ticks of them spent waiting for the
// GPU and `display_wait` for the display, the GPU finishing `gpu_done` ticks in.
static void run_frames(WlHwcount *hc, Fake *f, uint32_t frames, uint64_t frame_ticks, uint64_t gpu_wait,
                       uint64_t display_wait, uint64_t gpu_done) {
    uint32_t i;

    for (i = 0; i < frames; i++) {
        uint64_t t0;

        wl_hwcount_begin_frame(hc);
        t0 = f->now;
        f->now += frame_ticks / 2u;
        wl_hwcount_wait_start(hc, WL_HWWAIT_GPU);
        f->now += gpu_wait;
        wl_hwcount_wait_stop(hc, WL_HWWAIT_GPU);
        f->now = t0 + gpu_done;
        wl_hwcount_end_frame(hc);
        f->now = t0 + frame_ticks - display_wait;
        wl_hwcount_wait_start(hc, WL_HWWAIT_DISPLAY);
        f->now = t0 + frame_ticks;
        wl_hwcount_wait_stop(hc, WL_HWWAIT_DISPLAY);
    }
}

static WlHwcountSettings quick_settings(void) {
    WlHwcountSettings s;

    wl_hwcount_settings_default(&s);
    s.first = 0;
    s.seconds = 1;
    return s;
}

static bool has_line(const Fake *f, const char *needle) {
    uint32_t i;

    for (i = 0; i < f->line_count && i < 64; i++) {
        if (strstr(f->lines[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

// --- registers and arithmetic -------------------------------------------------------------------
TEST registers_hold_the_events_at_the_manuals_bit_positions(void) {
    const uint8_t sel[4] = {1, 2, 5, 8};

    // MMCR0: PMC1SELECT bits 19-25 (value shift 6), PMC2SELECT 26-31 (shift 0), THRESHOLD 10-15 (shift 16)
    ASSERT_EQ_FMT(0x00000042u, wl_hwcount_mmcr0(sel, 0), "%08x");
    ASSERT_EQ_FMT(0x00030042u, wl_hwcount_mmcr0(sel, 3), "%08x");
    ASSERT_EQ_FMT(0x003F0042u, wl_hwcount_mmcr0(sel, 127), "%08x"); // (six bits)
    // MMCR1: PMC3SELECT bits 0-4 (shift 27), PMC4SELECT 5-9 (shift 22)
    ASSERT_EQ_FMT(0x28000000u | 0x02000000u, wl_hwcount_mmcr1(sel), "%08x");
    ASSERT_EQ_FMT(0x80000000u, WL_HWCOUNT_MMCR0_DIS, "%08x");
    PASS();
}

TEST every_group_selects_cycles_on_pmc1_and_fits_the_fields(void) {
    uint32_t i;

    for (i = 0; i < WL_HWCOUNT_CPU_GROUPS; i++) {
        const WlHwcpuGroup *g = wl_hwcount_cpu_group(i);

        ASSERT(g != NULL);
        ASSERT_EQ(1, g->sel[0]);
        ASSERT(g->sel[1] < 64);
        ASSERT(g->sel[2] < 32 && g->sel[3] < 32);
        ASSERT_EQ(0u, wl_hwcount_mmcr0(g->sel, 0) & WL_HWCOUNT_MMCR0_DIS);
    }
    ASSERT(wl_hwcount_cpu_group(WL_HWCOUNT_CPU_GROUPS) == NULL);
    ASSERT(wl_hwcount_gp_group(WL_HWCOUNT_GP_GROUPS) == NULL);
    ASSERT_EQ(WL_HWGP_XFRAS, wl_hwcount_gp_group(0)->kind);
    PASS();
}

TEST counter_differences_wrap_modulo_2_to_the_32(void) {
    ASSERT_EQ(5u, wl_hwcount_delta(10, 15));
    ASSERT_EQ(5u, wl_hwcount_delta(0xFFFFFFFEu, 3));
    ASSERT_EQ(0u, wl_hwcount_delta(7, 7));
    ASSERT_EQ(0xFFFFFFFFu, wl_hwcount_delta(1, 0));
    PASS();
}

TEST the_median_takes_the_lower_middle_and_leaves_the_input_alone(void) {
    uint32_t odd[5] = {9, 1, 5, 3, 7};
    uint32_t even[4] = {40, 10, 30, 20};

    ASSERT_EQ(5u, wl_hwcount_median(odd, 5));
    ASSERT_EQ(20u, wl_hwcount_median(even, 4));
    ASSERT_EQ(9u, odd[0]);
    ASSERT_EQ(40u, even[0]);
    ASSERT_EQ(7u, wl_hwcount_median(odd + 4, 1));
    ASSERT_EQ(0u, wl_hwcount_median(odd, 0));
    PASS();
}

// --- the control file -----------------------------------------------------------------------------
typedef struct {
    int count;
    int issue[4];
    char key[4][32];
    uint32_t line[4];
} Issues;

static void note_issue(void *user, int issue, const char *key, uint32_t line) {
    Issues *i = user;

    if (i->count < 4) {
        i->issue[i->count] = issue;
        snprintf(i->key[i->count], sizeof i->key[0], "%s", key);
        i->line[i->count] = line;
    }
    i->count++;
}

TEST the_control_file_overrides_the_defaults_and_reports_what_it_cannot_use(void) {
    WlHwcountSettings s;
    Issues issues;
    const char *text = "# a comment\n  seconds 5\ngp=0\nfifo_hz 99999 # too fast\nbogus 3\nraw x\nthreshold\r\nfirst 10\n";

    memset(&issues, 0, sizeof issues);
    wl_hwcount_settings_default(&s);
    ASSERT_EQ(1u, s.seconds);
    ASSERT_EQ(480u, s.fifo_hz);
    ASSERT_EQ(2u, wl_hwcount_settings_parse(&s, text, strlen(text), note_issue, &issues));
    ASSERT_EQ(5u, s.seconds);
    ASSERT_EQ(0u, s.gp);
    ASSERT_EQ(1u, s.cpu);
    ASSERT_EQ(10000u, s.fifo_hz); // (clamped)
    ASSERT_EQ(10u, s.first);
    ASSERT_EQ(0u, s.raw);
    ASSERT_EQ(2, issues.count);
    ASSERT_EQ(1, issues.issue[0]);
    ASSERT_STR_EQ("bogus", issues.key[0]);
    ASSERT_EQ(5u, issues.line[0]);
    ASSERT_EQ(2, issues.issue[1]);
    ASSERT_STR_EQ("raw", issues.key[1]);
    PASS();
}

TEST a_missing_control_file_is_not_found_and_a_present_one_loads(void) {
    WlHwcountSettings s;
    FILE *f;

    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_hwcount_settings_load(&s, "wl_hwcount_no_such_file.txt", NULL, NULL));
    ASSERT_EQ(1u, s.cpu); // (the defaults are there anyway)
    f = fopen("wl_hwcount_test.txt", "wb");
    ASSERT(f != NULL);
    fputs("seconds 3\ntimeline 0\n", f);
    fclose(f);
    ASSERT_EQ(WL_OK, wl_hwcount_settings_load(&s, "wl_hwcount_test.txt", NULL, NULL));
    ASSERT_EQ(3u, s.seconds);
    ASSERT_EQ(0u, s.timeline);
    remove("wl_hwcount_test.txt");
    PASS();
}

// --- the verdict ----------------------------------------------------------------------------------
static WlHwcountView view_of_cpu_bound(void) {
    WlHwcountView v;
    const Scenario s = cpu_bound_scenario();

    memset(&v, 0, sizeof v);
    v.frame_us = 36000;
    v.gpu_wait_us = 500;
    v.display_wait_us = 0;
    v.fifo_mean_pm = 30;
    v.fifo_max_pm = 200;
#define SET(metric, value) (v.have[metric] = true, v.m[metric] = (value))
    SET(WL_HWM_XF_WAIT_IN_PM, s.xf_in_pm);
    SET(WL_HWM_RAS_BUSY_PM, s.ras_pm);
    SET(WL_HWM_XF_WAIT_OUT_PM, 10);
    SET(WL_HWM_IPC_M, s.ipc_m);
    SET(WL_HWM_DC_STALL_PM, s.dc_stall_pm);
    SET(WL_HWM_DC_LOAD_PM, s.dc_load_pm);
    SET(WL_HWM_BR_STALL_PM, 40);
    SET(WL_HWM_BR_MISP_PKI, 6);
    SET(WL_HWM_L1D_MISS_PKI, 18);
    return v;
}

TEST a_starved_gp_is_cpu_bound_and_the_memory_stalls_name_why(void) {
    WlHwcountView v = view_of_cpu_bound();
    WlHwcountVerdict r;

    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_CPU, r.kind);
    ASSERT_EQ(WL_HWC_MEMORY, r.cpu);
    ASSERT(strstr(r.text, "CPU-bound") != NULL);
    ASSERT(strstr(r.text, "xf wait-in 72.0%") != NULL);
    ASSERT(strstr(r.text, "memory stalls") != NULL);
    ASSERT(strstr(r.text, "dcache stall 31.0%") != NULL);
    PASS();
}

TEST the_processors_cause_is_branches_then_pipeline_stalls_then_compute(void) {
    WlHwcountView v = view_of_cpu_bound();
    WlHwcountVerdict r;

    v.m[WL_HWM_DC_STALL_PM] = 50;
    v.m[WL_HWM_DC_LOAD_PM] = 40;
    v.m[WL_HWM_BR_MISP_PKI] = 25;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWC_BRANCH, r.cpu);
    v.m[WL_HWM_BR_MISP_PKI] = 4;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWC_STALLED, r.cpu); // ipc 0.52
    v.m[WL_HWM_IPC_M] = 900;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWC_COMPUTE, r.cpu);
    ASSERT_EQ(WL_HWV_CPU, r.kind);
    PASS();
}

static WlHwcountView view_of_busy_gpu(void) {
    WlHwcountView v;

    memset(&v, 0, sizeof v);
    v.frame_us = 41000;
    v.gpu_wait_us = 12000; // the processor waits for the GPU a third of the frame
    v.fifo_mean_pm = 600;
    v.fifo_max_pm = 950;
    SET(WL_HWM_XF_WAIT_IN_PM, 80);
    SET(WL_HWM_XF_WAIT_OUT_PM, 40);
    SET(WL_HWM_RAS_BUSY_PM, 200);
    SET(WL_HWM_XF_XFRM_PM, 100);
    SET(WL_HWM_XF_LIT_PM, 50);
    SET(WL_HWM_TX_MEMSTALL_PM, 30);
    SET(WL_HWM_VC_STALL_PM, 20);
    SET(WL_HWM_IPC_M, 800);
    return v;
}

TEST a_busy_gpu_is_told_apart_by_the_part_that_stands_out(void) {
    WlHwcountView v = view_of_busy_gpu();
    WlHwcountVerdict r;

    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_MIXED, r.kind); // nothing over its threshold
    ASSERT(strstr(r.text, "mixed") != NULL);

    v = view_of_busy_gpu();
    v.m[WL_HWM_XF_XFRM_PM] = 380;
    v.m[WL_HWM_XF_LIT_PM] = 150;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_XFORM, r.kind);

    v = view_of_busy_gpu();
    v.m[WL_HWM_RAS_BUSY_PM] = 780;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_FILL, r.kind);
    ASSERT(strstr(r.text, "ras busy 78.0%") != NULL);

    v = view_of_busy_gpu();
    v.m[WL_HWM_XF_WAIT_OUT_PM] = 520; // the XF backed up behind the pixel side
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_FILL, r.kind);

    v = view_of_busy_gpu();
    v.m[WL_HWM_TX_MEMSTALL_PM] = 420;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_TEXTURE, r.kind);

    v = view_of_busy_gpu();
    v.m[WL_HWM_VC_STALL_PM] = 360;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_VERTEX, r.kind);

    // the largest score wins when two are over
    v = view_of_busy_gpu();
    v.m[WL_HWM_TX_MEMSTALL_PM] = 300; // 2.0 times its threshold
    v.m[WL_HWM_VC_STALL_PM] = 600;    // 4.0 times
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_VERTEX, r.kind);
    PASS();
}

TEST a_texture_cache_that_misses_under_a_busy_rasterizer_is_texture_bound(void) {
    WlHwcountView v = view_of_busy_gpu();
    WlHwcountVerdict r;

    v.m[WL_HWM_RAS_BUSY_PM] = 400; // below the fill threshold, over the texture rule's 300
    SET(WL_HWM_TC_MISS_PKT, 180);
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_TEXTURE, r.kind);
    v.m[WL_HWM_RAS_BUSY_PM] = 200; // (an idle rasterizer: the misses do not matter)
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_MIXED, r.kind);
    PASS();
}

TEST a_frame_that_waits_for_the_display_fits_and_none_measured_is_unknown(void) {
    WlHwcountView v = view_of_cpu_bound();
    WlHwcountVerdict r;

    v.frame_us = 33300;
    v.display_wait_us = 9000;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_FITS, r.kind);
    ASSERT(strstr(r.text, "waits 27.0% for the display") != NULL);
    v.frame_us = 45000; // (too long to fit, whatever it waits for)
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_CPU, r.kind);
    memset(&v, 0, sizeof v);
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_UNKNOWN, r.kind);
    PASS();
}

TEST without_gpu_counters_the_fifo_and_the_waits_still_decide(void) {
    WlHwcountView v;
    WlHwcountVerdict r;

    memset(&v, 0, sizeof v);
    v.frame_us = 40000;
    v.gpu_wait_us = 200;
    v.fifo_mean_pm = 20;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_CPU, r.kind);
    v.gpu_wait_us = 15000;
    v.fifo_mean_pm = 700;
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_MIXED, r.kind);
    v.fifo_mean_pm = UINT32_MAX; // not measured
    wl_hwcount_verdict(&v, NULL, &r);
    ASSERT_EQ(WL_HWV_GPU_MIXED, r.kind);
    PASS();
}

TEST thresholds_can_be_replaced(void) {
    WlHwcountView v = view_of_busy_gpu();
    WlHwcountThresholds t;
    WlHwcountVerdict r;

    wl_hwcount_thresholds_default(&t);
    v.m[WL_HWM_RAS_BUSY_PM] = 400;
    wl_hwcount_verdict(&v, &t, &r);
    ASSERT_EQ(WL_HWV_GPU_MIXED, r.kind);
    t.fill_ras_pm = 350;
    wl_hwcount_verdict(&v, &t, &r);
    ASSERT_EQ(WL_HWV_GPU_FILL, r.kind);
    PASS();
}

// --- the engine on the fake machine ------------------------------------------------------------------
TEST a_cpu_bound_run_logs_its_blocks_and_says_cpu_bound(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    hc.fifo_cap = 65536;
    ASSERT_EQ(480u, f.fifo_hz);
    // 4 seconds at 30 fps, the processor waiting 1 ms a frame for the GPU, the GPU done 8 ms in
    run_frames(&hc, &f, 126, FRAME_TICKS, TB / 1000u, 0, TB * 8u / 1000u);
    ASSERT(f.line_count > 0);
    ASSERT(has_line(&f, "block 1:"));
    ASSERT(has_line(&f, "ipc 0.52"));
    ASSERT(has_line(&f, "xf wait-in 72.0%"));
    ASSERT(has_line(&f, "clk 243.0 MHz"));
    ASSERT(has_line(&f, "verdict: CPU-bound"));
    ASSERT(has_line(&f, "memory stalls"));
    ASSERT(has_line(&f, "frame 33.33 ms"));
    ASSERT(has_line(&f, "gpu done at 8.00 ms"));
    ASSERT(hc.cpu_ok && hc.gp_ok);
    ASSERT(hc.log_blocks >= 3);
    PASS();
}

TEST the_groups_rotate_one_a_frame_so_all_are_seen_within_a_rotation(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;
    WlHwcountView v;
    uint32_t i;

    s.seconds = 3600; // (no block: the samples stay for the view)
    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    run_frames(&hc, &f, 43, FRAME_TICKS, 0, 0, TB / 100u); // 42 windows closed: a full rotation of 6 and of 7
    wl_hwcount_view(&hc, &v);
    for (i = 0; i < WL_HWM_COUNT; i++) {
        WlHwMetric m = (WlHwMetric)i;

        // everything but the derived and the vertex-cache-only ones came from some group
        if (m == WL_HWM_TC_MISS_PKT || m == WL_HWM_VCM_MISS_PM || m == WL_HWM_VCM_STALL_PM) {
            continue;
        }
        if (!v.have[i]) { printf("never measured: %s\n", wl_hwcount_metric_name(m)); }
        ASSERTm("a metric was never measured", v.have[i]);
    }
    ASSERT(v.have[WL_HWM_TC_MISS_PKT]);
    ASSERT(v.have[WL_HWM_VCM_MISS_PM]);
    ASSERT_EQ(520u, v.m[WL_HWM_IPC_M]);
    ASSERT_EQ(18u, v.m[WL_HWM_L1D_MISS_PKI]);
    ASSERT_EQ(310u, v.m[WL_HWM_DC_STALL_PM]);
    ASSERT_EQ(720u, v.m[WL_HWM_XF_WAIT_IN_PM]);
    ASSERT_EQ(150u, v.m[WL_HWM_RAS_BUSY_PM]);
    ASSERT_EQ(80u, v.m[WL_HWM_XF_XFRM_PM]);
    ASSERT_EQ(400u, v.m[WL_HWM_TX_IDLE_PM]);
    ASSERT_EQ(30000u, v.m[WL_HWM_VERTS]);
    ASSERT_EQ(20u, v.m[WL_HWM_TC_MISS_PKT]); // 8000 misses over 400000 texels
    ASSERT_EQ(100u, v.m[WL_HWM_VCM_MISS_PM]);
    ASSERT_EQ(2430u, v.m[WL_HWM_GP_MHZ_X10]);
    PASS();
}

TEST waits_are_left_out_of_the_processors_cycles(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;
    WlHwcountView v;

    s.seconds = 3600;
    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    // half of every frame spent in waits: the cycles counted are those of the other half, so the
    // sanity check (cycles near 12 a tick of the time not waited) still passes
    run_frames(&hc, &f, 20, FRAME_TICKS, FRAME_TICKS / 4u, FRAME_TICKS / 4u, FRAME_TICKS / 2u);
    wl_hwcount_view(&hc, &v);
    ASSERT(hc.cpu_ok);
    ASSERT_EQ(0u, hc.cpu_insane_n);
    ASSERT(v.have[WL_HWM_CPU_KCYC]);
    // 16.67 ms of 729 MHz is 12.15 million cycles
    ASSERT(v.m[WL_HWM_CPU_KCYC] > 12000 && v.m[WL_HWM_CPU_KCYC] < 12300);
    ASSERT_EQ(33333u, v.frame_us / 10u * 10u + 3u); // (rounded to the tick)
    ASSERT(v.gpu_wait_us > 8300 && v.gpu_wait_us < 8400);
    ASSERT(v.display_wait_us > 8300 && v.display_wait_us < 8400);
    PASS();
}

TEST a_machine_whose_counters_read_zero_is_marked_invalid_and_logged_once(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;
    WlHwcountView v;

    fake_init(&f);
    f.cpu_counts = false;
    f.gp_counts = false;
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    run_frames(&hc, &f, 60, FRAME_TICKS, 0, 0, FRAME_TICKS / 2u);
    ASSERT(!hc.cpu_ok);
    ASSERT(!hc.gp_ok);
    ASSERT(has_line(&f, "cpu: no counters (cycles read 0"));
    ASSERT(has_line(&f, "gp: no counters (every group read zero"));
    wl_hwcount_view(&hc, &v);
    ASSERT(!v.have[WL_HWM_IPC_M]);
    ASSERT(!v.have[WL_HWM_XF_WAIT_IN_PM]);
    ASSERT(has_line(&f, "verdict:"));
    PASS();
}

TEST a_counter_far_off_the_time_base_is_not_believed(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    // the clock that the monitor counts at is nothing like 12 cycles a tick: wait time claimed that
    // the engine did not see (a wait the application forgot to bracket would look the same)
    hw.cpu_clock_per_tick = 40;
    hc.hw.cpu_clock_per_tick = 40;
    run_frames(&hc, &f, 10, FRAME_TICKS, 0, 0, FRAME_TICKS / 2u);
    ASSERT(!hc.cpu_ok);
    ASSERT_EQ(0u, hc.cpu_sane_n);
    PASS();
}

TEST the_gpu_selftest_finds_the_slots_a_vertex_count_lands_in(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    fake_init(&f);
    f.sc.verts = 6;
    f.vertex_slot0 = 0;
    f.vertex_slot1 = 2;
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    wl_hwcount_selftest_start(&hc);
    wl_hwcount_selftest_stop(&hc, 6);
    ASSERT(hc.gp_ok && hc.gp_checked);
    ASSERT_EQ(0u, hc.gp_slot0);
    ASSERT_EQ(2u, hc.gp_slot1);
    ASSERT(has_line(&f, "gp: counters count (drew 6 vertices"));

    // a console that puts counter 1 somewhere else is followed, and one with a single slot loses perf1
    fake_init(&f);
    f.sc.verts = 6;
    f.vertex_slot0 = 1;
    f.vertex_slot1 = 3;
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    wl_hwcount_selftest_start(&hc);
    wl_hwcount_selftest_stop(&hc, 6);
    ASSERT_EQ(1u, hc.gp_slot0);
    ASSERT_EQ(3u, hc.gp_slot1);
    PASS();
}

TEST the_gpu_selftest_on_counters_that_stay_zero_says_no_counters(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    fake_init(&f);
    f.gp_counts = false;
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    wl_hwcount_selftest_start(&hc);
    wl_hwcount_selftest_stop(&hc, 6);
    ASSERT(!hc.gp_ok);
    ASSERT(has_line(&f, "gp: no counters (drew 6 vertices; slots read 0 0 0 0)"));
    // and the windows that follow measure the processor only
    run_frames(&hc, &f, 10, FRAME_TICKS, 0, 0, FRAME_TICKS / 2u);
    ASSERT(hc.cpu_ok);
    ASSERT_EQ(0u, hc.samples[WL_HWM_XF_WAIT_IN_PM].n);
    PASS();
}

TEST the_first_frames_are_let_go_by(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    s.first = 30;
    s.seconds = 3600;
    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    run_frames(&hc, &f, 30, FRAME_TICKS, 0, 0, FRAME_TICKS / 2u);
    ASSERT_EQ(0u, hc.samples[WL_HWM_CPU_KCYC].n);
    ASSERT(!hc.open);
    run_frames(&hc, &f, 4, FRAME_TICKS, 0, 0, FRAME_TICKS / 2u);
    ASSERT_EQ(3u, hc.samples[WL_HWM_CPU_KCYC].n); // (the first window opened at frame 31 closes at 32, ...)
    PASS();
}

TEST marks_record_when_the_processor_gave_a_token_and_when_the_gpu_passed_it(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;
    uint32_t i;

    s.seconds = 1;
    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    ASSERT(f.hook != NULL);
    hc.mark_names[2] = "world";
    for (i = 0; i < 40; i++) {
        uint64_t t0;

        wl_hwcount_begin_frame(&hc);
        t0 = f.now;
        f.now = t0 + TB / 1000u * 5u; // 5 ms in: the processor marks 2
        wl_hwcount_mark_at(&hc, 2);
        f.now = t0 + TB / 1000u * 9u; // the GPU passes it at 9 ms
        f.hook(f.hook_arg, f.tokens);
        f.now = t0 + FRAME_TICKS;
    }
    ASSERT(has_line(&f, "marks (ms into the frame, cpu/gpu): world 5.0/9.0"));
    PASS();
}

TEST a_token_is_stamped_once_and_older_ones_with_it(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    s.seconds = 3600;
    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    wl_hwcount_begin_frame(&hc);
    wl_hwcount_mark_at(&hc, 0);
    f.now += 100;
    wl_hwcount_mark_at(&hc, 1);
    f.now += 100;
    wl_hwcount_mark_at(&hc, 2);
    ASSERT_EQ(3u, hc.mark_count);
    f.now += 100;
    wl_hwcount_token_passed(&hc, 2); // the second token passed: the first two are stamped
    ASSERT(!hc.marks[0].pending && !hc.marks[1].pending && hc.marks[2].pending);
    ASSERT_EQ(hc.marks[0].gpu_tick, hc.marks[1].gpu_tick);
    f.now += 100;
    wl_hwcount_token_passed(&hc, 3);
    ASSERT(!hc.marks[2].pending);
    ASSERT(hc.marks[2].gpu_tick > hc.marks[1].gpu_tick);
    // a token number that wrapped past 16 bits still compares as later
    f.tokens = 0xFFFF;
    wl_hwcount_mark_at(&hc, 3);
    f.tokens = 0; // (the next token is 1 in the wrap)
    wl_hwcount_token_passed(&hc, 1);
    ASSERT(!hc.marks[3].pending);
    PASS();
}

TEST marks_past_the_table_and_outside_a_window_are_ignored(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;
    uint32_t i;

    s.seconds = 3600;
    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    wl_hwcount_mark_at(&hc, 1); // (no window open yet)
    ASSERT_EQ(0u, hc.mark_count);
    wl_hwcount_begin_frame(&hc);
    for (i = 0; i < WL_HWCOUNT_MARKS + 8u; i++) {
        wl_hwcount_mark_at(&hc, i);
    }
    ASSERT_EQ(WL_HWCOUNT_MARKS, hc.mark_count);
    PASS();
}

TEST the_fifo_samples_become_a_share_of_its_size(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;
    WlHwcountView v;

    s.seconds = 3600;
    fake_init(&f);
    hw = hw_of(&f);
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    hc.fifo_cap = 100000;
    f.sc.fifo = 25000;
    wl_hwcount_begin_frame(&hc);
    wl_hwcount_fifo_sample(&hc);
    f.sc.fifo = 75000;
    wl_hwcount_fifo_sample(&hc);
    f.now += FRAME_TICKS;
    wl_hwcount_begin_frame(&hc);
    wl_hwcount_view(&hc, &v);
    ASSERT_EQ(500u, v.fifo_mean_pm);
    ASSERT_EQ(750u, v.fifo_max_pm);
    hc.fifo_cap = 0;
    wl_hwcount_view(&hc, &v);
    ASSERT_EQ(UINT32_MAX, v.fifo_mean_pm);
    PASS();
}

TEST a_machine_with_nothing_but_a_clock_still_logs_frame_times(void) {
    static WlHwcount hc;
    static Fake f;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    fake_init(&f);
    f.have_cpu = false;
    f.have_gp = false;
    hw = hw_of(&f);
    hw.fifo_fill = NULL;
    hw.mark_token = NULL;
    ASSERT_EQ(WL_OK, wl_hwcount_start(&hc, &s, &hw));
    ASSERT(!hc.cpu_ok && !hc.gp_ok);
    run_frames(&hc, &f, 40, FRAME_TICKS, 0, 0, FRAME_TICKS / 2u);
    ASSERT(has_line(&f, "frame 33.33 ms"));
    ASSERT(has_line(&f, "cpu: no counters"));
    ASSERT(has_line(&f, "gp: no counters"));
    PASS();
}

TEST the_engine_refuses_a_machine_without_a_clock(void) {
    static WlHwcount hc;
    WlHwcountSettings s = quick_settings();
    WlHwcountHw hw;

    memset(&hw, 0, sizeof hw);
    ASSERT_EQ(WL_ERR_ARG, wl_hwcount_start(&hc, &s, &hw));
    hw.tb_hz = TB;
    ASSERT_EQ(WL_ERR_ARG, wl_hwcount_start(&hc, &s, &hw));
    PASS();
}

TEST the_application_face_is_inert_until_armed_and_not_found_without_a_file(void) {
    ASSERT(!wl_hwcount_armed());
    wl_hwcount_frame_begin(); // (all of these are tests of one flag)
    wl_hwcount_mark(1);
    wl_hwcount_wait_begin(WL_HWWAIT_GPU);
    wl_hwcount_wait_end(WL_HWWAIT_GPU);
    wl_hwcount_frame_end();
    wl_hwcount_selftest_begin();
    wl_hwcount_selftest_end(6);
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_hwcount_arm("wl_hwcount_no_such_folder"));
    ASSERT(!wl_hwcount_armed());
    PASS();
}

TEST the_platform_machine_has_a_clock(void) {
    const WlHwcountHw *hw = wl_hwcount_platform_hw();

    ASSERT(hw != NULL && hw->ticks != NULL && hw->tb_hz != 0);
    PASS();
}

SUITE(hwcount_suite) {
    RUN_TEST(registers_hold_the_events_at_the_manuals_bit_positions);
    RUN_TEST(every_group_selects_cycles_on_pmc1_and_fits_the_fields);
    RUN_TEST(counter_differences_wrap_modulo_2_to_the_32);
    RUN_TEST(the_median_takes_the_lower_middle_and_leaves_the_input_alone);
    RUN_TEST(the_control_file_overrides_the_defaults_and_reports_what_it_cannot_use);
    RUN_TEST(a_missing_control_file_is_not_found_and_a_present_one_loads);
    RUN_TEST(a_starved_gp_is_cpu_bound_and_the_memory_stalls_name_why);
    RUN_TEST(the_processors_cause_is_branches_then_pipeline_stalls_then_compute);
    RUN_TEST(a_busy_gpu_is_told_apart_by_the_part_that_stands_out);
    RUN_TEST(a_texture_cache_that_misses_under_a_busy_rasterizer_is_texture_bound);
    RUN_TEST(a_frame_that_waits_for_the_display_fits_and_none_measured_is_unknown);
    RUN_TEST(without_gpu_counters_the_fifo_and_the_waits_still_decide);
    RUN_TEST(thresholds_can_be_replaced);
    RUN_TEST(a_cpu_bound_run_logs_its_blocks_and_says_cpu_bound);
    RUN_TEST(the_groups_rotate_one_a_frame_so_all_are_seen_within_a_rotation);
    RUN_TEST(waits_are_left_out_of_the_processors_cycles);
    RUN_TEST(a_machine_whose_counters_read_zero_is_marked_invalid_and_logged_once);
    RUN_TEST(a_counter_far_off_the_time_base_is_not_believed);
    RUN_TEST(the_gpu_selftest_finds_the_slots_a_vertex_count_lands_in);
    RUN_TEST(the_gpu_selftest_on_counters_that_stay_zero_says_no_counters);
    RUN_TEST(the_first_frames_are_let_go_by);
    RUN_TEST(marks_record_when_the_processor_gave_a_token_and_when_the_gpu_passed_it);
    RUN_TEST(a_token_is_stamped_once_and_older_ones_with_it);
    RUN_TEST(marks_past_the_table_and_outside_a_window_are_ignored);
    RUN_TEST(the_fifo_samples_become_a_share_of_its_size);
    RUN_TEST(a_machine_with_nothing_but_a_clock_still_logs_frame_times);
    RUN_TEST(the_engine_refuses_a_machine_without_a_clock);
    RUN_TEST(the_application_face_is_inert_until_armed_and_not_found_without_a_file);
    RUN_TEST(the_platform_machine_has_a_clock);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(hwcount_suite);
    GREATEST_MAIN_END();
}
