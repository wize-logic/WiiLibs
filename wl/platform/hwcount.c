// SPDX-License-Identifier: MIT
// The hardware counters' engine (hwcount.h): the groups, a window per frame, the medians, the
// verdict and the log block. Portable; the machine is a WlHwcountHw.
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/hwcount.h"

#define SETTINGS_FILE_LIMIT (4u * 1024u)
#define LINE_MAX_CHARS 200u
#define GP_SLOT_NONE 0xFFu

// --- the groups ---------------------------------------------------------------------------------
// The 750's PMC events (IBM's 750CL user manual, the performance monitor chapter; the numbers per
// counter agree with the 750 event list Apple published for Shark). PMC1 is always 1, cycles.
//   PMC2: 2 instructions completed, 4 dispatched, 6 ITLB search cycles, 7 L2 hits, 10 L1 data miss
//         cycles above the threshold, 12 branch unit speculative stall cycles
//   PMC3: 5 L1 data misses, 6 DTLB misses, 7 data L2 misses, 11 FP instructions, 15 L1 data load
//         miss cycles, 17 branch unit LR/CTR stall cycles
//   PMC4: 2 instructions completed, 5 L2 castouts, 6 DTLB search cycles, 8 mispredicted branches,
//         12 snoop retries, 13 integer instructions
static const WlHwcpuGroup g_cpu_groups[WL_HWCOUNT_CPU_GROUPS] = {
    {"ipc", {1, 2, 5, 8}},      // instructions, L1 data misses, mispredicted branches
    {"dcache", {1, 10, 15, 6}}, // L1 data miss cycles, load miss cycles, DTLB search cycles
    {"l2", {1, 7, 7, 5}},       // L2 hits, data L2 misses, L2 castouts
    {"branch", {1, 12, 17, 2}}, // speculative stall cycles, LR/CTR stall cycles, instructions
    {"mix", {1, 4, 11, 13}},    // dispatched, FP instructions, integer instructions
    {"tlb", {1, 6, 6, 12}},     // ITLB search cycles, DTLB misses, snoop retries
};

// GX's metric numbers (libogc gx.h; wii/hwcount_wii.c checks them against the header).
enum {
    P0_XF_WAIT_IN = 3,
    P0_XF_XFRM_CLKS = 5,
    P0_XF_LIT_CLKS = 6,
    P0_XF_BOT_CLKS = 7,
    P0_TRIANGLES_PASSED = 13,
    P0_QUAD_NON0CVG = 28,
    P0_NONE = 35,
    P1_TEXELS = 0,
    P1_TX_IDLE = 1,
    P1_TX_MEMSTALL = 3,
    P1_TC_MISS = 8,
    P1_VC_ALL_STALLS = 15,
    P1_VERTICES = 16,
    P1_NONE = 22
};

static const WlHwgpGroup g_gp_groups[WL_HWCOUNT_GP_GROUPS] = {
    {"xfras", WL_HWGP_XFRAS, P0_NONE, P1_NONE},
    {"xfrm+verts", WL_HWGP_PAIR, P0_XF_XFRM_CLKS, P1_VERTICES},
    {"light+texels", WL_HWGP_PAIR, P0_XF_LIT_CLKS, P1_TEXELS},
    {"tris+txstall", WL_HWGP_PAIR, P0_TRIANGLES_PASSED, P1_TX_MEMSTALL},
    {"quads+tcmiss", WL_HWGP_PAIR, P0_QUAD_NON0CVG, P1_TC_MISS},
    {"waitin+txidle", WL_HWGP_PAIR, P0_XF_WAIT_IN, P1_TX_IDLE},
    {"bot+vcstall", WL_HWGP_PAIR, P0_XF_BOT_CLKS, P1_VC_ALL_STALLS},
};

const WlHwcpuGroup *wl_hwcount_cpu_group(uint32_t index) {
    return index < WL_HWCOUNT_CPU_GROUPS ? &g_cpu_groups[index] : NULL;
}

const WlHwgpGroup *wl_hwcount_gp_group(uint32_t index) {
    return index < WL_HWCOUNT_GP_GROUPS ? &g_gp_groups[index] : NULL;
}

// MMCR0 (IBM bit n is the value 1 << (31 - n)): DIS bit 0, DP 1, DU 2 (both left 0: count in
// supervisor and user mode), THRESHOLD 10 to 15, PMC1SELECT 19 to 25, PMC2SELECT 26 to 31.
// MMCR1: PMC3SELECT 0 to 4, PMC4SELECT 5 to 9.
uint32_t wl_hwcount_mmcr0(const uint8_t sel[4], uint32_t threshold) {
    return ((threshold & 0x3Fu) << 16) | ((uint32_t)(sel[0] & 0x7Fu) << 6) | (uint32_t)(sel[1] & 0x3Fu);
}

uint32_t wl_hwcount_mmcr1(const uint8_t sel[4]) {
    return ((uint32_t)(sel[2] & 0x1Fu) << 27) | ((uint32_t)(sel[3] & 0x1Fu) << 22);
}

static const char *const g_metric_names[WL_HWM_COUNT] = {
    "ipc", "l1d_miss_pki", "br_misp_pki", "dc_stall_pm", "dc_load_pm", "dtlb_search_pm", "itlb_search_pm",
    "dtlb_miss_pkc", "l2_hit_pkc", "l2_miss_pkc", "l2_castout_pkc", "br_stall_pm", "br_lrctr_pm", "fp_pm",
    "int_pm", "snoop_pkc", "cpu_kcyc", "ras_busy_pm", "xf_wait_in_pm", "xf_wait_out_pm", "xf_xfrm_pm",
    "xf_lit_pm", "xf_bot_pm", "tx_memstall_pm", "tx_idle_pm", "vc_stall_pm", "vcm_miss_pm", "vcm_stall_pm",
    "verts", "tris", "quads", "texels", "tc_miss", "tc_miss_pkt", "gp_mhz_x10"};

const char *wl_hwcount_metric_name(WlHwMetric metric) {
    return (unsigned)metric < WL_HWM_COUNT ? g_metric_names[metric] : "?";
}

// --- settings -----------------------------------------------------------------------------------
void wl_hwcount_settings_default(WlHwcountSettings *s) {
    memset(s, 0, sizeof *s);
    s->seconds = 1;
    s->cpu = 1;
    s->gp = 1;
    s->vcache = 1;
    s->timeline = 1;
    s->fifo_hz = 480;
    s->selftest = 1;
    s->threshold = 0;
    s->first = 60;
    s->raw = 0;
}

void wl_hwcount_settings_clamp(WlHwcountSettings *s) {
    if (s->seconds < 1) {
        s->seconds = 1;
    }
    if (s->seconds > 3600) {
        s->seconds = 3600;
    }
    if (s->fifo_hz > 10000) {
        s->fifo_hz = 10000;
    }
    if (s->threshold > 63) {
        s->threshold = 63;
    }
}

static bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

