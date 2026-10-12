// SPDX-License-Identifier: MIT
// The console measures itself: the Broadway's performance monitor and the GX pipeline's counters,
// read once a frame, rotated through groups of events, reduced to medians once a second and
// turned into a verdict on what bounds the frame (the processor or the GPU, and which part of
// either). Where the sampling profiler (profile.h) says which functions take the time, this says
// which hardware resource the time is spent waiting for.
//
//   - CPU: the 750CL's four performance counters (PMC1 to PMC4, set by MMCR0 and MMCR1; IBM's
//     "PowerPC 750CL RISC Microprocessor User's Manual", the performance monitor chapter). PMC1
//     always counts processor cycles; the other three take one group's events per frame
//     (instructions completed and L1 data misses, miss stall cycles, L2, branches, the mix of
//     instructions, TLB). Counted over the whole of the frame's period (the CPU's frame-to-frame
//     time: game, drawing, sound), in supervisor mode, with the waits the application brackets
//     (wl_hwcount_wait_begin/end: the GPU, the display) left out. Other threads and interrupt
//     handlers count too.
//   - GPU: GX's two virtual counters (libogc: GX_SetGPMetric, which queues the selection in the
//     FIFO) and the XF/RAS counters (GX_InitXfRasMetric), one pair of metrics per frame, plus the
//     vertex cache's three. libogc's GX_ReadGPMetric never stores through its second pointer
//     (confirmed by disassembling libogc.a: it overwrites r4 at its first load), so the counters
//     are read from the command processor's registers directly, 0xCC000040 and up, 32 bits each in
//     two 16-bit halves (the addresses libogc's own readers use; Dolphin's CommandProcessor names
//     them XF_RASBUSY, XF_CLKS, XF_WAIT_IN, XF_WAIT_OUT).
//   - Timeline: wl_hwcount_mark(id) sends a draw-sync token into the FIFO and stamps the time the
//     CPU gave it and the time the GPU passed it (the token interrupt), so the gap shows how far
//     the GPU lags the CPU at that point of the frame. The FIFO's fill (the GP's read/write
//     distance) is sampled at a few hundred Hz from a tick task, and at each mark.
//
// It is off unless a control file `hwcount.txt` is in the program's folder (the pattern of
// profile_arm.h): "name value" lines, '#' comments:
//
//   seconds   1     log a block every this many seconds
//   cpu       1     the CPU counters
//   gp        1     the GPU counters
//   vcache    1     the vertex cache's counters, with each GPU group
//   timeline  1     marks (draw-sync tokens) and the FIFO fill sampler
//   fifo_hz   480   the FIFO fill sampler's rate (0: only at the frame's begin, end and marks)
//   selftest  1     check that the counters count (below); 0 trusts them
//   threshold 0     MMCR0's THRESHOLD, for the "L1 miss cycles over threshold" event
//   first     60    frames to let go by before the first window
//   raw       0     also log each group's raw counters (to check a mapping against the console)
//
// Where the application calls it:
//   wl_hwcount_arm(folder);                // at start-up, once the file system is up
//   wl_hwcount_frame_begin();              // each frame, before its GX commands (wl_gx_begin_frame does)
//   wl_hwcount_wait_begin(WL_HWWAIT_GPU);  // around a wait for the GPU (wl_gx_sync) or the display
//   wl_hwcount_wait_end(WL_HWWAIT_GPU);    //   (WiiLibs' own waits are already bracketed)
//   wl_hwcount_mark(3);                    // anywhere in the frame; ids 0 to 15
//   wl_hwcount_frame_end();                // when the GPU has finished the frame (wl_gx_end_frame does)
//
// Self-test: Dolphin does not model the counters (they read zero, or whatever was last written).
// At the first windows the CPU counters are checked against the time base (cycles must be near
// 12 times the time base's ticks, and instructions must be counted), and the GP counters against
// vertices the application draws between wl_hwcount_selftest_begin and _end (or, without that, a
// frame that shows vertices counted in the groups that count them). A counter that fails is
// marked invalid, logged once as "no counters", and left out of the verdict and the log.
//
// The engine (groups, windows, medians, verdict, formatting) is portable and tested on every host
// against a fake machine (WlHwcountHw); the Wii's backend is wii/hwcount_wii.c.
#pragma once