uint32_t wl_hwcount_settings_parse(WlHwcountSettings *s, const char *text, size_t length, WlHwcountIssueFn issue,
                                   void *user) {
    struct {
        const char *name;
        uint32_t *field;
    } const keys[] = {{"seconds", &s->seconds}, {"cpu", &s->cpu},           {"gp", &s->gp},
                      {"vcache", &s->vcache},   {"timeline", &s->timeline}, {"fifo_hz", &s->fifo_hz},
                      {"selftest", &s->selftest}, {"threshold", &s->threshold}, {"first", &s->first},
                      {"raw", &s->raw}};
    const char *p = text;
    const char *end = text + length;
    uint32_t line_no = 0;
    uint32_t reported = 0;

    while (p < end) {
        const char *eol = p;
        const char *q;
        const char *name;
        size_t name_len;
        char key[32];
        size_t i;

        line_no++;
        while (eol < end && *eol != '\n') {
            eol++;
        }
        q = p;
        while (q < eol && is_blank(*q)) {
            q++;
        }
        if (q < eol && *q != '#' && *q != '\r') {
            name = q;
            while (q < eol && !is_blank(*q) && *q != '=' && *q != '\r') {
                q++;
            }
            name_len = (size_t)(q - name);
            while (q < eol && (is_blank(*q) || *q == '=')) {
                q++;
            }
            if (q < eol && *q != '#' && *q != '\r') { // (a name alone is skipped)
                size_t found = sizeof keys / sizeof keys[0];

                for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
                    if (strlen(keys[i].name) == name_len && memcmp(keys[i].name, name, name_len) == 0) {
                        found = i;
                        break;
                    }
                }
                if (found == sizeof keys / sizeof keys[0]) {
                    size_t n = name_len < sizeof key - 1 ? name_len : sizeof key - 1;

                    memcpy(key, name, n);
                    key[n] = 0;
                    reported++;
                    if (issue != NULL) {
                        issue(user, 1, key, line_no);
                    }
                } else {
                    uint64_t value = 0;
                    const char *digits = q;

                    while (q < eol && *q >= '0' && *q <= '9') {
                        value = value * 10u + (uint64_t)(*q - '0');
                        if (value > UINT32_MAX) {
                            value = UINT32_MAX;
                        }
                        q++;
                    }
                    if (q == digits || (q < eol && !is_blank(*q) && *q != '#' && *q != '\r')) {
                        reported++;
                        if (issue != NULL) {
                            issue(user, 2, keys[found].name, line_no);
                        }
                    } else {
                        *keys[found].field = (uint32_t)value;
                    }
                }
            }
        }
        p = eol < end ? eol + 1 : end;
    }
    wl_hwcount_settings_clamp(s);
    return reported;
}

WlResult wl_hwcount_settings_load(WlHwcountSettings *s, const char *path, WlHwcountIssueFn issue, void *user) {
    FILE *f = fopen(path, "rb");
    char *text;
    size_t got;

    wl_hwcount_settings_default(s);
    if (f == NULL) {
        return WL_ERR_NOT_FOUND;
    }
    text = malloc(SETTINGS_FILE_LIMIT + 1u);
    if (text == NULL) {
        fclose(f);
        return WL_ERR_NOMEM;
    }
    got = fread(text, 1, SETTINGS_FILE_LIMIT + 1u, f);
    fclose(f);
    if (got > SETTINGS_FILE_LIMIT) {
        free(text);
        return WL_ERR_LIMIT;
    }
    (void)wl_hwcount_settings_parse(s, text, got, issue, user);
    free(text);
    return WL_OK;
}

// --- medians ------------------------------------------------------------------------------------
uint32_t wl_hwcount_median(const uint32_t *values, uint32_t count) {
    uint32_t tmp[WL_HWCOUNT_HISTORY];
    uint32_t i;
    uint32_t j;

    if (count == 0 || values == NULL) {
        return 0;
    }
    if (count > WL_HWCOUNT_HISTORY) {
        count = WL_HWCOUNT_HISTORY;
    }
    for (i = 0; i < count; i++) { // (insertion sort of a copy)
        uint32_t v = values[i];

        for (j = i; j > 0 && tmp[j - 1] > v; j--) {
            tmp[j] = tmp[j - 1];
        }
        tmp[j] = v;
    }
    return tmp[(count - 1u) / 2u];
}

static uint32_t ratio(uint64_t num, uint64_t den, uint32_t scale) {
    uint64_t v;

    if (den == 0) {
        return 0;
    }
    v = num * scale / den;
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

// --- the verdict --------------------------------------------------------------------------------
void wl_hwcount_thresholds_default(WlHwcountThresholds *t) {
    t->starved_wait_in_pm = 600;
    t->fifo_low_pm = 100;
    t->cpu_wait_gpu_pm = 100;
    t->fits_display_wait_pm = 100;
    t->target_frame_us = 33333;
    t->xform_pm = 300;
    t->fill_ras_pm = 500;
    t->fill_wait_out_pm = 300;
    t->texture_stall_pm = 150;
    t->texture_miss_pkt = 100;
    t->vertex_stall_pm = 150;
    t->cpu_memory_pm = 300;
    t->cpu_branch_pm = 150;
    t->cpu_mispredict_pki = 15;
    t->cpu_low_ipc_m = 600;
}

const char *wl_hwcount_verdict_name(WlHwVerdictKind kind) {
    switch (kind) {
    case WL_HWV_FITS:
        return "fits";
    case WL_HWV_CPU:
        return "CPU-bound";
    case WL_HWV_GPU_XFORM:
        return "transform-bound";
    case WL_HWV_GPU_FILL:
        return "fill/TEV-bound";
    case WL_HWV_GPU_TEXTURE:
        return "texture-bound";
    case WL_HWV_GPU_VERTEX:
        return "vertex-fetch-bound";
    case WL_HWV_GPU_MIXED:
        return "GPU-bound (mixed)";
    default:
        return "unknown";
    }
}

const char *wl_hwcount_cpu_kind_name(WlHwCpuKind kind) {
    switch (kind) {
    case WL_HWC_MEMORY:
        return "memory stalls";
    case WL_HWC_BRANCH:
        return "branches";
    case WL_HWC_STALLED:
        return "pipeline stalls";
    case WL_HWC_COMPUTE:
        return "compute";
    default:
        return "unknown";
    }
}

// Appends printf-formatted text to a buffer of `size` bytes, which stays terminated.
static void append(char *buf, size_t size, const char *fmt, ...) WL_PRINTF_FMT(3, 4);
static void append(char *buf, size_t size, const char *fmt, ...) {
    size_t used = strlen(buf);
    va_list ap;

    if (used + 1 >= size) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf + used, size - used, fmt, ap);
    va_end(ap);
}

static uint32_t pm_whole(uint32_t pm) {
    return pm / 10u;
}

static uint32_t pm_tenth(uint32_t pm) {
    return pm % 10u;
}

// The processor: why is it slow? Reads the CPU metrics that were measured.
static WlHwCpuKind classify_cpu(const WlHwcountView *v, const WlHwcountThresholds *t, char *why, size_t why_size) {
    uint32_t mem = 0;
    bool have_mem = v->have[WL_HWM_DC_STALL_PM] || v->have[WL_HWM_DC_LOAD_PM];

    if (v->have[WL_HWM_DC_STALL_PM]) {
        mem = v->m[WL_HWM_DC_STALL_PM];
    }
    if (v->have[WL_HWM_DC_LOAD_PM] && v->m[WL_HWM_DC_LOAD_PM] > mem) {
        mem = v->m[WL_HWM_DC_LOAD_PM];
    }
    if (!have_mem && !v->have[WL_HWM_IPC_M] && !v->have[WL_HWM_BR_STALL_PM] && !v->have[WL_HWM_BR_MISP_PKI]) {
        return WL_HWC_NONE;
    }
    if (have_mem && mem >= t->cpu_memory_pm) {
        append(why, why_size, "dcache stall %u.%u%%", pm_whole(mem), pm_tenth(mem));
        if (v->have[WL_HWM_L1D_MISS_PKI]) {
            append(why, why_size, ", %u L1D miss/ki", v->m[WL_HWM_L1D_MISS_PKI]);
        }
        return WL_HWC_MEMORY;
    }
    if ((v->have[WL_HWM_BR_STALL_PM] && v->m[WL_HWM_BR_STALL_PM] >= t->cpu_branch_pm) ||
        (v->have[WL_HWM_BR_MISP_PKI] && v->m[WL_HWM_BR_MISP_PKI] >= t->cpu_mispredict_pki)) {
        if (v->have[WL_HWM_BR_STALL_PM]) {
            append(why, why_size, "branch stall %u.%u%%", pm_whole(v->m[WL_HWM_BR_STALL_PM]),
                   pm_tenth(v->m[WL_HWM_BR_STALL_PM]));
        }
        if (v->have[WL_HWM_BR_MISP_PKI]) {
            append(why, why_size, "%s%u mispredicted/ki", v->have[WL_HWM_BR_STALL_PM] ? ", " : "",
                   v->m[WL_HWM_BR_MISP_PKI]);
        }
        return WL_HWC_BRANCH;
    }
    if (v->have[WL_HWM_IPC_M] && v->m[WL_HWM_IPC_M] < t->cpu_low_ipc_m) {
        append(why, why_size, "ipc %u.%02u with no memory or branch cause", v->m[WL_HWM_IPC_M] / 1000u,
               (v->m[WL_HWM_IPC_M] % 1000u) / 10u);
        return WL_HWC_STALLED;
    }
    if (v->have[WL_HWM_IPC_M]) {
        append(why, why_size, "ipc %u.%02u", v->m[WL_HWM_IPC_M] / 1000u, (v->m[WL_HWM_IPC_M] % 1000u) / 10u);
    }
    return WL_HWC_COMPUTE;
}

void wl_hwcount_verdict(const WlHwcountView *v, const WlHwcountThresholds *tp, WlHwcountVerdict *out) {
    WlHwcountThresholds defaults;
    const WlHwcountThresholds *t = tp;
    char why[120] = "";
    char cpu_why[80] = "";
    uint32_t gpu_wait_pm;
    uint32_t display_wait_pm;
    uint32_t known = 0;
    uint32_t votes = 0;
    bool wait_in_known;
    bool fifo_known;

    if (t == NULL) {
        wl_hwcount_thresholds_default(&defaults);
        t = &defaults;
    }
    memset(out, 0, sizeof *out);
    out->kind = WL_HWV_UNKNOWN;
    if (v->frame_us == 0) {
        snprintf(out->text, sizeof out->text, "unknown (no frames measured)");
        return;
    }
    gpu_wait_pm = ratio(v->gpu_wait_us, v->frame_us, 1000);
    display_wait_pm = ratio(v->display_wait_us, v->frame_us, 1000);
    wait_in_known = v->have[WL_HWM_XF_WAIT_IN_PM];
    fifo_known = v->fifo_mean_pm != UINT32_MAX;
    out->cpu = classify_cpu(v, t, cpu_why, sizeof cpu_why);

    // Done early, waiting for the display: nothing is the bottleneck at this frame time.
    if (v->frame_us <= t->target_frame_us + t->target_frame_us / 20u && display_wait_pm >= t->fits_display_wait_pm) {
        out->kind = WL_HWV_FITS;
        snprintf(out->text, sizeof out->text, "fits (frame %u.%u ms, waits %u.%u%% for the display)", v->frame_us / 1000u,
                 (v->frame_us % 1000u) / 100u, pm_whole(display_wait_pm), pm_tenth(display_wait_pm));
        return;
    }

    // Is the GP starved? Three witnesses, the majority of those measured decides: the XF waiting
    // for input, the FIFO nearly empty, the processor hardly waiting for the GPU.
    if (wait_in_known) {
        known++;
        if (v->m[WL_HWM_XF_WAIT_IN_PM] >= t->starved_wait_in_pm) {
            votes++;
        }
    }
    if (fifo_known) {
        known++;
        if (v->fifo_mean_pm <= t->fifo_low_pm) {
            votes++;
        }
    }
    known++; // the processor's wait on the GPU is always measured
    if (gpu_wait_pm < t->cpu_wait_gpu_pm) {
        votes++;
    }
    if (votes * 2u > known) {
        out->kind = WL_HWV_CPU;
        if (wait_in_known) {
            append(why, sizeof why, "xf wait-in %u.%u%%, ", pm_whole(v->m[WL_HWM_XF_WAIT_IN_PM]),
                   pm_tenth(v->m[WL_HWM_XF_WAIT_IN_PM]));
        }
        if (fifo_known) {
            append(why, sizeof why, "fifo %u.%u%% full, ", pm_whole(v->fifo_mean_pm), pm_tenth(v->fifo_mean_pm));
        }
        append(why, sizeof why, "gpu wait %u.%u%%", pm_whole(gpu_wait_pm), pm_tenth(gpu_wait_pm));
        if (out->cpu != WL_HWC_NONE) {
            snprintf(out->text, sizeof out->text, "CPU-bound (gp starved: %s); cpu: %s (%s)", why,
                     wl_hwcount_cpu_kind_name(out->cpu), cpu_why);
        } else {
            snprintf(out->text, sizeof out->text, "CPU-bound (gp starved: %s)", why);
        }
        return;
    }

    // The GPU is the one working: which part? Each candidate's score is its metric over its threshold.
    {
        uint32_t xform = 0;
        uint32_t fill = 0;
        uint32_t texture = 0;
        uint32_t vertex = 0;
        uint32_t best = 0;
        WlHwVerdictKind kind = WL_HWV_GPU_MIXED;

        if (v->have[WL_HWM_XF_XFRM_PM]) {
            xform = v->m[WL_HWM_XF_XFRM_PM];
            if (v->have[WL_HWM_XF_LIT_PM]) {
                xform += v->m[WL_HWM_XF_LIT_PM];
            }
        }
        xform = ratio(xform, t->xform_pm, 100);
        {
            uint32_t ras = v->have[WL_HWM_RAS_BUSY_PM] ? ratio(v->m[WL_HWM_RAS_BUSY_PM], t->fill_ras_pm, 100) : 0;
            uint32_t out_wait = v->have[WL_HWM_XF_WAIT_OUT_PM]
                                    ? ratio(v->m[WL_HWM_XF_WAIT_OUT_PM], t->fill_wait_out_pm, 100)
                                    : 0;

            fill = ras > out_wait ? ras : out_wait;
        }
        if (v->have[WL_HWM_TX_MEMSTALL_PM]) {
            texture = ratio(v->m[WL_HWM_TX_MEMSTALL_PM], t->texture_stall_pm, 100);
        }
        if (v->have[WL_HWM_TC_MISS_PKT] && v->m[WL_HWM_TC_MISS_PKT] >= t->texture_miss_pkt &&
            v->have[WL_HWM_RAS_BUSY_PM] && v->m[WL_HWM_RAS_BUSY_PM] >= 300) {
            uint32_t score = ratio(v->m[WL_HWM_TC_MISS_PKT], t->texture_miss_pkt, 100);

            if (score > texture) {
                texture = score;
            }
        }
        if (v->have[WL_HWM_VC_STALL_PM]) {
            vertex = ratio(v->m[WL_HWM_VC_STALL_PM], t->vertex_stall_pm, 100);
        }
        if (v->have[WL_HWM_VCM_STALL_PM]) {
            uint32_t score = ratio(v->m[WL_HWM_VCM_STALL_PM], t->vertex_stall_pm, 100);

            if (score > vertex) {
                vertex = score;
            }
        }
        // (a score of 100 is the threshold itself; below it nothing stands out)
        if (xform >= 100 && xform > best) {
            best = xform;
            kind = WL_HWV_GPU_XFORM;
        }
        if (fill >= 100 && fill > best) {
            best = fill;
            kind = WL_HWV_GPU_FILL;
        }
        if (texture >= 100 && texture > best) {
            best = texture;
            kind = WL_HWV_GPU_TEXTURE;
        }
        if (vertex >= 100 && vertex > best) {
            best = vertex;
            kind = WL_HWV_GPU_VERTEX;
        }
        out->kind = kind;
        switch (kind) {
        case WL_HWV_GPU_XFORM:
            snprintf(why, sizeof why, "xfrm %u.%u%% lit %u.%u%%", pm_whole(v->m[WL_HWM_XF_XFRM_PM]),
                     pm_tenth(v->m[WL_HWM_XF_XFRM_PM]), pm_whole(v->m[WL_HWM_XF_LIT_PM]),
                     pm_tenth(v->m[WL_HWM_XF_LIT_PM]));
            break;
        case WL_HWV_GPU_FILL:
            snprintf(why, sizeof why, "ras busy %u.%u%%, xf wait-out %u.%u%%", pm_whole(v->m[WL_HWM_RAS_BUSY_PM]),
                     pm_tenth(v->m[WL_HWM_RAS_BUSY_PM]), pm_whole(v->m[WL_HWM_XF_WAIT_OUT_PM]),
                     pm_tenth(v->m[WL_HWM_XF_WAIT_OUT_PM]));
            break;
        case WL_HWV_GPU_TEXTURE:
            snprintf(why, sizeof why, "tx mem stall %u.%u%%, tc miss %u/ktexel", pm_whole(v->m[WL_HWM_TX_MEMSTALL_PM]),
                     pm_tenth(v->m[WL_HWM_TX_MEMSTALL_PM]), v->m[WL_HWM_TC_MISS_PKT]);
            break;
        case WL_HWV_GPU_VERTEX:
            snprintf(why, sizeof why, "vertex cache stalls %u.%u%%", pm_whole(v->m[WL_HWM_VC_STALL_PM]),
                     pm_tenth(v->m[WL_HWM_VC_STALL_PM]));
            break;
        default:
            snprintf(why, sizeof why, "gpu wait %u.%u%%, xf wait-in %u.%u%%; no part over its threshold",
                     pm_whole(gpu_wait_pm), pm_tenth(gpu_wait_pm), pm_whole(v->m[WL_HWM_XF_WAIT_IN_PM]),
                     pm_tenth(v->m[WL_HWM_XF_WAIT_IN_PM]));
            break;
        }
        snprintf(out->text, sizeof out->text, "%s (%s)", wl_hwcount_verdict_name(kind), why);
    }
}