#include "wl/base/base.h"

#define WL_HWCOUNT_FILE "hwcount.txt"

#define WL_HWCOUNT_CPU_GROUPS 6u
#define WL_HWCOUNT_GP_GROUPS 7u
#define WL_HWCOUNT_MARKS 16u
#define WL_HWCOUNT_HISTORY 16u // samples kept for a metric between two log blocks

// --- the metrics --------------------------------------------------------------------------------
// Everything the verdict and the log use is one of these. The unit is in the name: _PM is parts per
// thousand (of the processor's cycles, or of the GP's clocks), _PKI per thousand instructions,
// _PKC per thousand cycles, _PKT per thousand texels; the rest are counts per window.
typedef enum WlHwMetric {
    // CPU (from the group that carries it)
    WL_HWM_IPC_M = 0,       // instructions completed per cycle, times 1000
    WL_HWM_L1D_MISS_PKI,    // L1 data cache misses per 1000 instructions
    WL_HWM_BR_MISP_PKI,     // mispredicted branches per 1000 instructions
    WL_HWM_DC_STALL_PM,     // cycles an L1 data miss was outstanding beyond the threshold
    WL_HWM_DC_LOAD_PM,      // cycles of L1 data load miss
    WL_HWM_DTLB_SEARCH_PM,  // cycles searching the page table for the data TLB
    WL_HWM_ITLB_SEARCH_PM,  // the same for the instruction TLB
    WL_HWM_DTLB_MISS_PKC,
    WL_HWM_L2_HIT_PKC,
    WL_HWM_L2_MISS_PKC,     // data L2 misses (to memory) per 1000 cycles
    WL_HWM_L2_CASTOUT_PKC,
    WL_HWM_BR_STALL_PM,     // cycles the branch unit stalled on speculative branches
    WL_HWM_BR_LRCTR_PM,     // cycles the branch unit waited for LR or CTR
    WL_HWM_FP_PM,           // floating point instructions per 1000 dispatched
    WL_HWM_INT_PM,          // integer instructions per 1000 dispatched
    WL_HWM_SNOOP_PKC,       // snoop retries per 1000 cycles
    WL_HWM_CPU_KCYC,        // processor cycles in the window (thousands), waits excluded
    // GPU
    WL_HWM_RAS_BUSY_PM,     // the rasterizer busy (GP clocks)
    WL_HWM_XF_WAIT_IN_PM,   // the transform unit waiting for input: the GP is starved
    WL_HWM_XF_WAIT_OUT_PM,  // the transform unit waiting to hand on its output: the pixel side is behind
    WL_HWM_XF_XFRM_PM,      // the transform engine busy
    WL_HWM_XF_LIT_PM,       // the lighting engine busy
    WL_HWM_XF_BOT_PM,       // the bottom of the XF pipe busy
    WL_HWM_TX_MEMSTALL_PM,  // texture unit stalled on main memory
    WL_HWM_TX_IDLE_PM,
    WL_HWM_VC_STALL_PM,     // vertex cache stalls (all kinds), perf group
    WL_HWM_VCM_MISS_PM,     // the vertex cache's own counters: misses per 1000 checks
    WL_HWM_VCM_STALL_PM,    //   and stall clocks
    WL_HWM_VERTS,           // vertices the GP processed in the window
    WL_HWM_TRIS,
    WL_HWM_QUADS,           // 2x2 pixel quads with coverage
    WL_HWM_TEXELS,
    WL_HWM_TC_MISS,
    WL_HWM_TC_MISS_PKT,     // texture cache misses per 1000 texels
    WL_HWM_GP_MHZ_X10,      // the GP's clock as measured (XF/RAS clks over the window), MHz times 10
    WL_HWM_COUNT
} WlHwMetric;

const char *wl_hwcount_metric_name(WlHwMetric metric);

// --- the groups ---------------------------------------------------------------------------------
// CPU group: the event selected on each of PMC1 to PMC4 (the numbers are the 750's PMC event
// numbers; PMC1 always 1, processor cycles).
typedef struct WlHwcpuGroup {
    const char *name;
    uint8_t sel[4];
} WlHwcpuGroup;