// --- the engine ---------------------------------------------------------------------------------
static void log_line(const WlHwcount *hc, const char *line) {
    if (hc->hw.log != NULL) {
        hc->hw.log(hc->hw.user, line);
    }
}

static void logf_line(const WlHwcount *hc, const char *fmt, ...) WL_PRINTF_FMT(2, 3);
static void logf_line(const WlHwcount *hc, const char *fmt, ...) {
    char line[LINE_MAX_CHARS + 1];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_line(hc, line);
}

static uint64_t now_ticks(const WlHwcount *hc) {
    return hc->hw.ticks != NULL ? hc->hw.ticks(hc->hw.user) : 0;
}

static void push(WlHwcount *hc, WlHwMetric metric, uint32_t value) {
    WlHwSamples *s = &hc->samples[metric];

    s->v[s->n % WL_HWCOUNT_HISTORY] = value;
    s->n++;
}

static bool token_le(uint32_t a, uint32_t b) {
    return (uint16_t)(b - a) < 0x8000u;
}

static void barrier(void) {
    __asm__ __volatile__("" ::: "memory");
}

void wl_hwcount_fifo_sample(WlHwcount *hc) {
    uint32_t fill;

    if (!hc->running || hc->hw.fifo_fill == NULL) {
        return;
    }
    fill = hc->hw.fifo_fill(hc->hw.user);
    if (fill == UINT32_MAX) {
        return;
    }
    hc->fifo_n++;
    hc->fifo_sum += fill;
    if (fill > hc->fifo_max) {
        hc->fifo_max = fill;
    }
    if (fill < hc->fifo_min) {
        hc->fifo_min = fill;
    }
}

static void fifo_tick(void *arg) {
    wl_hwcount_fifo_sample(arg);
}

static void token_hook(void *arg, uint32_t token) {
    wl_hwcount_token_passed(arg, token);
}

WlResult wl_hwcount_start(WlHwcount *hc, const WlHwcountSettings *settings, const WlHwcountHw *hw) {
    uint32_t probe[4];
    uint32_t i;

    if (hc == NULL || settings == NULL || hw == NULL || hw->ticks == NULL || hw->tb_hz == 0) {
        return WL_ERR_ARG;
    }
    memset(hc, 0, sizeof *hc);
    hc->s = *settings;
    wl_hwcount_settings_clamp(&hc->s);
    hc->hw = *hw;
    wl_hwcount_thresholds_default(&hc->thresholds);
    hc->gp_slot0 = 0;
    hc->gp_slot1 = 2;
    hc->gp_mult_x100 = hw->gp_clock_per_tick * 100u;
    hc->fifo_min = UINT32_MAX;
    hc->fifo_cap = 0;
    hc->cpu_ok = hc->s.cpu != 0 && hw->cpu_start != NULL && hw->cpu_read != NULL && hw->cpu_clock_per_tick != 0;
    hc->gp_ok = hc->s.gp != 0 && hw->gp_start != NULL && hw->gp_read != NULL && hw->gp_clock_per_tick != 0;
    if (!hc->s.selftest) {
        hc->gp_checked = true;
    }
    if (hc->cpu_ok) { // (a probe: the counters must move)
        hw->cpu_start(hw->user, g_cpu_groups[0].sel, hc->s.threshold);
        hw->cpu_read(hw->user, probe);
        if (hw->cpu_stop != NULL) {
            hw->cpu_stop(hw->user);
        }
    }
    for (i = 0; i < WL_HWCOUNT_MARKS; i++) {
        hc->mark_names[i] = NULL;
    }
    hc->running = true;
    hc->armed = true;
    if (hc->s.timeline && hw->set_token_hook != NULL) {
        hw->set_token_hook(hw->user, token_hook, hc);
    }
    if (hc->s.fifo_hz != 0 && hc->s.timeline && hw->fifo_sampler != NULL) {
        hw->fifo_sampler(hw->user, hc->s.fifo_hz, fifo_tick, hc);
    }
    logf_line(hc, "armed: cpu %s, gp %s, timeline %s, first window after %u frames, a block every %u s",
              hc->cpu_ok ? "on" : "off", hc->gp_ok ? "on" : "off", hc->s.timeline ? "on" : "off", hc->s.first,
              hc->s.seconds);
    return WL_OK;
}

void wl_hwcount_end(WlHwcount *hc) {
    if (!hc->running) {
        return;
    }
    hc->running = false;
    if (hc->hw.fifo_sampler != NULL) {
        hc->hw.fifo_sampler(hc->hw.user, 0, NULL, NULL);
    }
    if (hc->hw.set_token_hook != NULL) {
        hc->hw.set_token_hook(hc->hw.user, NULL, NULL);
    }
    if (hc->hw.cpu_stop != NULL) {
        hc->hw.cpu_stop(hc->hw.user);
    }
    if (hc->hw.gp_stop != NULL) {
        hc->hw.gp_stop(hc->hw.user);
    }
}

static void cpu_pause(WlHwcount *hc) {
    uint32_t p[4];
    uint32_t i;

    if (!hc->cpu_ok) {
        return;
    }
    hc->hw.cpu_read(hc->hw.user, p);
    for (i = 0; i < 4; i++) {
        hc->pmc_acc[i] += wl_hwcount_delta(hc->pmc_mark[i], p[i]);
    }
}

static void cpu_resume(WlHwcount *hc) {
    if (hc->cpu_ok) {
        hc->hw.cpu_read(hc->hw.user, hc->pmc_mark);
    }
}

void wl_hwcount_wait_start(WlHwcount *hc, WlHwWait kind) {
    if (!hc->running || !hc->open || (unsigned)kind >= WL_HWWAIT_COUNT) {
        return;
    }
    if (hc->wait_depth++ == 0) {
        cpu_pause(hc);
        hc->wait_t0 = now_ticks(hc);
    }
}

void wl_hwcount_wait_stop(WlHwcount *hc, WlHwWait kind) {
    if (!hc->running || !hc->open || (unsigned)kind >= WL_HWWAIT_COUNT || hc->wait_depth == 0) {
        return;
    }
    if (--hc->wait_depth == 0) {
        uint64_t now = now_ticks(hc);

        hc->wait_ticks[kind] += now - hc->wait_t0;
        cpu_resume(hc);
    }
}

void wl_hwcount_token_passed(WlHwcount *hc, uint32_t token) {
    uint64_t now;
    uint32_t i;
    uint32_t n = hc->mark_count;

    if (n == 0) {
        return;
    }
    now = now_ticks(hc);
    for (i = 0; i < n && i < WL_HWCOUNT_MARKS; i++) {
        WlHwMark *m = &hc->marks[i];

        if (m->pending && token_le(m->token, token)) {
            m->gpu_tick = now;
            barrier();
            m->pending = 0;
        }
    }
}

void wl_hwcount_mark_at(WlHwcount *hc, uint32_t id) {
    WlHwMark *m;
    uint32_t token;

    if (!hc->running || !hc->open || !hc->s.timeline || hc->hw.mark_token == NULL || hc->mark_count >= WL_HWCOUNT_MARKS) {
        return;
    }
    m = &hc->marks[hc->mark_count];
    m->pending = 0;
    m->id = id < WL_HWCOUNT_MARKS ? id : WL_HWCOUNT_MARKS - 1u;
    m->cpu_tick = now_ticks(hc);
    m->gpu_tick = 0;
    wl_hwcount_fifo_sample(hc);
    token = hc->hw.mark_token(hc->hw.user);
    m->token = token;
    m->fifo = hc->hw.fifo_fill != NULL ? hc->hw.fifo_fill(hc->hw.user) : UINT32_MAX;
    barrier();
    hc->mark_count++;
    barrier();
    m->pending = token != 0 ? 1u : 0u;
}

static void open_window(WlHwcount *hc, uint64_t now) {
    uint32_t window_no = hc->frames - hc->s.first - 1u;
    const WlHwcpuGroup *cg = &g_cpu_groups[window_no % WL_HWCOUNT_CPU_GROUPS];
    const WlHwgpGroup *gg = &g_gp_groups[window_no % WL_HWCOUNT_GP_GROUPS];

    hc->cpu_group = window_no % WL_HWCOUNT_CPU_GROUPS;
    hc->gp_group = window_no % WL_HWCOUNT_GP_GROUPS;
    memset(hc->pmc_acc, 0, sizeof hc->pmc_acc);
    memset(hc->pmc_mark, 0, sizeof hc->pmc_mark);
    memset(hc->wait_ticks, 0, sizeof hc->wait_ticks);
    hc->wait_depth = 0;
    hc->gpu_done_ticks = 0;
    if (hc->cpu_ok) {
        hc->hw.cpu_start(hc->hw.user, cg->sel, hc->s.threshold);
    }
    if (hc->gp_ok) {
        hc->hw.gp_start(hc->hw.user, gg, hc->s.vcache != 0);
    }
    hc->fifo_n_prev = hc->fifo_n;
    hc->fifo_sum_prev = hc->fifo_sum;
    hc->fifo_max = 0;
    hc->fifo_min = UINT32_MAX;
    hc->mark_count = 0;
    hc->t_begin = now;
    hc->open = true;
}

static void gp_metrics(WlHwcount *hc, const WlHwWindow *w) {
    const WlHwgpGroup *g = &g_gp_groups[w->gp_group];
    uint64_t gclk = w->ticks * hc->gp_mult_x100 / 100u;
    uint32_t v0 = w->slot[hc->gp_slot0 & 3u];
    uint32_t v1 = hc->gp_slot1 != GP_SLOT_NONE ? w->slot[hc->gp_slot1 & 3u] : 0;
    bool have1 = hc->gp_slot1 != GP_SLOT_NONE;

    if (g->kind == WL_HWGP_XFRAS) {
        uint32_t clks = w->slot[1];

        if (clks != 0) {
            uint64_t per_tick_x100 = (uint64_t)clks * 100u / (w->ticks != 0 ? w->ticks : 1u);
            uint64_t dflt = hc->hw.gp_clock_per_tick * 100u;

            push(hc, WL_HWM_RAS_BUSY_PM, ratio(w->slot[0], clks, 1000));
            push(hc, WL_HWM_XF_WAIT_IN_PM, ratio(w->slot[2], clks, 1000));
            push(hc, WL_HWM_XF_WAIT_OUT_PM, ratio(w->slot[3], clks, 1000));
            push(hc, WL_HWM_GP_MHZ_X10, (uint32_t)(((uint64_t)clks * 10u * hc->hw.tb_hz) / (w->ticks != 0 ? w->ticks : 1u) /
                                                  1000000u));
            if (per_tick_x100 * 2u >= dflt && per_tick_x100 <= dflt * 2u) {
                hc->gp_mult_x100 = (uint32_t)per_tick_x100; // (the clock as measured replaces the nominal one)
            }
        }
    } else {
        switch (g->perf0) {
        case P0_XF_WAIT_IN:
            push(hc, WL_HWM_XF_WAIT_IN_PM, (uint32_t)ratio(v0, gclk, 1000));
            break;
        case P0_XF_XFRM_CLKS:
            push(hc, WL_HWM_XF_XFRM_PM, (uint32_t)ratio(v0, gclk, 1000));
            break;
        case P0_XF_LIT_CLKS:
            push(hc, WL_HWM_XF_LIT_PM, (uint32_t)ratio(v0, gclk, 1000));
            break;
        case P0_XF_BOT_CLKS:
            push(hc, WL_HWM_XF_BOT_PM, (uint32_t)ratio(v0, gclk, 1000));
            break;
        case P0_TRIANGLES_PASSED:
            push(hc, WL_HWM_TRIS, v0);
            break;
        case P0_QUAD_NON0CVG:
            push(hc, WL_HWM_QUADS, v0);
            break;
        default:
            break;
        }
        if (have1) {
            switch (g->perf1) {
            case P1_VERTICES:
                push(hc, WL_HWM_VERTS, v1);
                break;
            case P1_TEXELS:
                push(hc, WL_HWM_TEXELS, v1);
                break;
            case P1_TX_MEMSTALL:
                push(hc, WL_HWM_TX_MEMSTALL_PM, (uint32_t)ratio(v1, gclk, 1000));
                break;
            case P1_TX_IDLE:
                push(hc, WL_HWM_TX_IDLE_PM, (uint32_t)ratio(v1, gclk, 1000));
                break;
            case P1_TC_MISS:
                push(hc, WL_HWM_TC_MISS, v1);
                break;
            case P1_VC_ALL_STALLS:
                push(hc, WL_HWM_VC_STALL_PM, (uint32_t)ratio(v1, gclk, 1000));
                break;
            default:
                break;
            }
        }
    }
    if (hc->s.vcache && w->vcache[0] != 0) {
        push(hc, WL_HWM_VCM_MISS_PM, ratio(w->vcache[1], w->vcache[0], 1000));
        push(hc, WL_HWM_VCM_STALL_PM, (uint32_t)ratio(w->vcache[2], gclk, 1000));
    }
}