typedef enum WlHwgpKind {
    WL_HWGP_PAIR = 0, // GX_SetGPMetric(perf0, perf1)
    WL_HWGP_XFRAS     // GX_InitXfRasMetric: RAS busy, clocks, XF wait in, XF wait out
} WlHwgpKind;

// GX's performance metrics by libogc's numbering (gx.h GX_PERF0_*, GX_PERF1_*).
typedef struct WlHwgpGroup {
    const char *name;
    uint8_t kind;
    uint8_t perf0;
    uint8_t perf1;
} WlHwgpGroup;

const WlHwcpuGroup *wl_hwcount_cpu_group(uint32_t index); // NULL past the end
const WlHwgpGroup *wl_hwcount_gp_group(uint32_t index);

// The registers' values for a CPU group (MMCR0 counting in supervisor and user mode, no interrupt;
// MMCR1 for PMC3 and PMC4). Bit numbers are IBM's (bit 0 the most significant).
//   MMCR0: DIS 0, THRESHOLD 10 to 15, PMC1SELECT 19 to 25, PMC2SELECT 26 to 31
//   MMCR1: PMC3SELECT 0 to 4, PMC4SELECT 5 to 9
#define WL_HWCOUNT_MMCR0_DIS 0x80000000u // freezes all the counters
uint32_t wl_hwcount_mmcr0(const uint8_t sel[4], uint32_t threshold);
uint32_t wl_hwcount_mmcr1(const uint8_t sel[4]);

// The counters count modulo 2^32: the difference of two reads, whichever wrapped.
static inline uint32_t wl_hwcount_delta(uint32_t before, uint32_t after) {
    return after - before;
}

// --- settings -----------------------------------------------------------------------------------
typedef struct WlHwcountSettings {
    uint32_t seconds;
    uint32_t cpu;
    uint32_t gp;
    uint32_t vcache;
    uint32_t timeline;
    uint32_t fifo_hz;
    uint32_t selftest;
    uint32_t threshold;
    uint32_t first;
    uint32_t raw;
} WlHwcountSettings;

typedef void (*WlHwcountIssueFn)(void *user, int issue, const char *key, uint32_t line); // 1 unknown name, 2 bad value

void wl_hwcount_settings_default(WlHwcountSettings *settings);
void wl_hwcount_settings_clamp(WlHwcountSettings *settings);
// Reads "name value" lines over `settings` (defaults first), then clamps. Returns the lines reported.
uint32_t wl_hwcount_settings_parse(WlHwcountSettings *settings, const char *text, size_t length,
                                   WlHwcountIssueFn issue, void *user);
// The same on a file: WL_ERR_NOT_FOUND when it is not there (the usual case: not armed).
WlResult wl_hwcount_settings_load(WlHwcountSettings *settings, const char *path, WlHwcountIssueFn issue, void *user);

// --- the verdict --------------------------------------------------------------------------------
typedef enum WlHwVerdictKind {
    WL_HWV_UNKNOWN = 0,   // too little measured
    WL_HWV_FITS,          // the frame is done early and waits for the display
    WL_HWV_CPU,           // the GP is starved: the processor is the bottleneck
    WL_HWV_GPU_XFORM,     // vertex transform / lighting
    WL_HWV_GPU_FILL,      // fill and TEV (the rasterizer; the XF backs up behind it)
    WL_HWV_GPU_TEXTURE,   // texture fetch (cache misses, main memory)
    WL_HWV_GPU_VERTEX,    // vertex fetch (vertex cache stalls)
    WL_HWV_GPU_MIXED      // the GPU is busy but no one part stands out
} WlHwVerdictKind;

typedef enum WlHwCpuKind {
    WL_HWC_NONE = 0,
    WL_HWC_MEMORY,        // stalls on data memory (L1 and L2 misses)
    WL_HWC_BRANCH,        // branch stalls or mispredicts
    WL_HWC_STALLED,       // a low IPC without a memory or branch cause: the pipeline's latencies (FPU, load-use)
    WL_HWC_COMPUTE        // instructions flow: the code simply has too many
} WlHwCpuKind;