static void cpu_metrics(WlHwcount *hc, const WlHwWindow *w) {
    uint32_t c = w->pmc[0];

    push(hc, WL_HWM_CPU_KCYC, c / 1000u);
    switch (w->cpu_group) {
    case 0:
        push(hc, WL_HWM_IPC_M, ratio(w->pmc[1], c, 1000));
        push(hc, WL_HWM_L1D_MISS_PKI, ratio(w->pmc[2], w->pmc[1], 1000));
        push(hc, WL_HWM_BR_MISP_PKI, ratio(w->pmc[3], w->pmc[1], 1000));
        break;
    case 1:
        push(hc, WL_HWM_DC_STALL_PM, ratio(w->pmc[1], c, 1000));
        push(hc, WL_HWM_DC_LOAD_PM, ratio(w->pmc[2], c, 1000));
        push(hc, WL_HWM_DTLB_SEARCH_PM, ratio(w->pmc[3], c, 1000));
        break;
    case 2:
        push(hc, WL_HWM_L2_HIT_PKC, ratio(w->pmc[1], c, 1000));
        push(hc, WL_HWM_L2_MISS_PKC, ratio(w->pmc[2], c, 1000));
        push(hc, WL_HWM_L2_CASTOUT_PKC, ratio(w->pmc[3], c, 1000));
        break;
    case 3:
        push(hc, WL_HWM_BR_STALL_PM, ratio(w->pmc[1], c, 1000));
        push(hc, WL_HWM_BR_LRCTR_PM, ratio(w->pmc[2], c, 1000));
        push(hc, WL_HWM_IPC_M, ratio(w->pmc[3], c, 1000));
        break;
    case 4:
        push(hc, WL_HWM_FP_PM, ratio(w->pmc[2], w->pmc[1], 1000));
        push(hc, WL_HWM_INT_PM, ratio(w->pmc[3], w->pmc[1], 1000));
        break;
    default:
        push(hc, WL_HWM_ITLB_SEARCH_PM, ratio(w->pmc[1], c, 1000));
        push(hc, WL_HWM_DTLB_MISS_PKC, ratio(w->pmc[2], c, 1000));
        push(hc, WL_HWM_SNOOP_PKC, ratio(w->pmc[3], c, 1000));
        break;
    }
}

static void close_window(WlHwcount *hc, uint64_t now) {
    WlHwWindow w;
    uint32_t i;
    uint64_t busy;

    memset(&w, 0, sizeof w);
    w.cpu_group = hc->cpu_group;
    w.gp_group = hc->gp_group;
    w.ticks = now - hc->t_begin;
    for (i = 0; i < WL_HWWAIT_COUNT; i++) {
        w.wait_ticks[i] = hc->wait_ticks[i];
    }
    w.gpu_done_ticks = hc->gpu_done_ticks;
    if (hc->cpu_ok) {
        cpu_pause(hc);
        for (i = 0; i < 4; i++) {
            w.pmc[i] = hc->pmc_acc[i];
            hc->raw_pmc[w.cpu_group][i] = w.pmc[i];
        }
    }
    if (hc->gp_ok) {
        hc->hw.gp_read(hc->hw.user, w.slot, w.vcache);
        for (i = 0; i < 4; i++) {
            hc->raw_slot[w.gp_group][i] = w.slot[i];
        }
        for (i = 0; i < 3; i++) {
            hc->raw_vc[i] = w.vcache[i];
        }
    }
    w.fifo_n = hc->fifo_n - hc->fifo_n_prev;
    w.fifo_sum = hc->fifo_sum - hc->fifo_sum_prev;
    w.fifo_max = hc->fifo_max;
    w.fifo_min = hc->fifo_min;

    // The frame
    hc->log_frames++;
    hc->log_ticks += w.ticks;
    if (w.ticks > hc->log_worst_ticks) {
        hc->log_worst_ticks = w.ticks;
    }
    for (i = 0; i < WL_HWWAIT_COUNT; i++) {
        hc->log_wait_ticks[i] += w.wait_ticks[i];
    }
    if (w.gpu_done_ticks != 0) {
        hc->log_gpu_done_ticks += w.gpu_done_ticks;
        hc->log_gpu_done_n++;
    }
    if (w.fifo_n != 0) {
        hc->log_fifo_sum += w.fifo_sum;
        hc->log_fifo_n += w.fifo_n;
        if (w.fifo_max > hc->log_fifo_max) {
            hc->log_fifo_max = w.fifo_max;
        }
    }
    for (i = 0; i < hc->mark_count && i < WL_HWCOUNT_MARKS; i++) {
        const WlHwMark *m = &hc->marks[i];

        if (m->cpu_tick >= hc->t_begin) {
            hc->mark_cpu_sum[m->id] += m->cpu_tick - hc->t_begin;
            hc->mark_n[m->id]++;
            if (!m->pending && m->gpu_tick >= hc->t_begin) {
                hc->mark_gpu_sum[m->id] += m->gpu_tick - hc->t_begin;
                hc->mark_gpu_n[m->id]++;
            }
        }
    }

    // The processor: the cycles must follow the time base (12 a tick) or the counters are not counting.
    if (hc->cpu_ok) {
        uint64_t expect;

        busy = w.ticks - w.wait_ticks[WL_HWWAIT_GPU] - w.wait_ticks[WL_HWWAIT_DISPLAY];
        expect = busy * hc->hw.cpu_clock_per_tick;
        if (w.pmc[0] != 0 && (uint64_t)w.pmc[0] * 10u >= expect * 7u && (uint64_t)w.pmc[0] * 10u <= expect * 13u) {
            hc->cpu_sane_n++;
            cpu_metrics(hc, &w);
        } else {
            hc->cpu_insane_n++;
            if (hc->cpu_sane_n == 0 && hc->cpu_insane_n >= 3) {
                hc->cpu_ok = false;
                logf_line(hc, "cpu: no counters (cycles read %u for %u expected)", w.pmc[0], (uint32_t)expect);
            }
        }
    }

    // The GPU: a clock that never ticks in the XF/RAS group means the counters are not there.
    if (hc->gp_ok) {
        uint32_t any = w.slot[0] | w.slot[1] | w.slot[2] | w.slot[3] | w.vcache[0] | w.vcache[1] | w.vcache[2];

        hc->gp_windows++;
        if (any != 0) {
            hc->gp_nonzero++;
        }
        if (!hc->gp_checked && hc->gp_windows >= 2u * WL_HWCOUNT_GP_GROUPS && hc->gp_nonzero == 0) {
            hc->gp_ok = false;
            hc->gp_checked = true;
            logf_line(hc, "gp: no counters (every group read zero over %u frames)", hc->gp_windows);
        } else if (any != 0 && !hc->selftest_skip) {
            gp_metrics(hc, &w);
        }
        hc->selftest_skip = false;
    }
}

void wl_hwcount_begin_frame(WlHwcount *hc) {
    uint64_t now;

    if (!hc->running) {
        return;
    }
    now = now_ticks(hc);
    if (hc->open) {
        close_window(hc, now);
        hc->open = false;
        if (hc->next_log == 0) {
            hc->next_log = now + hc->hw.tb_hz * hc->s.seconds;
        } else if (now >= hc->next_log) {
            (void)wl_hwcount_report(hc);
            hc->next_log = now + hc->hw.tb_hz * hc->s.seconds;
        }
    }
    hc->frames++;
    if (hc->frames > hc->s.first) {
        open_window(hc, now);
    }
}

void wl_hwcount_end_frame(WlHwcount *hc) {
    if (!hc->running || !hc->open) {
        return;
    }
    hc->gpu_done_ticks = now_ticks(hc) - hc->t_begin;
    wl_hwcount_fifo_sample(hc);
}

// --- the GPU self-test ----------------------------------------------------------------------------
void wl_hwcount_selftest_start(WlHwcount *hc) {
    static const WlHwgpGroup test = {"selftest", WL_HWGP_PAIR, P0_NONE, P1_VERTICES};
    WlHwgpGroup both = test;

    if (!hc->running || !hc->gp_ok || hc->hw.gp_start == NULL) {
        return;
    }
    both.perf0 = 0; // GX_PERF0_VERTICES
    hc->hw.gp_start(hc->hw.user, &both, false);
    hc->selftest_open = true;
}

void wl_hwcount_selftest_stop(WlHwcount *hc, uint32_t vertices) {
    uint32_t slot[4];
    uint32_t vc[3];
    uint32_t mask = 0;
    uint32_t i;

    if (!hc->running || !hc->selftest_open) {
        return;
    }
    hc->selftest_open = false;
    hc->selftest_skip = true; // (the window in progress now holds this test's configuration)
    hc->hw.gp_read(hc->hw.user, slot, vc);
    for (i = 0; i < 4; i++) {
        if (slot[i] == vertices && vertices != 0) {
            mask |= 1u << i;
        }
    }
    hc->gp_checked = true;
    if (mask == 0) {
        hc->gp_ok = false;
        logf_line(hc, "gp: no counters (drew %u vertices; slots read %u %u %u %u)", vertices, slot[0], slot[1], slot[2],
                  slot[3]);
        return;
    }
    hc->gp_slot0 = 0;
    hc->gp_slot1 = GP_SLOT_NONE;
    {
        uint32_t found = 0;

        for (i = 0; i < 4; i++) {
            if (mask & (1u << i)) {
                if (found == 0) {
                    hc->gp_slot0 = i;
                } else if (found == 1) {
                    hc->gp_slot1 = i;
                }
                found++;
            }
        }
    }
    logf_line(hc, "gp: counters count (drew %u vertices; slots read %u %u %u %u; perf0 on slot %u, perf1 on slot %d)",
              vertices, slot[0], slot[1], slot[2], slot[3], hc->gp_slot0,
              hc->gp_slot1 == GP_SLOT_NONE ? -1 : (int)hc->gp_slot1);
}

// --- the view and the log ---------------------------------------------------------------------------
void wl_hwcount_view(const WlHwcount *hc, WlHwcountView *v) {
    uint32_t i;

    memset(v, 0, sizeof *v);
    for (i = 0; i < WL_HWM_COUNT; i++) {
        uint32_t n = hc->samples[i].n < WL_HWCOUNT_HISTORY ? hc->samples[i].n : WL_HWCOUNT_HISTORY;

        if (n != 0) {
            v->have[i] = true;
            v->m[i] = wl_hwcount_median(hc->samples[i].v, n);
        }
    }
    if (v->have[WL_HWM_TC_MISS] && v->have[WL_HWM_TEXELS] && v->m[WL_HWM_TEXELS] != 0) {
        v->have[WL_HWM_TC_MISS_PKT] = true;
        v->m[WL_HWM_TC_MISS_PKT] = ratio(v->m[WL_HWM_TC_MISS], v->m[WL_HWM_TEXELS], 1000);
    }
    if (hc->log_frames != 0) {
        v->frame_us = (uint32_t)(hc->log_ticks * 1000000u / hc->hw.tb_hz / hc->log_frames);
        v->gpu_wait_us = (uint32_t)(hc->log_wait_ticks[WL_HWWAIT_GPU] * 1000000u / hc->hw.tb_hz / hc->log_frames);
        v->display_wait_us = (uint32_t)(hc->log_wait_ticks[WL_HWWAIT_DISPLAY] * 1000000u / hc->hw.tb_hz / hc->log_frames);
    }
    v->fifo_mean_pm = UINT32_MAX;
    v->fifo_max_pm = UINT32_MAX;
    if (hc->fifo_cap != 0 && hc->log_fifo_n != 0) {
        v->fifo_mean_pm = ratio(hc->log_fifo_sum / hc->log_fifo_n, hc->fifo_cap, 1000);
        v->fifo_max_pm = ratio(hc->log_fifo_max, hc->fifo_cap, 1000);
    }
}

static void put_pm(char *line, size_t size, const char *label, const WlHwcountView *v, WlHwMetric m) {
    if (v->have[m]) {
        append(line, size, " %s %u.%u%%", label, pm_whole(v->m[m]), pm_tenth(v->m[m]));
    } else {
        append(line, size, " %s -", label);
    }
}

static void put_count(char *line, size_t size, const char *label, const WlHwcountView *v, WlHwMetric m) {
    if (v->have[m]) {
        append(line, size, " %s %u", label, v->m[m]);
    } else {
        append(line, size, " %s -", label);
    }
}

static void put_ms(char *line, size_t size, const char *label, uint64_t ticks, uint64_t tb_hz) {
    uint64_t us = ticks * 1000000u / tb_hz;

    append(line, size, " %s %u.%02u ms", label, (unsigned)(us / 1000u), (unsigned)((us % 1000u) / 10u));
}