// What the verdict reads: the medians of a rotation (one value per metric where it was measured),
// and the frame's own figures.
typedef struct WlHwcountView {
    uint32_t m[WL_HWM_COUNT];
    bool have[WL_HWM_COUNT];
    uint32_t frame_us;        // the frame's period (mean)
    uint32_t gpu_wait_us;     // the processor waiting for the GPU (mean per frame)
    uint32_t display_wait_us; // the processor waiting for the display (a retrace, a free XFB)
    uint32_t fifo_mean_pm;    // the FIFO's mean fill, parts per thousand of its size (UINT32_MAX: not measured)
    uint32_t fifo_max_pm;
} WlHwcountView;

// Starting values, from reading the counters' definitions and not from a console: the first
// hardware run is what tunes them.
typedef struct WlHwcountThresholds {
    uint32_t starved_wait_in_pm;   // XF waiting for input at least this share of the clocks: starved
    uint32_t fifo_low_pm;          // FIFO mean fill at most this: starved
    uint32_t cpu_wait_gpu_pm;      // processor waiting for the GPU at most this share of the frame: not GPU-bound
    uint32_t fits_display_wait_pm; // processor waiting for the display at least this much, at the target time: fits
    uint32_t target_frame_us;      // 33333
    uint32_t xform_pm;             // transform + lighting busy
    uint32_t fill_ras_pm;          // rasterizer busy
    uint32_t fill_wait_out_pm;     // XF waiting on the pixel side
    uint32_t texture_stall_pm;     // texture unit stalled on memory
    uint32_t texture_miss_pkt;     // misses per 1000 texels (with the rasterizer busy at least 300)
    uint32_t vertex_stall_pm;      // vertex cache stalls
    uint32_t cpu_memory_pm;        // L1 data miss stall cycles
    uint32_t cpu_branch_pm;        // branch stall cycles
    uint32_t cpu_mispredict_pki;
    uint32_t cpu_low_ipc_m;        // below this IPC (times 1000) with no named cause: stalled
} WlHwcountThresholds;

void wl_hwcount_thresholds_default(WlHwcountThresholds *t);

typedef struct WlHwcountVerdict {
    WlHwVerdictKind kind;
    WlHwCpuKind cpu;      // for WL_HWV_CPU (and WL_HWV_FITS, when the processor is measured)
    char text[256];       // one line: "CPU-bound (gp starved: xf wait-in 71%, fifo 3%); cpu: memory stalls (dcache 31%)"
} WlHwcountVerdict;

// A pure function of the view. `t` NULL: the defaults.
void wl_hwcount_verdict(const WlHwcountView *view, const WlHwcountThresholds *t, WlHwcountVerdict *out);
const char *wl_hwcount_verdict_name(WlHwVerdictKind kind);
const char *wl_hwcount_cpu_kind_name(WlHwCpuKind kind);

// The median of `count` values (the lower middle of an even count); 0 for none. Does not change `values`.
uint32_t wl_hwcount_median(const uint32_t *values, uint32_t count);

// --- the machine --------------------------------------------------------------------------------
// What the engine needs of the hardware; the Wii's is wl_hwcount_platform_hw(), tests give a fake.
typedef void (*WlHwcountTokenFn)(void *arg, uint32_t token);
typedef void (*WlHwcountTickFn)(void *arg);