uint32_t wl_hwcount_report(WlHwcount *hc) {
    WlHwcountView v;
    WlHwcountVerdict verdict;
    char line[LINE_MAX_CHARS + 1];
    uint32_t lines = 0;
    uint32_t i;

    if (hc->log_frames == 0) {
        return 0;
    }
    wl_hwcount_view(hc, &v);
    hc->log_blocks++;

    line[0] = 0;
    append(line, sizeof line, "block %u: %u frames,", hc->log_blocks, hc->log_frames);
    put_ms(line, sizeof line, "frame", hc->log_ticks / hc->log_frames, hc->hw.tb_hz);
    put_ms(line, sizeof line, "(worst", hc->log_worst_ticks, hc->hw.tb_hz);
    append(line, sizeof line, "), cpu waits:");
    put_ms(line, sizeof line, "gpu", hc->log_wait_ticks[WL_HWWAIT_GPU] / hc->log_frames, hc->hw.tb_hz);
    put_ms(line, sizeof line, "display", hc->log_wait_ticks[WL_HWWAIT_DISPLAY] / hc->log_frames, hc->hw.tb_hz);
    if (hc->log_gpu_done_n != 0) {
        put_ms(line, sizeof line, "| gpu done at", hc->log_gpu_done_ticks / hc->log_gpu_done_n, hc->hw.tb_hz);
    }
    log_line(hc, line);
    lines++;

    if (hc->s.cpu) {
        if (!hc->cpu_ok) {
            log_line(hc, "cpu: no counters");
        } else {
            line[0] = 0;
            append(line, sizeof line, "cpu:");
            if (v.have[WL_HWM_IPC_M]) {
                append(line, sizeof line, " ipc %u.%02u", v.m[WL_HWM_IPC_M] / 1000u, (v.m[WL_HWM_IPC_M] % 1000u) / 10u);
            } else {
                append(line, sizeof line, " ipc -");
            }
            put_count(line, sizeof line, "| L1D miss/ki", &v, WL_HWM_L1D_MISS_PKI);
            put_pm(line, sizeof line, "stall", &v, WL_HWM_DC_STALL_PM);
            put_pm(line, sizeof line, "load", &v, WL_HWM_DC_LOAD_PM);
            put_count(line, sizeof line, "| L2 miss/kc", &v, WL_HWM_L2_MISS_PKC);
            put_count(line, sizeof line, "hit/kc", &v, WL_HWM_L2_HIT_PKC);
            put_count(line, sizeof line, "castout/kc", &v, WL_HWM_L2_CASTOUT_PKC);
            log_line(hc, line);
            lines++;
            line[0] = 0;
            append(line, sizeof line, "cpu:");
            put_count(line, sizeof line, "br mispredict/ki", &v, WL_HWM_BR_MISP_PKI);
            put_pm(line, sizeof line, "stall", &v, WL_HWM_BR_STALL_PM);
            put_pm(line, sizeof line, "lr/ctr", &v, WL_HWM_BR_LRCTR_PM);
            put_pm(line, sizeof line, "| fp", &v, WL_HWM_FP_PM);
            put_pm(line, sizeof line, "int", &v, WL_HWM_INT_PM);
            put_pm(line, sizeof line, "| dtlb search", &v, WL_HWM_DTLB_SEARCH_PM);
            put_pm(line, sizeof line, "itlb", &v, WL_HWM_ITLB_SEARCH_PM);
            put_count(line, sizeof line, "| kcycles", &v, WL_HWM_CPU_KCYC);
            log_line(hc, line);
            lines++;
        }
    }
    if (hc->s.gp) {
        if (!hc->gp_ok) {
            log_line(hc, "gp: no counters");
        } else {
            line[0] = 0;
            append(line, sizeof line, "gp:");
            put_pm(line, sizeof line, "xf wait-in", &v, WL_HWM_XF_WAIT_IN_PM);
            put_pm(line, sizeof line, "wait-out", &v, WL_HWM_XF_WAIT_OUT_PM);
            put_pm(line, sizeof line, "| ras busy", &v, WL_HWM_RAS_BUSY_PM);
            put_pm(line, sizeof line, "| xfrm", &v, WL_HWM_XF_XFRM_PM);
            put_pm(line, sizeof line, "lit", &v, WL_HWM_XF_LIT_PM);
            put_pm(line, sizeof line, "bot", &v, WL_HWM_XF_BOT_PM);
            if (v.have[WL_HWM_GP_MHZ_X10]) {
                append(line, sizeof line, " | clk %u.%u MHz", v.m[WL_HWM_GP_MHZ_X10] / 10u, v.m[WL_HWM_GP_MHZ_X10] % 10u);
            }
            log_line(hc, line);
            lines++;
            line[0] = 0;
            append(line, sizeof line, "gp:");
            put_count(line, sizeof line, "verts", &v, WL_HWM_VERTS);
            put_count(line, sizeof line, "tris", &v, WL_HWM_TRIS);
            put_count(line, sizeof line, "quads", &v, WL_HWM_QUADS);
            put_count(line, sizeof line, "texels", &v, WL_HWM_TEXELS);
            put_count(line, sizeof line, "| tc miss/ktexel", &v, WL_HWM_TC_MISS_PKT);
            put_pm(line, sizeof line, "tx stall", &v, WL_HWM_TX_MEMSTALL_PM);
            put_pm(line, sizeof line, "idle", &v, WL_HWM_TX_IDLE_PM);
            put_pm(line, sizeof line, "| vc stall", &v, WL_HWM_VC_STALL_PM);
            put_pm(line, sizeof line, "vcache miss", &v, WL_HWM_VCM_MISS_PM);
            log_line(hc, line);
            lines++;
        }
    }
    if (v.fifo_mean_pm != UINT32_MAX) {
        logf_line(hc, "fifo: mean %u.%u%% max %u.%u%% of %u bytes (%u samples)", pm_whole(v.fifo_mean_pm),
                  pm_tenth(v.fifo_mean_pm), pm_whole(v.fifo_max_pm), pm_tenth(v.fifo_max_pm), hc->fifo_cap,
                  hc->log_fifo_n);
        lines++;
    } else if (hc->log_fifo_n != 0) {
        logf_line(hc, "fifo: %u samples, size unknown", hc->log_fifo_n);
        lines++;
    }
    line[0] = 0;
    for (i = 0; i < WL_HWCOUNT_MARKS; i++) {
        if (hc->mark_n[i] == 0) {
            continue;
        }
        if (line[0] == 0) {
            append(line, sizeof line, "marks (ms into the frame, cpu/gpu):");
        }
        if (hc->mark_names[i] != NULL) {
            append(line, sizeof line, " %s", hc->mark_names[i]);
        } else {
            append(line, sizeof line, " #%u", i);
        }
        {
            uint64_t cpu_us = hc->mark_cpu_sum[i] / hc->mark_n[i] * 1000000u / hc->hw.tb_hz;

            append(line, sizeof line, " %u.%u", (unsigned)(cpu_us / 1000u), (unsigned)((cpu_us % 1000u) / 100u));
        }
        if (hc->mark_gpu_n[i] != 0) {
            uint64_t gpu_us = hc->mark_gpu_sum[i] / hc->mark_gpu_n[i] * 1000000u / hc->hw.tb_hz;

            append(line, sizeof line, "/%u.%u", (unsigned)(gpu_us / 1000u), (unsigned)((gpu_us % 1000u) / 100u));
        } else {
            append(line, sizeof line, "/-");
        }
        if (strlen(line) > LINE_MAX_CHARS - 24u) {
            log_line(hc, line);
            lines++;
            line[0] = 0;
        }
    }
    if (line[0] != 0) {
        log_line(hc, line);
        lines++;
    }
    if (hc->s.raw) {
        for (i = 0; i < WL_HWCOUNT_CPU_GROUPS; i++) {
            logf_line(hc, "raw cpu %s: %u %u %u %u", g_cpu_groups[i].name, hc->raw_pmc[i][0], hc->raw_pmc[i][1],
                      hc->raw_pmc[i][2], hc->raw_pmc[i][3]);
            lines++;
        }
        for (i = 0; i < WL_HWCOUNT_GP_GROUPS; i++) {
            logf_line(hc, "raw gp %s: %u %u %u %u", g_gp_groups[i].name, hc->raw_slot[i][0], hc->raw_slot[i][1],
                      hc->raw_slot[i][2], hc->raw_slot[i][3]);
            lines++;
        }
        logf_line(hc, "raw vcache: check %u miss %u stall %u", hc->raw_vc[0], hc->raw_vc[1], hc->raw_vc[2]);
        lines++;
    }
    wl_hwcount_verdict(&v, &hc->thresholds, &verdict);
    logf_line(hc, "verdict: %s", verdict.text);
    lines++;

    // a new period
    memset(hc->samples, 0, sizeof hc->samples);
    hc->log_frames = 0;
    hc->log_ticks = 0;
    hc->log_worst_ticks = 0;
    memset(hc->log_wait_ticks, 0, sizeof hc->log_wait_ticks);
    hc->log_gpu_done_ticks = 0;
    hc->log_gpu_done_n = 0;
    hc->log_fifo_sum = 0;
    hc->log_fifo_n = 0;
    hc->log_fifo_max = 0;
    memset(hc->mark_cpu_sum, 0, sizeof hc->mark_cpu_sum);
    memset(hc->mark_gpu_sum, 0, sizeof hc->mark_gpu_sum);
    memset(hc->mark_n, 0, sizeof hc->mark_n);
    memset(hc->mark_gpu_n, 0, sizeof hc->mark_gpu_n);
    return lines;
}