typedef struct WlHwcountHw {
    uint64_t tb_hz;                                   // ticks a second of `ticks`
    uint32_t cpu_clock_per_tick;                      // processor cycles a tick (12 on the Wii)
    uint32_t gp_clock_per_tick;                       // GP clocks a tick (4 on the Wii: the bus clock is 4 times the time base)
    uint64_t (*ticks)(void *user);
    // Processor counters; NULL members: not available.
    void (*cpu_start)(void *user, const uint8_t sel[4], uint32_t threshold); // stops, zeroes, selects, starts
    void (*cpu_read)(void *user, uint32_t pmc[4]);
    void (*cpu_stop)(void *user);
    // GPU counters. gp_start selects a group and clears the counters (the first frame of a rotation
    // is a window's start); gp_read returns the four counter slots at 0xCC000040/44/48/4C and the
    // vertex cache's three.
    void (*gp_start)(void *user, const WlHwgpGroup *group, bool vcache);
    void (*gp_read)(void *user, uint32_t slot[4], uint32_t vcache[3]);
    void (*gp_stop)(void *user);
    uint32_t (*fifo_fill)(void *user);                // bytes the GP has yet to read; UINT32_MAX: unknown
    // Timeline. mark_token sends a draw-sync token and returns it; set_token_hook has the token
    // interrupt call `fn` (NULL removes it). fifo_sampler runs `fn` hz times a second (hz 0 stops).
    uint32_t (*mark_token)(void *user);
    void (*set_token_hook)(void *user, WlHwcountTokenFn fn, void *arg);
    void (*fifo_sampler)(void *user, uint32_t hz, WlHwcountTickFn fn, void *arg);
    void (*log)(void *user, const char *line);
    void *user;
} WlHwcountHw;

typedef enum WlHwWait {
    WL_HWWAIT_GPU = 0,
    WL_HWWAIT_DISPLAY,
    WL_HWWAIT_COUNT
} WlHwWait;

typedef struct WlHwWindow { // one frame's period
    uint32_t cpu_group;
    uint32_t gp_group;
    uint64_t ticks;            // begin to the next begin
    uint64_t wait_ticks[WL_HWWAIT_COUNT];
    uint64_t gpu_done_ticks;   // begin to wl_hwcount_frame_end (0: not seen)
    uint32_t pmc[4];           // the counters, waits excluded
    uint32_t slot[4];          // GP counters, raw
    uint32_t vcache[3];
    uint32_t fifo_n;           // FIFO samples in the window
    uint64_t fifo_sum;
    uint32_t fifo_max;
    uint32_t fifo_min;
} WlHwWindow;

typedef struct WlHwMark {
    volatile uint32_t pending; // set after the other fields; cleared by the token interrupt
    uint32_t token;
    uint64_t cpu_tick;
    uint64_t gpu_tick;
    uint32_t id;
    uint32_t fifo;
} WlHwMark;

typedef struct WlHwSamples {
    uint32_t v[WL_HWCOUNT_HISTORY];
    uint32_t n;
} WlHwSamples;

typedef struct WlHwcount {
    WlHwcountSettings s;
    WlHwcountHw hw;
    bool armed;
    bool cpu_ok;               // the processor counters work (probe and self-test)
    bool gp_ok;                // the GPU counters work
    bool gp_checked;           // the GPU self-test has decided
    bool running;
    bool selftest_open;
    uint32_t selftest_vertices;
    uint32_t gp_slot0;         // which of the four slots holds perf0's and perf1's counter
    uint32_t gp_slot1;
    // the window in progress
    bool open;
    uint64_t t_begin;
    uint32_t frames;           // windows begun
    uint32_t cpu_group;
    uint32_t gp_group;
    uint32_t pmc_acc[4];
    uint32_t pmc_mark[4];
    uint32_t wait_depth;
    uint64_t wait_t0;
    uint64_t wait_ticks[WL_HWWAIT_COUNT];
    uint64_t gpu_done_ticks;
    uint32_t fifo_n_seen;
    uint32_t fifo_n_prev;
    uint64_t fifo_sum_seen;
    uint64_t fifo_sum_prev;
    uint32_t fifo_cap;
    volatile uint32_t fifo_n;
    volatile uint64_t fifo_sum;
    volatile uint32_t fifo_max;
    volatile uint32_t fifo_min;
    uint32_t gp_mult_x100;     // GP clocks per tick, times 100: measured, else the backend's
    WlHwMark marks[WL_HWCOUNT_MARKS];
    uint32_t mark_count;
    WlHwMark last_marks[WL_HWCOUNT_MARKS]; // the previous window's
    uint32_t last_mark_count;
    const char *mark_names[WL_HWCOUNT_MARKS];
    // the log period's accumulation
    WlHwSamples samples[WL_HWM_COUNT];
    uint32_t log_frames;
    uint64_t log_ticks;
    uint64_t log_worst_ticks;
    uint64_t log_wait_ticks[WL_HWWAIT_COUNT];
    uint64_t log_gpu_done_ticks;
    uint32_t log_gpu_done_n;
    uint64_t log_fifo_sum;
    uint32_t log_fifo_n;
    uint32_t log_fifo_max;
    uint64_t mark_cpu_sum[WL_HWCOUNT_MARKS];
    uint64_t mark_gpu_sum[WL_HWCOUNT_MARKS];
    uint32_t mark_n[WL_HWCOUNT_MARKS];
    uint32_t mark_gpu_n[WL_HWCOUNT_MARKS];
    uint32_t log_blocks;
    uint64_t next_log;
    uint32_t raw_slot[WL_HWCOUNT_GP_GROUPS][4];
    uint32_t raw_pmc[WL_HWCOUNT_CPU_GROUPS][4];
    uint32_t raw_vc[3];
    uint32_t cpu_sane_n;       // windows that passed the cycles check
    uint32_t cpu_insane_n;
    uint32_t gp_windows;       // GPU windows closed, and how many read any non-zero counter
    uint32_t gp_nonzero;
    bool selftest_skip;        // the window in progress was disturbed by the self-test
    WlHwcountThresholds thresholds;
} WlHwcount;

// --- the engine, on a machine of the caller's -----------------------------------------------------
// `hw` is copied. Probes what the machine has (a member NULL or a counter that does not count
// leaves it out) and starts the fifo sampler and token hook if settings ask.
WlResult wl_hwcount_start(WlHwcount *hc, const WlHwcountSettings *settings, const WlHwcountHw *hw);
void wl_hwcount_end(WlHwcount *hc); // stops everything

void wl_hwcount_begin_frame(WlHwcount *hc);   // closes the window in progress (if any), opens the next
void wl_hwcount_end_frame(WlHwcount *hc);     // the GPU has finished the frame
void wl_hwcount_wait_start(WlHwcount *hc, WlHwWait kind);
void wl_hwcount_wait_stop(WlHwcount *hc, WlHwWait kind);
void wl_hwcount_mark_at(WlHwcount *hc, uint32_t id);
void wl_hwcount_token_passed(WlHwcount *hc, uint32_t token); // the token interrupt's, also callable by a test
void wl_hwcount_fifo_sample(WlHwcount *hc);                   // the sampler's tick
void wl_hwcount_selftest_start(WlHwcount *hc);
void wl_hwcount_selftest_stop(WlHwcount *hc, uint32_t vertices); // the GPU must be idle (the caller synced)

// The medians of what the log period has gathered, as the verdict's view; resets nothing.
void wl_hwcount_view(const WlHwcount *hc, WlHwcountView *view);
// The log block of the period gathered so far, line by line through `hw.log` (and reset the
// period). Called from frame_begin once `seconds` have passed; also callable at any time.
uint32_t wl_hwcount_report(WlHwcount *hc);

// --- the application's face (one instance, the platform's machine) -----------------------------------
// Reads `<folder>/hwcount.txt`; WL_ERR_NOT_FOUND when it is not there (nothing is started, and the
// functions below are a test of one flag).
WlResult wl_hwcount_arm(const char *folder);
bool wl_hwcount_armed(void);
void wl_hwcount_frame_begin(void);
void wl_hwcount_frame_end(void);
void wl_hwcount_wait_begin(WlHwWait kind);
void wl_hwcount_wait_end(WlHwWait kind);
void wl_hwcount_mark(uint32_t id);
void wl_hwcount_mark_name(uint32_t id, const char *name); // a static string
void wl_hwcount_selftest_begin(void);
// The GPU is synced (wl_gx_sync) and idle: the vertices drawn since _begin were `vertices`.
void wl_hwcount_selftest_end(uint32_t vertices);
void wl_hwcount_set_fifo_size(uint32_t bytes); // the FIFO's capacity, for the fill's share (wl_gx_init does)
void wl_hwcount_stop(void);

// The platform's machine: the Wii's performance monitor and GX; on the hosts one with the clock
// and nothing else.
const WlHwcountHw *wl_hwcount_platform_hw(void);
