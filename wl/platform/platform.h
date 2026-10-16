// SPDX-License-Identifier: MIT
// Platform layer: the one boundary between application code and libogc.
//
// wl/platform/wii implements it on the console (the only directory that includes <ogc/*.h>);
// wl/platform/host implements it as a dependency-free stub so the same application code builds
// and runs in host tests. Everything above this header (rendering, audio, input mapping, UI,
// scenes, networking) is written against it.
//
// Areas: system events and exit, memory regions and the data cache, video and frame pacing, the
// GX bootstrap with a state shadow, threads, files (SD/USB), raw input, audio output and logging.
// See wl/platform/README.md for the hardware facts behind the rules stated here.
#pragma once

#include "wl/base/base.h"
#include "wl/platform/present.h"
#include "wl/platform/video_mode.h"

// ---------------------------------------------------------------------------------------------
// System events, exit and time
// ---------------------------------------------------------------------------------------------

typedef enum {
    WL_SYS_NONE = 0,
    WL_SYS_RESET = 1, // console reset button
    WL_SYS_POWER = 2  // console power button or Wii Remote power button
} WlSystemEvent;

// Registers the reset and power button callbacks (console). Idempotent; the host stub does
// nothing. Without it wl_platform_system_events never reports anything.
void wl_platform_system_init(void);

// Bit set of WlSystemEvent raised since the last call (the callbacks latch them).
uint32_t wl_platform_system_events(void);

typedef enum {
    WL_EXIT_LOADER = 0, // return to the Homebrew Channel (or whatever loaded the program)
    WL_EXIT_WII_MENU,
    WL_EXIT_POWER_OFF
} WlExitTarget;

// Leaves the program. Before anything is closed it winds the program down, once, in this order:
// the shutdown hooks (below), newest first; the audio output stopped (wl_platform_audio_stop);
// every motor stopped (wl_platform_input_stop_rumble). Then the log is flushed and closed, and
// the SD card and USB drive are unmounted (their caches written out). A second caller, from
// another thread or from a hook, waits for the first to finish leaving. Host stub: the same
// wind-down, then exit(0).
WL_NORETURN void wl_platform_exit(WlExitTarget target);

// A function wl_platform_exit calls first, with the target it was called with: the place to let
// a save finish, close files the program holds open for writing, or tell a peer goodbye. It runs
// on whichever thread leaves (the system watcher's, for the reset and power buttons), while the
// program's other threads still run, so it must not wait for the thread that is waiting to
// leave, and should not wait long (the console powers off only when the program does). Up to
// WL_SHUTDOWN_HOOKS; they run newest first. WL_ERR_ARG for NULL, WL_ERR_LIMIT past the last.
typedef void (*WlShutdownFn)(WlExitTarget target, void *user);
#define WL_SHUTDOWN_HOOKS 8u
WlResult wl_platform_shutdown_hook(WlShutdownFn fn, void *user);

// Starts the thread that serves the reset and power buttons (wl_platform_system_init is called
// if it has not been) and so works while the program is loading or stalled: every `period_ms`
// (1..1000) it collects wl_platform_system_events and leaves, the power button (the console's
// or a Wii Remote's) with WL_EXIT_POWER_OFF, the reset button with WL_EXIT_LOADER. Otherwise it
// calls `tick(tick_user)` (NULL: nothing) on its own thread, a place for work that must go on
// in a stall, such as the motors' on and off. The thread's stack is 16 KiB from WL_TAG_STACK, so
// wl_mem_init must have run. While it runs the program must not call wl_platform_system_events
// itself (the events are consumed by whoever asks first). WL_ERR_STATE if already started or
// without memory, WL_ERR_ARG for a bad period, WL_ERR_NOMEM for no stack.
typedef void (*WlSystemTickFn)(void *user);
WlResult wl_platform_system_watch_start(uint32_t period_ms, WlSystemTickFn tick, void *tick_user);
// Pretends the buttons of `events` (WlSystemEvent bits) were pressed, for a script that wants to
// see the program's reaction without leaving: the watcher thread logs "reset button (simulated)" or
// "power button (simulated)" at its next look, and leaves nothing, whichever it is; real presses
// are served as ever. WL_ERR_STATE when the watcher is not running (nothing would see it).
WlResult wl_platform_system_simulate(uint32_t events);
// Stops the thread and waits for it (for tests and programs that handle the buttons themselves
// again). Does nothing when it is not running; not to be called from the thread's own tick.
void wl_platform_system_watch_stop(void);

// Monotonic microseconds (time base ticks on the console, CLOCK_MONOTONIC on the host); the
// epoch is unspecified, only differences are meaningful. Works before any other call.
uint64_t wl_platform_time_us(void);
// The same clock in milliseconds (rounded down), for a caller that wants only those: on the
// console one read of the time base and no 64-bit division (timebase.h).
uint64_t wl_platform_time_ms(void);
// The wall clock: microseconds since 1970-01-01 00:00:00. The console's clock is set to local
// time and knows no time zone, so there this is the local time counted as if it were UTC; the
// host's is UTC. Read once and then advanced by the monotonic clock, so it never steps back while
// the program runs.
uint64_t wl_platform_wall_time_us(void);

// ---------------------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------------------

typedef enum { WL_MEM1 = 0, WL_MEM2 } WlMemRegion;

// The free range of a region after the executable and system reservations, for the memory
// system (mem.h) to carve into budgeted heaps. Host stub: one malloc'd block per region.
WlResult wl_platform_mem_region(WlMemRegion region, void **out_base, size_t *out_size);

// Limits the claim wl_platform_mem_region makes of a region to `max_size` bytes from the top of
// its free arena; the part below stays libogc's, for malloc (newlib's sbrk grows up from MEM1's
// arena lo and then spills into MEM2's). MEM1 still leaves its system minimum below the claim.
// Call it before the region is claimed (wl_mem_init does, from WlMemConfig.claim). WL_ERR_ARG
// for size 0 or a bad region; WL_ERR_STATE once the region is claimed. Host stub: the region's
// block is at most `max_size` bytes.
WlResult wl_platform_mem_region_limit(WlMemRegion region, size_t max_size);

// Reserves [address, address + size) of a region for the caller, at exactly that address: for
// data built for fixed addresses (images holding absolute pointers, pre-relocated assets). Call
// it before the region is claimed (before wl_mem_init, or any wl_platform_mem_region of it).
// MEM2 claims come from the bottom of its free arena, in ascending order: everything below a
// claim's end leaves the region. MEM1 claims come from the top, in descending order (libogc's
// malloc grows up from the bottom): everything from a claim's start up leaves the region. A gap
// between the claim and the arena's end (or the previous claim) is lost, and logged; wl_mem's
// heaps get what is left. `address` and `size` must be 32-byte aligned. WL_ERR_ARG for bad
// alignment or size 0; WL_ERR_STATE once the region is claimed; WL_ERR_RANGE when the range is
// not inside the free arena or is out of order with an earlier claim. Host stub: maps the range
// at exactly `address` (mmap), WL_ERR_NOMEM when that address cannot be had, and shrinks the
// host region by the bytes claimed.
WlResult wl_platform_mem_claim_fixed(WlMemRegion region, void *address, size_t size);

// Flushes [ptr, ptr + size) from the data cache so the GPU or DMA sees it. `ptr` and `size`
// must be 32-byte aligned (WL_ARENA_ALIGN_GPU); asserts otherwise. Every GPU/DMA buffer goes
// through this (Dolphin hides a missing flush, hardware does not).
void wl_gpu_buffer_commit(void *ptr, size_t size);

// Invalidates the data cache for memory the GPU or DMA wrote (EFB copies, DMA reads).
void wl_gpu_buffer_invalidate(void *ptr, size_t size);

// ---------------------------------------------------------------------------------------------
// Video and frame pacing
// ---------------------------------------------------------------------------------------------

typedef struct {
    uint16_t fb_width;   // 640
    uint16_t efb_height; // 480: the picture's rows (the EFB has 528; the rows below are scratch)
    uint16_t xfb_height; // 480, or 576 at 50 Hz (the display copy scales the 480 rows to it)
    uint16_t refresh_mhz; // 59940 (NTSC/EuRGB60) or 50000
    bool progressive;
    bool widescreen; // the console's 16:9 setting (the picture drawn is 640x480 whatever it says)
    bool pal;        // 50 Hz
    bool pillarbox;  // widescreen, and the picture is narrowed into picture_x.. of the framebuffer
                     // by wl_gx_end_frame, black around it (WL_WIDESCREEN_PILLARBOX); draw 4:3 as ever
    uint16_t picture_x, picture_width; // the picture's columns in the framebuffer (all of it unless pillarbox)
    int16_t offset_h; // the console's display position, as applied to the picture's place on the line
} WlVideoInfo;

// Picks the console's preferred mode (480p when the setting and a component cable allow, EuRGB60
// instead of 50 Hz when that setting is on; wl_video_choose, video_mode.h), sets the line the
// picture is scanned into (a 16:9 TV's 704 pixels, shifted by the console's display position),
// logs the choice once, allocates the XFBs (two unless set_buffers asked for three;
// MEM1, 32-byte aligned, about 600 KB each) and starts the retrace interrupt's display queue.
// Idempotent.
WlResult wl_platform_video_init(void);
// Before init: 2 XFBs (the default) or 3. With two, finishing a frame while the one before is still
// waiting for its retrace waits for that retrace (in wl_platform_video_back_buffer); with three it
// does not, at the cost of one more XFB. After init: WL_OK for the count in use, else WL_ERR_STATE.
WlResult wl_platform_video_set_buffers(uint32_t count);
// The fewest retraces each frame stays on the screen (1 = every retrace, the default; 2 = 30 Hz on
// a 60 Hz display, as a Direct3D presentation interval of two). A frame presented early waits in
// the queue (the interrupt holds it), never the CPU; one presented late is shown at the next
// retrace. Clamped to 1..15; any time.
void wl_platform_video_set_interval(uint32_t retraces);

// Before init: what a 4:3 picture does on a 16:9 console setting: WL_WIDESCREEN_PILLARBOX (the
// default; in the middle of the screen, circles round) or WL_WIDESCREEN_STRETCH (over all of it).
// After init: WL_ERR_STATE unless it is the mode in use.
WlResult wl_platform_video_set_widescreen(WlWidescreen mode);

// The most frames a second to show (any time): the display queue holds each frame for the retraces
// that come to it, as a ratio (video_mode.h's wl_video_rate_ratio): 30 is two retraces at 60 Hz,
// and alternately two and one at 50 Hz, which makes 30 frames a second of uneven length, not 25.
// 0, or at least the refresh rate: every retrace. Replaces wl_platform_video_set_interval.
void wl_platform_video_set_rate(uint32_t hz);

void wl_platform_video_info(WlVideoInfo *out);
// One line saying what init chose (wl_video_describe's); "" before init.
void wl_platform_video_describe(char *out, size_t size);

// What the console configuration (SYSCONF) asks for, derived independently of the mode that
// wl_platform_video_init picked, by wl_video_choose over the settings (for a self-test that
// compares the two).
void wl_platform_video_expected(WlVideoInfo *out);

// The XFB being drawn this frame (the target of the display copy), the same until the next
// present. Never one the video interface scans or is about to: when every XFB is on the screen or
// waiting to be, this waits for retraces until one is free (the only wait in presenting; after a
// quarter second of a GPU that does not finish, the oldest waiting frame is dropped). NULL before
// init.
void *wl_platform_video_back_buffer(void);

// The back buffer is complete: it is queued to be shown at the next retrace the interval allows,
// after the frames presented before it (none is skipped). Returns at once, without waiting for
// the retrace; the retraces since the previous present (0 when the CPU runs ahead of the display).
uint32_t wl_platform_video_present(void);
// The same for a back buffer the GPU is still writing: it is shown only once `done(done_arg)` is
// true, a test the retrace interrupt makes (wl_gx_end_frame passes its draw-sync token's).
uint32_t wl_platform_video_present_when(WlPresentDone done, uint32_t done_arg);
// Waits (for retraces) until every presented frame is on the screen; false after half a second.
bool wl_platform_video_wait_shown(void);

typedef struct {
    uint32_t presented;          // frames presented
    uint32_t shown;              // frames that reached the screen
    uint32_t late;               // frames shown more than `interval` retraces after the one before
    uint32_t reclaimed;          // frames dropped because the GPU never finished them
    uint32_t last_gap;           // retraces between the last two frames to reach the screen
    uint32_t interval, buffers;
    uint32_t retraces;
    uint32_t buffer_waits;       // back_buffer calls that had to wait for a free XFB
    uint64_t buffer_wait_us;     // ...and how long, in all
    uint64_t buffer_wait_us_max; // ...the longest
} WlVideoStats;
void wl_platform_video_stats(WlVideoStats *out);

// Frames presented since wl_platform_video_init.
uint32_t wl_platform_video_frame_count(void);
// Frames that have reached the screen (presented frames are shown in order, none skipped).
uint32_t wl_platform_video_shown_count(void);

// Raw vertical retraces since init (a field in interlaced modes), counted in the retrace
// callback; the pacing reference. Presented frames never exceed it.
uint32_t wl_platform_video_retrace_count(void);

// A function called at every retrace (the console: in the retrace interrupt, after the display
// queue moved on, so it must be short and may only wake threads: wl_platform_sem_post). One hook;
// NULL removes it. Any time, before or after init. Host stub: never called (it has no display;
// a program that needs a beat there makes its own).
typedef void (*WlRetraceHook)(void *user);
void wl_platform_video_set_retrace_hook(WlRetraceHook fn, void *user);

// Frames shown later than the interval asked (the retrace after the previous one was missed, or
// nothing new had been presented), since init.
uint32_t wl_platform_video_dropped_count(void);

// ---------------------------------------------------------------------------------------------
// GX bootstrap and state shadow.
// Implemented by wl/platform/wii/gx_wii.c, the only code that touches libogc's GX. Setters keep
// a shadow of what the GPU was last told and drop redundant writes (wl_gx_stats counts both).
// Anything that bypasses this layer must call wl_gx_invalidate_shadow afterwards. Immediate
// vertices have one layout, WlGxVertex: position and colour always, texture coordinates used by
// the textured recipes.
// ---------------------------------------------------------------------------------------------

// 1 MiB budget minus the 32-byte heap header: the largest block a 1 MiB WL_TAG_GXFIFO heap holds,
// the FIFO's size with the default budget. A program that gives WL_TAG_GXFIFO a smaller budget
// gets a smaller FIFO (wl_gx_fifo_bytes): libogc's examples use 256 KiB, in MEM1, where the GP
// reads it (a FIFO in MEM2 is not supported on the console).
#define WL_GX_FIFO_BYTES (1024u * 1024u - 32u)
// The least GX_Init takes.
#define WL_GX_FIFO_MIN_BYTES (64u * 1024u)

// The size wl_gx_init gives the FIFO: the largest block WL_TAG_GXFIFO's heap holds (its budget
// less the 32-byte header, a multiple of 32), at most WL_GX_FIFO_BYTES; 0 if the memory system is
// not up, the tag is disabled or its budget is below WL_GX_FIFO_MIN_BYTES.
uint32_t wl_gx_fifo_bytes(void);

// Needs wl_mem_init (the FIFO comes from WL_TAG_GXFIFO, wl_gx_fifo_bytes long) and
// wl_platform_video_init. Brings up GX with the FIFO, the video mode's EFB/XFB copy setup and the
// default state. The log says the FIFO's address and size, and each new peak fill (WlGxStats.
// fifo_peak) by 64 KiB steps. WL_ERR_STATE if already up, WL_ERR_NOMEM if the FIFO cannot be
// allocated.
WlResult wl_gx_init(void);
// Drains the GPU and releases the FIFO. The next wl_gx_init starts from scratch.
void wl_gx_shutdown(void);
bool wl_gx_ready(void);

// Debug validation (bad enums, out-of-range indices, unaligned buffers, draws with the wrong
// recipe state, FIFO high-water hits, GPU wait timeouts): invalid calls are logged, counted in
// WlGxStats.validation_errors and dropped. On by default in builds without NDEBUG.
void wl_gx_set_validation(bool on);
// The EFB with a destination alpha (RGBA6_Z24: 6 bits a channel) or without (RGB8_Z24, the
// default), for blending by the destination's alpha. Ignored in anti-aliased modes (RGB565_Z16).
void wl_gx_set_efb_alpha(bool on);

typedef struct {
    float x, y, z;
    uint32_t rgba; // 0xRRGGBBAA
    float u, v;
} WlGxVertex;      // 24 bytes: stride of vertex arrays

typedef enum { WL_GX_TRIANGLES = 0, WL_GX_TRIANGLE_STRIP, WL_GX_QUADS, WL_GX_LINES, WL_GX_POINTS } WlGxPrim;
// GX's front face is the one whose vertices run CLOCKWISE seen from outside (right-handed, y up),
// the opposite of OpenGL: with CULL_BACK a counter-clockwise triangle disappears.
typedef enum { WL_GX_CULL_NONE = 0, WL_GX_CULL_FRONT, WL_GX_CULL_BACK, WL_GX_CULL_ALL } WlGxCull;
// ALPHA: src*a + dst*(1-a). ADD: src*a + dst. MULTIPLY: src*dst (shadows, tints). PREMULT: src + dst*(1-a).
typedef enum {
    WL_GX_BLEND_OFF = 0, WL_GX_BLEND_ALPHA, WL_GX_BLEND_ADD, WL_GX_BLEND_MULTIPLY, WL_GX_BLEND_PREMULT,
    WL_GX_BLEND_COUNT
} WlGxBlend;
// Same order as the hardware compare functions.
typedef enum {
    WL_GX_NEVER = 0, WL_GX_LESS, WL_GX_EQUAL, WL_GX_LEQUAL, WL_GX_GREATER, WL_GX_NEQUAL,
    WL_GX_GEQUAL, WL_GX_ALWAYS
} WlGxCompare;
// TEV recipes: colour source and texture use. "ras" is the rasteriser colour: the vertex colour
// (or the material colour when the layout has none), lit for TEX_LIT. `konst` is wl_gx_set_konst.
// The recipes are TEV programs (wl_tev_recipe in tev.h). Programs beyond these are built with
// tev.h (or directly on GX) and applied with wl_tev_apply, followed by wl_gx_invalidate_shadow.
typedef enum {
    WL_GX_RECIPE_VERTEX_COLOR = 0, // ras
    WL_GX_RECIPE_TEX_REPLACE,      // tex (texture 0 only; no colour channel)
    WL_GX_RECIPE_TEX_MODULATE,     // unlit textured: tex * ras (colour and alpha)
    WL_GX_RECIPE_TEX_LIT,          // vertex-lit textured: tex * (ambient + sum of directional lights) * vertex colour
    WL_GX_RECIPE_TEX_TINT,         // tex * ras * konst (colour and alpha), e.g. a time-of-day tint
    WL_GX_RECIPE_ADDITIVE,         // effects: rgb = tex.rgb * ras.rgb * ras.a, alpha 1 (texture alpha ignored, vertex alpha fades)
    WL_GX_RECIPE_UI,               // tex * ras, alpha also times konst.a (widget opacity)
    WL_GX_RECIPE_TEXT,             // rgb = ras.rgb, alpha = tex.a * ras.a (I4/IA4/IA8 glyph atlases)
    WL_GX_RECIPE_KONST,            // rgb and alpha = konst (flat silhouettes, outline hulls; no texture, no vertex colour)
    WL_GX_RECIPE_COUNT
} WlGxRecipe;
typedef enum {
    WL_GX_TEX_I4 = 0, WL_GX_TEX_I8, WL_GX_TEX_IA4, WL_GX_TEX_IA8, WL_GX_TEX_RGB565,
    WL_GX_TEX_RGB5A3, WL_GX_TEX_RGBA8, WL_GX_TEX_CMPR,
    WL_GX_TEX_C4, WL_GX_TEX_C8, // colour-indexed: need a TLUT (wl_gx_tlut_*)
    WL_GX_TEX_FORMAT_COUNT
} WlGxTexFormat;

typedef enum { WL_GX_WRAP_CLAMP = 0, WL_GX_WRAP_REPEAT, WL_GX_WRAP_MIRROR } WlGxWrap;
// Same order as the hardware filters. The *_MIP_* values need a texture with mip levels.
typedef enum {
    WL_GX_FILTER_NEAREST = 0, WL_GX_FILTER_LINEAR, WL_GX_FILTER_NEAREST_MIP_NEAREST,
    WL_GX_FILTER_LINEAR_MIP_NEAREST, WL_GX_FILTER_NEAREST_MIP_LINEAR, WL_GX_FILTER_LINEAR_MIP_LINEAR,
    WL_GX_FILTER_COUNT
} WlGxFilter;
// Per-material sampler state. `pinned` exempts it from the global override.
typedef struct {
    uint8_t wrap_s, wrap_t; // WlGxWrap; repeat and mirror need power-of-two dimensions
    uint8_t min_filter;     // WlGxFilter
    uint8_t mag_filter;     // WL_GX_FILTER_NEAREST or WL_GX_FILTER_LINEAR
    bool pinned;            // crisp pixel art (sprites, billboards, text): ignores the override
    float lod_bias;         // mip level bias (negative = sharper); 0 for none
} WlGxSampler;
// Global texture filtering choice (e.g. a "sharp pixels / smooth" user setting).
// NONE: each sampler decides. NEAREST / BILINEAR replace the min and mag filters of every
// sampler that is not pinned (a mip filter keeps its mip blend: nearest -> NEAREST_MIP_NEAREST or
// NEAREST_MIP_LINEAR, bilinear -> LINEAR_MIP_NEAREST or LINEAR_MIP_LINEAR; textures without mips
// get no mip filter).
typedef enum {
    WL_GX_FILTER_OVERRIDE_NONE = 0, WL_GX_FILTER_OVERRIDE_NEAREST, WL_GX_FILTER_OVERRIDE_BILINEAR
} WlGxFilterOverride;
void wl_gx_set_filter_override(WlGxFilterOverride mode);
WlGxFilterOverride wl_gx_get_filter_override(void);
// The filters a bind would use: `s` with the override applied and mip filters dropped when the
// texture has no mip levels (pure function, shared by the backends and host tests).
void wl_gx_resolve_filters(const WlGxSampler *s, uint32_t mips, WlGxFilterOverride override_mode, uint8_t *min_out,
                           uint8_t *mag_out);

// A prepared texture binding. Opaque to callers except for the description fields; initialise
// with wl_gx_tex_init / wl_gx_tex_init_ex.
typedef struct {
    uint32_t opaque[8]; // the GX texture object
    WlGxSampler sampler;
    uint16_t width, height;
    uint8_t format; // WlGxTexFormat
    uint8_t mips;   // levels, 1 = none
    uint8_t tlut;   // TLUT slot for C4/C8 (0..15)
} WlGxTexObj;

typedef struct {
    const void *image;     // 32-byte aligned, tile-ordered, mip levels one after the other, committed
    uint16_t width, height; // 1..1024
    uint8_t format;        // WlGxTexFormat
    uint8_t mips;          // levels (0 or 1 = none); the dimensions must be powers of two when > 1
    uint8_t tlut;          // TLUT slot 0..15 the C4/C8 texture reads
    WlGxSampler sampler;
} WlGxTexDesc;

// Bytes of GX tile-ordered image data for the format (dimensions rounded up to the tile size).
size_t wl_gx_tex_bytes(WlGxTexFormat fmt, uint32_t width, uint32_t height);
// Bytes of a full mip chain (each level halves, minimum 1 texel, rounded up to the tile size).
size_t wl_gx_tex_bytes_mips(WlGxTexFormat fmt, uint32_t width, uint32_t height, uint32_t mips);
// `image` is 32-byte aligned, tile-ordered, and was committed with wl_gpu_buffer_commit (the
// GPU reads main memory, not the CPU cache). Sizes 1..1024. Single level; sampler wrap
// repeat/clamp, filter linear/nearest. The shorthand of wl_gx_tex_init_ex.
WlResult wl_gx_tex_init(WlGxTexObj *tex, const void *image, uint32_t width, uint32_t height,
                        WlGxTexFormat fmt, bool repeat, bool linear);
WlResult wl_gx_tex_init_ex(WlGxTexObj *tex, const WlGxTexDesc *desc);

// TLUT (palette) for C4/C8 textures: 16-bit entries in `format`, in a 32-byte aligned committed buffer.
typedef enum { WL_GX_TLUT_IA8 = 0, WL_GX_TLUT_RGB565, WL_GX_TLUT_RGB5A3 } WlGxTlutFormat;
typedef struct {
    uint32_t opaque[3]; // the GX TLUT object
    const void *entries;
    uint16_t count;
    uint8_t format;
} WlGxTlut;
WlResult wl_gx_tlut_init(WlGxTlut *tlut, const void *entries, WlGxTlutFormat fmt, uint16_t count);
// Loads `tlut` into TLUT slot `slot` (0..15, 256 entries each) unless it is already there. A
// palette swap is a load of another WlGxTlut; rewriting the entries in place needs a commit and
// wl_gx_tlut_invalidate.
void wl_gx_tlut_load(const WlGxTlut *tlut, uint32_t slot);
void wl_gx_tlut_invalidate(void);

// Binds to texture map 0. Replacing the contents of memory a bound texture points at needs
// wl_gx_tex_invalidate (once, not per draw). `sampler` NULL uses the one stored in the texture;
// the global filter override applies unless the sampler is pinned. C4/C8 textures need `tlut`
// (loaded into the texture's slot); other formats ignore it. wl_gx_tex_bind(t) = bind_ex(t, NULL, NULL).
void wl_gx_tex_bind_ex(const WlGxTexObj *tex, const WlGxSampler *sampler, const WlGxTlut *tlut);
void wl_gx_tex_bind(const WlGxTexObj *tex);
void wl_gx_tex_invalidate(void);

// The ramp: an I8 texture of 256 by 4 texels whose value is its x coordinate (every row the same),
// bound to a texture map of the caller's choice with nearest filtering and clamping. A vertex
// carries a number from 0 to 1 as a texture coordinate s = wl_gx_ramp_coordinate(value) (the middle
// of texel round(value * 255)); the TEV reads it back as the texture's colour and alpha, so a value
// the hardware has no vertex output for (the fog factor of a vertex shader, a per-vertex weight)
// reaches the stages. For a texture coordinate generator that passes the coordinate unchanged.
#define WL_GX_RAMP_WIDTH 256u
#define WL_GX_RAMP_HEIGHT 4u
#define WL_GX_RAMP_BYTES (WL_GX_RAMP_WIDTH * WL_GX_RAMP_HEIGHT)
// Writes the ramp's image into `image` (WL_GX_RAMP_BYTES bytes, tile-ordered like any I8 texture).
void wl_gx_ramp_fill(void *image);
// The shared image: 32-byte aligned, committed for the GPU, built at the first call.
const void *wl_gx_ramp_image(void);
// Loads the ramp into texture map `texmap` (0..7). Bypasses the shadow's texture binding when the
// map is 0 (it is marked unknown). Needs wl_gx_init.
void wl_gx_ramp_bind(uint32_t texmap);
// The texture coordinate s that reads `value` (0 to 1; anything else, NaN included, is clamped to 0
// or 1) off the ramp: (value * 255 + 0.5) / 256.
static inline float wl_gx_ramp_coordinate(float value) {
    if (!(value >= 0.0f)) {
        value = 0.0f;
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    return (value * 255.0f + 0.5f) * (1.0f / 256.0f);
}

// Frame: begin sets the default state, viewport/scissor to the full EFB and clears to
// `clear_rgba` (free when it equals the colour the last end_frame cleared with); end copies the
// EFB to the back XFB (clearing for the next frame), sends a draw-sync token behind the copy and
// presents the XFB to be shown once the GPU has passed it (wl_platform_video_present_when). It
// never waits for the display: only for a free XFB (wl_platform_video_back_buffer) and, unless
// frame overlap is on, for the GPU to finish the frame (250 ms timeout). Returns the retraces
// since the last present like wl_platform_video_present.
void wl_gx_begin_frame(uint32_t clear_rgba);
uint32_t wl_gx_end_frame(void);
// (On a 16:9 console setting with WL_WIDESCREEN_PILLARBOX, wl_gx_end_frame narrows the finished picture,
// overlay included, into the middle three quarters of the framebuffer, black around it, before the display
// copy (wl_gx_target_squeeze): one EFB copy of 640x480 and one filtered quad a frame, and 1.2 MiB from the
// render targets' allocator. The program draws 4:3 as ever; WlVideoInfo.pillarbox says it is done.)
// A frame overlay: `fn(user, width, height)` runs inside wl_gx_end_frame, after the frame's last draw and
// before its display copy, so it draws over the finished image (a menu, a notice, a battery warning)
// without the program's renderer knowing: it costs the frame one pointer test while none is set, and
// nothing else. The back buffer has the EFB then (a render target that had it has been resolved), the
// wl_gx state is reset first (wl_gx_reset_state: the whole EFB, an orthographic projection over its
// pixels, y down, blending off), `width` and `height` are the EFB's size in pixels, and the state is
// reset again afterwards, so the overlay needs no clean-up. The drawing helpers of overlay.h are
// meant for it. The hook is read once per frame on the thread that ends frames; set it from that
// thread (a pointer store, but `user` must outlive it). NULL removes it. Host stub: called the same
// way, in end_frame.
typedef void (*WlGxOverlayFn)(void *user, uint32_t width, uint32_t height);
void wl_gx_set_overlay(WlGxOverlayFn fn, void *user);
// Frame overlap (off by default): end_frame returns without waiting for the GPU, so the CPU builds
// the next frame while the GPU finishes this one (the next frame's commands queue behind the
// copy); it waits only for the frame before this one, so the GPU is never more than one frame
// behind (a double-buffered frame arena stays safe). The caller then owns the hazard the wait removed: memory the GPU reads by address
// (vertex arrays, textures, TLUTs, display lists) must not be rewritten or freed until the frame
// that used it is retired (wl_gx_frame_retired / wl_gx_frame_wait), and whatever its CPU-side
// "is the GPU done with this" answers must follow the same.
void wl_gx_set_frame_overlap(bool on);
// The index of the frame being built (frames ended so far); a resource drawn now is free for the
// CPU again once this frame is retired.
uint32_t wl_gx_frame_index(void);
// Whether the GPU has finished frame `index` (its display copy passed). false for the frame being
// built; true for frames more than eight back.
bool wl_gx_frame_retired(uint32_t index);
// Waits until frame `index` is retired (for the frame being built: wl_gx_sync). false on timeout.
bool wl_gx_frame_wait(uint32_t index);
// Flush the FIFO and wait for the GPU to finish everything submitted. False on timeout.
bool wl_gx_sync(void);
// What the program is at, for the GPU hang report: slot 0..3 a name (a string that lives on) and a
// number (a frame, a draw), two stores. On the Wii, when the GPU has read no command for 10 s while
// it has work, the report logs these, the command processor's FIFO registers and the bytes at its
// read pointer, and the last GX events, syncs the log to the SD card and leaves to the loader 20 s
// later (wii/gx_hang_wii.c). Host: nothing.
void wl_gx_hang_where(unsigned slot, const char *name, uint32_t value);
// Restores the default state through the shadow (afterwards shadow == GPU).
void wl_gx_reset_state(void);
// Marks the shadow unknown: the next setter of each kind always writes.
void wl_gx_invalidate_shadow(void);

void wl_gx_set_viewport(int x, int y, int w, int h);
void wl_gx_set_scissor(int x, int y, int w, int h);
void wl_gx_set_ortho(float left, float top, float right, float bottom, float znear, float zfar);
void wl_gx_set_perspective(float fovy_degrees, float aspect, float znear, float zfar);
// 3x4 row-major position matrix; NULL = identity.
void wl_gx_set_modelview(const float m[12]);
// Loads a model-view into position/normal matrix slot `slot` (1..9; slot 0 is
// wl_gx_set_modelview's) for vertices of a layout with mtx_idx: a skinned or batched mesh picks
// its matrix per vertex. The normal matrix is the inverse transpose, computed here. Stays loaded
// until replaced.
void wl_gx_load_matrix_slot(uint32_t slot, const float m[12]);
void wl_gx_set_cull(WlGxCull cull);
void wl_gx_set_blend(WlGxBlend blend);
void wl_gx_set_zmode(bool test, WlGxCompare func, bool write);
// func ALWAYS disables the test.
void wl_gx_set_alpha_test(WlGxCompare func, uint8_t ref);
void wl_gx_set_color_write(bool rgb, bool alpha);
void wl_gx_set_recipe(WlGxRecipe recipe);
// Konst colour (0xRRGGBBAA) read by TEX_TINT (rgb and alpha), UI (alpha) and KONST. Default white.
void wl_gx_set_konst(uint32_t rgba);
// Colour used as "vertex colour" when the layout has none. Default white.
void wl_gx_set_material_color(uint32_t rgba);
// Texture matrix of texture coordinate 0: `m` is the 2x3 {a b tu, c d tv}, uv' = {a b; c d} * uv
// + t, applied to the coordinate every recipe reads for its first texture; NULL = identity (the
// default, and what every frame starts with). Costs one matrix load per change and one
// texture-coordinate generator write when it switches between a matrix and none. Not allowed
// while a display list records.
void wl_gx_set_tex_matrix(const float m[6]);

// Fog: applied after the TEV, so a translucent draw fades too. start/end are view-space distances
// (the projection's near/far come from the last set_perspective/set_ortho; fog is re-applied when
// the projection changes). EXP and EXP2 treat start..end as the range the curve covers.
typedef enum { WL_GX_FOG_OFF = 0, WL_GX_FOG_LINEAR, WL_GX_FOG_EXP, WL_GX_FOG_EXP2, WL_GX_FOG_COUNT } WlGxFog;
void wl_gx_set_fog(WlGxFog mode, float start, float end, uint32_t rgba);

// Directional lights for WL_GX_RECIPE_TEX_LIT: ambient plus up to WL_GX_MAX_LIGHTS lights, each
// `dir` the direction the light travels (unit, in VIEW space: transform world directions by the
// view matrix first) and a colour. Lit colour = clamp(ambient + sum of max(0, n.-dir) * colour),
// times the vertex colour. Needs normals in the layout. Default: ambient white, no lights (the
// vertex colour unchanged).
#define WL_GX_MAX_LIGHTS 3u
typedef struct {
    float dir[3];
    uint32_t rgba; // 0xRRGGBBAA; alpha ignored
} WlGxDirLight;
void wl_gx_set_lighting(uint32_t ambient_rgba, const WlGxDirLight *lights, uint32_t count);

// Vertex layouts. One hardware layout is active; indexed draws and display lists use the layout
// given here, wl_gx_draw always uses WL_GX_LAYOUT_STD direct vertices (WlGxVertex).
typedef enum {
    WL_GX_COMP_NONE = 0, WL_GX_COMP_F32, WL_GX_COMP_S16, WL_GX_COMP_U16, WL_GX_COMP_S8, WL_GX_COMP_U8
} WlGxComp;
typedef enum {
    WL_GX_COLOR_NONE = 0, WL_GX_COLOR_RGBA8, WL_GX_COLOR_RGB565, WL_GX_COLOR_RGBA4, WL_GX_COLOR_RGB8
} WlGxColorFmt;
typedef struct {
    uint8_t pos;       // WlGxComp: F32, S16 or S8 (xyz)
    uint8_t pos_shift; // fraction bits for S16/S8/U16/U8 positions and uvs
    uint8_t normal;    // WlGxComp: NONE, F32, S16 or S8 (xyz; S8 has 6 fraction bits, S16 14)
    uint8_t color;     // WlGxColorFmt
    uint8_t uv;        // WlGxComp: NONE, F32, S16, U16, S8 or U8 (st)
    uint8_t uv_shift;
    uint8_t indexed;   // 0: attribute data follows each vertex inline; 1: one 16-bit index per attribute
    uint8_t mtx_idx;   // 1: every vertex starts with a direct u8 position-matrix index (3 * slot), for matrix-batched meshes
} WlGxLayout;
// f32 xyz, RGBA8, f32 st; direct. What wl_gx_draw writes.
#define WL_GX_LAYOUT_STD {WL_GX_COMP_F32, 0, WL_GX_COMP_NONE, WL_GX_COLOR_RGBA8, WL_GX_COMP_F32, 0, 0, 0}
// The layout for wl_gx_draw_indexed and display lists (applied lazily at the next such draw).
WlResult wl_gx_set_layout(const WlGxLayout *layout);

// Immediate vertices (up to 65535 per call), written straight into the FIFO.
void wl_gx_draw(WlGxPrim prim, const WlGxVertex *verts, uint32_t count);
// Vertex array draws: `verts` (32-byte aligned, committed) is set as the array, then primitives
// index it with 16-bit indices.
void wl_gx_set_vertex_array(const WlGxVertex *verts, uint32_t count); // also selects the STD layout
// Separate arrays for any layout attribute that is indexed. `base` is 32-byte aligned and committed,
// `stride` bytes between elements (<= 255), `count` elements (index validation; 0 = unchecked).
typedef enum { WL_GX_ARRAY_POS = 0, WL_GX_ARRAY_NORMAL, WL_GX_ARRAY_COLOR, WL_GX_ARRAY_UV, WL_GX_ARRAY_COUNT } WlGxArray;
void wl_gx_set_array(WlGxArray which, const void *base, uint32_t stride, uint32_t count);
// Primitives from one 16-bit index per vertex, applied to every indexed attribute of the layout
// (after wl_gx_set_layout, or the STD layout made indexed when none was set).
void wl_gx_draw_indexed(WlGxPrim prim, const uint16_t *indices, uint32_t count);

// Display lists: a recorded run of draw commands (typically one indexed mesh) replayed with one
// call. Record between dl_begin and dl_end with wl_gx_draw_indexed only (state setters are
// rejected); `buf` is 32-byte aligned, `capacity` a multiple of 32 (the list pads itself to 32
// bytes). Lists built elsewhere (offline, by a converter) are wrapped with wl_gx_dl_init.
// dl_call applies the list's layout, then replays it: arrays, textures and matrices are set by
// the caller beforehand.
typedef struct {
    const void *data;
    uint32_t size;      // bytes, multiple of 32
    uint32_t vertices;  // for the statistics
    WlGxLayout layout;
} WlGxDisplayList;
WlResult wl_gx_dl_begin(WlGxDisplayList *dl, void *buf, uint32_t capacity, const WlGxLayout *layout);
WlResult wl_gx_dl_end(WlGxDisplayList *dl); // WL_ERR_RANGE when the buffer was too small
WlResult wl_gx_dl_init(WlGxDisplayList *dl, const void *data, uint32_t size, uint32_t vertices,
                       const WlGxLayout *layout);
void wl_gx_dl_call(const WlGxDisplayList *dl);

// Reads an EFB pixel as 0xRRGGBBAA (syncs with the GPU first). Valid between begin_frame and
// end_frame; for tests and debugging only (CPU EFB access is slow).
uint32_t wl_gx_peek_rgba(int x, int y);

// Copies the EFB rectangle (x, y, w, h: even numbers, EFB pixels, origin top left) into a
// texture image in `dst` (32-byte aligned, wl_gx_tex_bytes(fmt, w, h) bytes, tile-ordered like
// any texture of `fmt`: I4, I8, IA8, RGB565, RGB5A3 or RGBA8; the intensity formats take the red
// channel of the pixel as it is, 0 to 255 stays 0 to 255). With `clear` the rectangle is cleared
// to the frame's clear colour and the far depth (it needs colour and depth writes on, which this
// call sets). The GPU writes `dst` behind the CPU's back: the call invalidates the range first
// and invalidates the texture cache after, so a texture object over `dst` shows the new image
// from the next draw on; the CPU must not write `dst` afterwards. Render-to-texture for shadows,
// silhouettes and post effects.
void wl_gx_copy_efb_texture(int x, int y, int w, int h, void *dst, WlGxTexFormat fmt, bool clear);
// The same copy again into a `dst` that wl_gx_copy_efb_texture has filled (a copy of no fewer bytes) and the
// CPU has not touched since (the contract above: it must not write `dst` afterwards): the range is not
// invalidated again, for no line of it can be in the CPU's cache. A full-screen picture every frame leaves
// out 1.2 MiB of invalidation. Memory made anew, or written by the CPU in between, is wl_gx_copy_efb_texture's.
void wl_gx_copy_efb_texture_again(int x, int y, int w, int h, void *dst, WlGxTexFormat fmt, bool clear);

typedef struct {
    uint32_t frames;
    uint32_t state_calls;   // shadowed setter calls
    uint32_t state_writes;  // ...that reached the GPU
    uint32_t draw_calls;
    uint32_t vertices;
    uint32_t fifo_peak;     // largest FIFO fill (bytes) sampled at end_frame
    uint32_t fifo_hiwater;  // end_frame samples where the GP reported the FIFO above its high mark
    uint32_t gpu_timeouts;
    uint32_t validation_errors;
    uint32_t gpu_wait_us_max; // longest end_frame / sync wait
    uint32_t dl_calls;        // display lists replayed (also counted in draw_calls)
    uint32_t tex_loads;       // texture objects loaded into the GPU (after the shadow)
    uint32_t tlut_loads;      // TLUTs loaded
    uint32_t tex_mtx_loads;   // texture matrices loaded into the GPU (after the shadow)
    uint32_t gpu_waits;       // times the processor waited for the GPU (end_frame, wl_gx_sync, wl_gx_frame_wait)
    uint32_t efb_copies;      // texture copies out of the EFB (render targets, parks, the squeeze); the GPU's
                              // pixel pipe syncs after each, the processor does not wait
} WlGxStats;
void wl_gx_stats(WlGxStats *out);

// ---------------------------------------------------------------------------------------------
// Threads. A typical layout is a main thread plus io, audio and net helpers; the priorities
// below name those roles.
// ---------------------------------------------------------------------------------------------

// Handles are the native ids (libogc lwp_t / sem_t / mutex_t are 32-bit), kept by value so no
// allocation is needed.
typedef struct {
    uint32_t id;
} WlPlatformThread;
typedef struct {
    uint32_t id;
} WlPlatformSemaphore;
typedef struct {
    uint32_t id;
} WlPlatformMutex;

// LWP priorities on the console: IO 32, MAIN 64, NET 80, AUDIO 100 (higher runs first; 0 is the
// idle thread, 127 the highest). The first thread created also pins the calling (main) thread to
// the MAIN value so the table is self-consistent. A higher-priority thread preempts a lower one
// the moment it becomes ready; equal priorities round-robin; a lower one runs only while the
// higher ones are blocked.
typedef enum {
    WL_THREAD_PRIO_IO = 0, // below main
    WL_THREAD_PRIO_MAIN,
    WL_THREAD_PRIO_NET,  // above main
    WL_THREAD_PRIO_AUDIO // high
} WlThreadPriority;

// True when priorities are enforced (the console); the host stub uses pthreads and ignores them,
// so tests that depend on preemption order only run when this is true.
bool wl_platform_thread_priorities_real(void);

typedef void *(*WlPlatformThreadFn)(void *arg);

// `stack` must be 32-byte aligned memory owned by the caller (e.g. from WL_TAG_STACK).
WlResult wl_platform_thread_create(WlPlatformThread *out, WlPlatformThreadFn fn, void *arg,
                                   void *stack, size_t stack_size, WlThreadPriority priority);
// Blocks until the thread function returned (every created thread must be joined once).
WlResult wl_platform_thread_join(WlPlatformThread thread, void **out_result);
// Lets an equal-priority runnable thread run; does nothing for lower-priority ones (a spin loop
// that yields starves them: block on a semaphore instead).
void wl_platform_thread_yield(void);
// Blocks the calling thread (not the whole system) for about `ms` milliseconds.
void wl_platform_thread_sleep_ms(uint32_t ms);
// POSIX sched_yield on the console: devkitPPC's is a stub that returns ENOSYS and yields nothing,
// so portable code spinning on it (or on something built over it) starves the threads of its own
// priority. thread_wii.c defines __wrap_sched_yield as wl_platform_thread_yield; wl_platform's
// CMake target links its users with -Wl,--wrap=sched_yield, and a program linked another way
// passes that flag itself. Host: the C library's sched_yield, unchanged.
// Threads made with pthread_create (the C library's) start at the lowest priority on the console:
// posix.h's wl_posix_thread_set_priority raises them.

// Stack depth measurement. wl_platform_stack_fill writes WL_STACK_FILL_BYTE over a stack before
// a thread runs on it; wl_platform_stack_used then gives the deepest it has been, in bytes (the
// stacks grow down, from `stack + size`): `size` less the run of untouched bytes at the low end.
#define WL_STACK_FILL_BYTE 0xA5u
void wl_platform_stack_fill(void *stack, size_t size);
size_t wl_platform_stack_used(const void *stack, size_t size);
// Runs fn(arg) on a thread of its own on `stack` (32-byte aligned, `stack_size` a multiple of
// 32), at WL_THREAD_PRIO_MAIN, and waits for it; `*out_result` is what fn returned. The stack is
// filled first, so wl_platform_stack_used(stack, stack_size) measures the run, also while it is
// going on. For a program's main: libogc's main thread has a 128 KiB stack, and its audio DSP
// structures lie right below it, so a deep call chain there corrupts them before anything
// faults. Host: the thread really runs on `stack` (pthread_attr_setstack); WL_ERR_ARG when the
// C library refuses it (below PTHREAD_STACK_MIN: 16 KiB on x86-64, 128 KiB on PowerPC Linux).
WlResult wl_platform_run_on_stack(WlPlatformThreadFn fn, void *arg, void *stack, size_t stack_size,
                                  void **out_result);

// Counting semaphores, `initial` <= `max` (WL_ERR_ARG otherwise); a post beyond `max` is dropped.
// sem_wait blocks until the count is positive; waiters wake in priority order.
WlResult wl_platform_sem_create(WlPlatformSemaphore *out, uint32_t initial, uint32_t max);
void wl_platform_sem_post(WlPlatformSemaphore sem);
void wl_platform_sem_wait(WlPlatformSemaphore sem);
void wl_platform_sem_destroy(WlPlatformSemaphore sem);

// Non-recursive mutexes (locking one you already hold deadlocks). Never lock one in a callback
// that runs on the audio thread: it would let a low-priority holder stall the output.
// Thread-local variables: on the console, clang's emulated TLS (-femulated-tls) is
// wl/platform/wii/tls_wii.c's. Native TLS cannot run there (the ABI addresses it from r2, the
// EABI's small data base), and devkitPPC's GCC has no emulated TLS, so code GCC builds must not
// use _Thread_local. A thread that used them calls this as it ends, so a new thread is not given
// its copies. Host stub: nothing (the host has native TLS).
void wl_platform_tls_thread_exit(void);

WlResult wl_platform_mutex_create(WlPlatformMutex *out);
void wl_platform_mutex_lock(WlPlatformMutex mutex);
void wl_platform_mutex_unlock(WlPlatformMutex mutex);
void wl_platform_mutex_destroy(WlPlatformMutex mutex);

// ---- Waits with a deadline, recursive mutexes, condition variables ---------------------------
// For code that ports another system's synchronisation (an emulated kernel's waits, a library's
// critical sections). Timeouts are relative milliseconds; 0 only tries, WL_WAIT_FOREVER blocks.
#define WL_WAIT_FOREVER 0xFFFFFFFFu

// A mutex the thread holding it may lock again; each lock needs its unlock. Same pool and
// handle type as the plain ones (wl_platform_mutex_lock/unlock/destroy take it).
WlResult wl_platform_mutex_create_recursive(WlPlatformMutex *out);
// Locks `mutex` if no other thread holds it (or, for a recursive one, the caller does); false
// at once otherwise.
bool wl_platform_mutex_try_lock(WlPlatformMutex mutex);

// Takes one count, waiting up to `timeout_ms` for it; false when none came in time (0: a try).
bool wl_platform_sem_wait_timeout(WlPlatformSemaphore sem, uint32_t timeout_ms);

// Condition variables over the platform's mutexes (a plain one, held once: a recursive mutex
// held more than once is not released by a wait). A wait releases `mutex`, sleeps until a
// signal or broadcast (or, rarely, for no reason: re-check the condition), and holds `mutex`
// again when it returns. Console: libogc's LWP conditions; host: pthreads on CLOCK_MONOTONIC.
typedef struct {
    uint32_t id;
} WlPlatformCond;
WlResult wl_platform_cond_create(WlPlatformCond *out);
void wl_platform_cond_wait(WlPlatformCond cond, WlPlatformMutex mutex);
// The same with a deadline `timeout_ms` from now: false when it passed without a wake (the
// mutex is held again either way). 0 releases and retakes the mutex without sleeping.
bool wl_platform_cond_wait_timeout(WlPlatformCond cond, WlPlatformMutex mutex, uint32_t timeout_ms);
// Wakes one waiter / every waiter (none waiting: nothing happens, nothing is remembered). May be
// called with or without the mutex held.
void wl_platform_cond_signal(WlPlatformCond cond);
void wl_platform_cond_broadcast(WlPlatformCond cond);
void wl_platform_cond_destroy(WlPlatformCond cond);

// ---- More of a thread's life ----------------------------------------------------------------
// A thread no one will join: its handle and (on the console) its control block are given back
// when it ends. Its stack stays the caller's, which must outlive the thread. Not after a join.
WlResult wl_platform_thread_detach(WlPlatformThread thread);
// Ends the calling thread, as if its function had returned `result`. For a thread made by
// wl_platform_thread_create; a program's main thread leaves through wl_platform_exit instead.
WL_NORETURN void wl_platform_thread_exit(void *result);
// The calling thread's id: not 0, the same for the thread's life, and different from every other
// live thread's (the LWP id on the console; on the host a number the stub hands out on first use).
uint32_t wl_platform_thread_self_id(void);
// Moves a thread to another of the roles' priorities (the console; the host checks and ignores).
WlResult wl_platform_thread_set_priority(WlPlatformThread thread, WlThreadPriority priority);

// ---------------------------------------------------------------------------------------------
// Files (SD and USB through libfat). Blocking: call from an IO thread (fs.h does), except small
// reads at boot and settings writes.
// ---------------------------------------------------------------------------------------------

typedef enum { WL_DEVICE_SD = 0, WL_DEVICE_USB } WlDevice;

// IOS 58 gives USB 2.0. Reloads only when IOS_GetVersion() != 58 (the Homebrew Channel normally
// already runs it, and a reload is a heavy operation that drops every IOS handle), so call it
// once at boot BEFORE WPAD, sockets, SD or USB are initialised. Writes the IOS version running
// afterwards to *out_ios_version (may be NULL). WL_OK when IOS 58 is running; WL_ERR_FAILED when
// the reload failed (the previous IOS keeps running: USB may be slow or absent, SD still works).
// Host stub: WL_OK, version 58. Idempotent.
WlResult wl_platform_fs_ios_init(uint32_t *out_ios_version);

// The single owner of the SD and USB mounts: the log sink and the file API go through it
// (libfat must not be initialised anywhere else). `sd:/` and `usb:/` prefixes address them.
// Idempotent: a mounted device returns WL_OK without touching the hardware. WL_ERR_NOT_FOUND
// when nothing is inserted or plugged in (USB absent is the normal case in Dolphin; a failed USB
// probe can take a few seconds).
WlResult wl_platform_fs_mount(WlDevice device);
bool wl_platform_fs_mounted(WlDevice device);
// Unmounts (flushes first). No-op when not mounted.
void wl_platform_fs_unmount(WlDevice device);

// Monotonic microseconds for throughput timing (the same clock as wl_platform_time_us).
uint64_t wl_platform_fs_clock_us(void);

typedef struct WlPlatformFile WlPlatformFile;

typedef enum { WL_FILE_READ = 0, WL_FILE_WRITE_NEW, WL_FILE_READ_WRITE } WlPlatformFileMode;

// Paths are UTF-8 with a device prefix ("sd:/myapp/config.ini"). File handles come from a fixed
// pool of WL_FS_MAX_FILES (WL_ERR_LIMIT when exhausted). Errors: WL_ERR_NOT_FOUND (no such file
// or directory), WL_ERR_STATE (device not mounted), WL_ERR_ARG (no device prefix, path too
// long), WL_ERR_IO (anything else). A handle is used by one thread at a time (reads seek).
#define WL_FS_MAX_FILES 32u
#define WL_FS_PATH_MAX 192u
// WL_FILE_READ: existing file. WL_FILE_WRITE_NEW: create or truncate. WL_FILE_READ_WRITE: create
// if missing, keep the contents.
WlResult wl_platform_fs_open(const char *path, WlPlatformFileMode mode, WlPlatformFile **out);
// Exact read: all `size` bytes at `offset` or an error (WL_ERR_RANGE when the file ends first,
// WL_ERR_IO on a device error). Offsets must fit 32 bits (FAT limit). Large reads go straight
// to the device: use sector-sized multiples of 512 bytes for speed.
WlResult wl_platform_fs_read_at(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size);
// Appends at the current position (after open WRITE_NEW: from the start).
WlResult wl_platform_fs_write(WlPlatformFile *file, const void *src, uint32_t size);
WlResult wl_platform_fs_flush(WlPlatformFile *file);
uint64_t wl_platform_fs_size(const WlPlatformFile *file);
void wl_platform_fs_close(WlPlatformFile *file);
// Replace for saves (write a temp file, flush, rename over the target). FAT cannot rename over a
// file, so the backends unlink the target first: the replace is not crash-atomic on the device;
// keep a backup copy of anything precious.
WlResult wl_platform_fs_rename(const char *from, const char *to);
WlResult wl_platform_fs_remove(const char *path);
// Creates one directory level; an existing directory is WL_OK.
WlResult wl_platform_fs_mkdir(const char *path);
// True when `path` names an existing file or directory. False for a missing one, an unmounted
// device and a path the file API refuses.
bool wl_platform_fs_exists(const char *path);

// ---- Virtual files and the written hook ----
// A virtual file is a read-only file that exists only in memory, under a path (device prefix
// included) that the file API then answers exactly as for a file on the card: exists, stat, open
// for reading, read, size. It shadows a real file of the same path. Made for a program's run
// options, which are files in its folder (a flag file, a settings text) and can instead be given
// in the command line (a Homebrew Channel's arguments): the code that reads the file does not
// change. `data` is copied. Up to WL_FS_VIRTUAL_MAX; a path is set once (WL_ERR_STATE again),
// from one thread before the code that reads them runs. WL_ERR_ARG for a path without a device
// prefix or too long. Opening one for writing, removing or renaming it go to the real path.
#define WL_FS_VIRTUAL_MAX 24u
WlResult wl_platform_fs_virtual_set(const char *path, const void *data, uint32_t size);
// The bytes of a virtual file (valid for ever), for code that reads files with the C library's
// stdio and so cannot see them. False when `path` is not one.
bool wl_platform_fs_virtual_get(const char *path, const void **data, uint32_t *size);

// A function called with the path of each file written through this API once it is closed (opened
// WRITE_NEW or READ_WRITE and written to, truncated or not), and of each file wl_platform_fs_rename
// moved into place: the place for a remote copy of a program's results. It runs on the thread that
// closed the file, so it must be short and may not use the file API on that path's handle. Code
// that writes with the C library's stdio calls wl_platform_fs_notify_written itself after fclose.
// One hook (NULL removes it); set it from the main thread before the files it watches are opened:
// a file opened before is not reported. Costs one pointer test per close when none is set.
typedef void (*WlFsWrittenFn)(void *user, const char *path);
void wl_platform_fs_set_written_hook(WlFsWrittenFn fn, void *user);
void wl_platform_fs_notify_written(const char *path);

// ---- Files at offsets, sizes, directories (for code that ports another system's file API) ----
// Writes all `size` bytes at `offset` (past the end the file grows; a gap reads as zeros) and
// leaves the handle's position after them. WL_ERR_IO for a device error or a full card.
WlResult wl_platform_fs_write_at(WlPlatformFile *file, uint64_t offset, const void *src, uint32_t size);
// Reads up to `size` bytes at `offset`: fewer when the file ends first (0 at or past the end), in
// `*got`. Not an error at the end, unlike wl_platform_fs_read_at.
WlResult wl_platform_fs_read_some_at(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size,
                                     uint32_t *got);
// Sets the file's length: cut, or grown with zeros (FAT allocates the clusters).
WlResult wl_platform_fs_truncate(WlPlatformFile *file, uint64_t size);

typedef struct {
    uint64_t size;     // bytes (0 for a directory)
    uint64_t mtime_us; // last change, wl_platform_wall_time_us's clock; 0 when the device has none
    bool is_dir;
    bool read_only;
} WlPlatformStat;
// What `path` is. WL_ERR_NOT_FOUND for a missing one; the errors of wl_platform_fs_open else.
WlResult wl_platform_fs_stat(const char *path, WlPlatformStat *out);
// The same for an open file.
WlResult wl_platform_fs_fstat(WlPlatformFile *file, WlPlatformStat *out);
// Removes an empty directory. WL_ERR_NOT_FOUND for a missing one, WL_ERR_DENIED for one with
// something in it.
WlResult wl_platform_fs_rmdir(const char *path);

// A directory's entries, in the order the device keeps them, without "." and "..". Handles
// come from a pool of WL_FS_MAX_DIRS (WL_ERR_LIMIT when exhausted).
#define WL_FS_MAX_DIRS 8u
typedef struct WlPlatformDir WlPlatformDir;
WlResult wl_platform_fs_dir_open(const char *path, WlPlatformDir **out);
// The next entry's name (truncated to `name_cap` - 1 bytes and terminated) and, when `st` is not
// NULL, what it is. WL_ERR_NOT_FOUND after the last.
WlResult wl_platform_fs_dir_next(WlPlatformDir *dir, char *name, size_t name_cap, WlPlatformStat *st);
// Back to the first entry.
void wl_platform_fs_dir_rewind(WlPlatformDir *dir);
void wl_platform_fs_dir_close(WlPlatformDir *dir);
// Finds the folder of a Homebrew Channel application: "sd:/apps/<app_name>", or
// "usb:/apps/<app_name>" when only the USB drive has it. `marker` is a path inside the folder
// (relative, e.g. "maps" or "data/game.pak") that must exist for a drive to count, so that an
// empty or stale folder on the SD card does not hide the full one on the USB drive; NULL or ""
// asks only for the folder itself. The SD card is preferred when both qualify. Writes the folder
// without a trailing slash to `out` and returns WL_OK, or, when neither drive has it, writes the
// SD card's folder (where a first run would create it) and returns WL_ERR_NOT_FOUND. WL_ERR_ARG
// for an empty `app_name` or one holding a slash or "..", an absolute or ".." `marker`, or a path
// that does not fit `out` (`size` bytes) or WL_FS_PATH_MAX. Call after the devices are mounted
// (an unmounted one has nothing). Host: the same, over the directories wl_host_fs_set_root gives.
WlResult wl_platform_fs_find_app_dir(const char *app_name, const char *marker, char *out, size_t size);

// ---------------------------------------------------------------------------------------------
// Entropy
// ---------------------------------------------------------------------------------------------

// Fills `size` bytes at `buf` with unpredictable bytes. NOT cryptographic on the Wii, which has no
// entropy source a program can reach: the bytes come from the processor's time base (its
// free-running counter, read once per byte) put through an avalanche mixer. They suit nonces,
// jitter, port choices and seeds for random play; they do not suit keys or anything that must stay
// out of reach of an attacker who can watch or guess when the program started. Host: getrandom or
// /dev/urandom, which are cryptographic. WL_ERR_ARG for a NULL `buf` with a `size` other than 0;
// the host returns WL_ERR_IO when the system's source fails. Callable from any thread; concurrent
// calls only mix in more.
WlResult wl_platform_random_bytes(void *buf, size_t size);

// ---------------------------------------------------------------------------------------------
// Raw input (an action-mapping layer goes on top)
// ---------------------------------------------------------------------------------------------

#define WL_INPUT_MAX_REMOTES 4u
#define WL_INPUT_MAX_PADS 4u
#define WL_INPUT_MAX_KEYS 16u
// A Nunchuk's acceleration readings kept for a frame (WlRawRemote.nunchuk_accel)
#define WL_NUNCHUK_SAMPLES 16u

// Raw button bits. The values equal libogc's WPAD_BUTTON_* / WPAD_NUNCHUK_BUTTON_* /
// WPAD_CLASSIC_BUTTON_* / PAD_BUTTON_* (input_wii.c asserts it at build time), so the Wii
// backend copies the words unchanged and nothing above this header includes <ogc/*.h>.
// WlRawRemote.buttons: remote bits in the low 16, extension bits in the high 16; which table the
// high half uses depends on WlRawRemote.extension (Nunchuk and Classic bits overlap).
#define WL_WM_2 0x00000001u
#define WL_WM_1 0x00000002u
#define WL_WM_B 0x00000004u
#define WL_WM_A 0x00000008u
#define WL_WM_MINUS 0x00000010u
#define WL_WM_HOME 0x00000080u
#define WL_WM_LEFT 0x00000100u
#define WL_WM_RIGHT 0x00000200u
#define WL_WM_DOWN 0x00000400u
#define WL_WM_UP 0x00000800u
#define WL_WM_PLUS 0x00001000u

#define WL_NC_Z 0x00010000u
#define WL_NC_C 0x00020000u

#define WL_CC_UP 0x00010000u
#define WL_CC_LEFT 0x00020000u
#define WL_CC_ZR 0x00040000u
#define WL_CC_X 0x00080000u
#define WL_CC_A 0x00100000u
#define WL_CC_Y 0x00200000u
#define WL_CC_B 0x00400000u
#define WL_CC_ZL 0x00800000u
#define WL_CC_R 0x02000000u // digital R (fully pressed)
#define WL_CC_PLUS 0x04000000u
#define WL_CC_HOME 0x08000000u
#define WL_CC_MINUS 0x10000000u
#define WL_CC_L 0x20000000u // digital L (fully pressed)
#define WL_CC_DOWN 0x40000000u
#define WL_CC_RIGHT 0x80000000u

// WlRawPad.buttons
#define WL_PAD_LEFT 0x0001u
#define WL_PAD_RIGHT 0x0002u
#define WL_PAD_DOWN 0x0004u
#define WL_PAD_UP 0x0008u
#define WL_PAD_Z 0x0010u
#define WL_PAD_R 0x0020u
#define WL_PAD_L 0x0040u
#define WL_PAD_A 0x0100u
#define WL_PAD_B 0x0200u
#define WL_PAD_X 0x0400u
#define WL_PAD_Y 0x0800u
#define WL_PAD_START 0x1000u

// WlRawKeyboard key modifiers (platform-neutral; the Wii backend derives them from libwiikeyboard).
#define WL_KMOD_SHIFT 0x01u
#define WL_KMOD_CTRL 0x02u
#define WL_KMOD_ALT 0x04u

typedef enum {
    WL_EXT_NONE = 0,
    WL_EXT_NUNCHUK,
    WL_EXT_CLASSIC,
    WL_EXT_MOTIONPLUS,
    WL_EXT_OTHER
} WlRemoteExtension;

typedef struct {
    bool connected;
    uint8_t extension; // WlRemoteExtension
    uint8_t battery;   // 0..100
    uint32_t buttons;  // remote buttons in the low half, extension buttons in the high half
    bool ir_valid;
    int16_t ir_x; // pointer position in screen pixels (fb space 640x480), UNbounded and
    int16_t ir_y; // unsmoothed: values outside the screen mean "pointing off-screen, bar visible"
    int16_t accel[3];   // raw, 1 g ~ 100 units after calibration
    int8_t stick[2][2]; // [nunchuk or classic left, classic right][x, y], -127..127, y up
    uint8_t trigger[2]; // classic analog L/R, 0..255 (a Classic Controller Pro may read 0: use WL_CC_L / WL_CC_R)
    // The Remote reports about every 5 ms, so several times in a frame. wl_platform_input_poll reads
    // them all (input_reports.h): `buttons` has every button pressed in any report of the frame (a
    // tap shorter than a frame is not lost), ir_x / ir_y are the mean of the reports that had a
    // pointer (ir_valid false when none did), `accel` is the last report's.
    int16_t accel_peak[3]; // the frame's report of greatest acceleration, in accel's units (for gestures)
    uint8_t reports;       // reports read this frame, up to 255; 0: none came, the last state is repeated
    // The Nunchuk's accelerometer, one entry for each of the frame's reports that carried it (the
    // last WL_NUNCHUK_SAMPLES, oldest first), in accel's units (1 g ~ 100). `nunchuk_reports` is how
    // many reports of the frame there were in all, which can be more than were kept; 0 when no
    // Nunchuk is connected or none reported. wl_thrust_feed (input_gesture.h) reads these.
    int16_t nunchuk_accel[WL_NUNCHUK_SAMPLES][3];
    uint8_t nunchuk_samples;
    uint8_t nunchuk_reports;
} WlRawRemote;

typedef struct {
    bool connected;
    uint16_t buttons;
    int8_t stick[2][2]; // [main, C][x, y], -127..127 over the stick's real travel, y up
    uint8_t trigger[2]; // analog L/R, 0..255 over the trigger's travel (WL_PAD_L / WL_PAD_R: the click)
} WlRawPad;

typedef struct {
    bool connected;
    uint8_t count;
    struct {
        uint16_t code; // USB HID usage
        uint8_t modifiers;
        bool down;
    } keys[WL_INPUT_MAX_KEYS]; // events since the last poll
} WlRawKeyboard;

typedef struct {
    WlRawRemote remotes[WL_INPUT_MAX_REMOTES];
    WlRawPad pads[WL_INPUT_MAX_PADS];
    WlRawKeyboard keyboard;
} WlRawInput;

// Starts WPAD (IR + accelerometer data format, 640x480 IR resolution), PAD and the USB keyboard
// stack. Idempotent; safe when no controller is connected. wl_platform_input_poll calls it
// when needed, so calling it is optional.
WlResult wl_platform_input_init(void);
void wl_platform_input_shutdown(void);

// Once per frame on the main thread. Never blocks; absent devices read as connected == false.
void wl_platform_input_poll(WlRawInput *out);
// GameCube controllers are read at every video retrace (60 or 50 times a second), not only when the
// program polls, so that a press shorter than a frame is not missed: the poll's `pads[i].buttons`
// has every button seen pressed in any of the retraces since the last poll (the sticks and triggers
// are the latest reading's). Where the retrace cannot read (no video started, the host) the poll reads
// the pads itself. These counters tell how well that works on a console: samples taken at retraces,
// how many of them carried fresh data, and polls that had to read the pads themselves.
typedef struct {
    uint32_t retrace_samples;
    uint32_t retrace_fresh;
    uint32_t polls;
    uint32_t polls_unsampled;
} WlPadStats;
void wl_platform_input_pad_stats(WlPadStats *out);
void wl_platform_rumble(uint8_t remote, bool on);
void wl_platform_pad_rumble(uint8_t pad, bool on);
// Whether the console's settings allow rumble (the Wii's Wii Remote vibration setting, SYSCONF's
// "BT.MOT" through libogc's CONF_GetPadMotorMode; allowed when it cannot be read). Read at each call.
// wl_platform_rumble and wl_platform_pad_rumble do nothing but stop the motor while it is off.
// Host stub: true unless a test says otherwise.
bool wl_platform_rumble_enabled(void);
// The console's sound setting (Wii Settings, Sound: SYSCONF's "IPL.SND" through libogc's
// CONF_GetSoundMode). The output plays what `fill` made; a program that honours the setting (a
// mono downmix) does so in its mixer. Read at each call. A setting that cannot be read, or that
// is not one of the three, is WL_SOUND_STEREO. Host stub: stereo unless a test says otherwise
// (wl_host_sound_mode).
typedef enum { WL_SOUND_MONO = 0, WL_SOUND_STEREO = 1, WL_SOUND_SURROUND = 2 } WlSoundMode;
WlSoundMode wl_platform_sound_mode(void);
// Stops every Wii Remote's and GameCube controller's motor, whatever asked for it. Safe to call
// from any thread, before or after wl_platform_input_init (nothing is touched while the input
// is not started). wl_platform_exit and the stop screen call it; wl_platform_input_shutdown does
// it too.
void wl_platform_input_stop_rumble(void);

// ---------------------------------------------------------------------------------------------
// Audio output: one stereo PCM16 stream fed by a fill callback on the audio thread.
// ---------------------------------------------------------------------------------------------

// Fills `frames` interleaved stereo samples. Runs on the audio thread at WL_THREAD_PRIO_AUDIO;
// must not block or allocate.
typedef void (*WlAudioFillFn)(void *user, int16_t *stereo, uint32_t frames);

// Starts output at `sample_rate` (e.g. 32000) through a ring of buffers; the DSP resamples to
// 48 kHz. `block_frames` (e.g. 256) is the largest chunk `fill` is asked for: the backend may
// call it with fewer frames (the console's AESND hardware block is 288 frames, built from two
// calls of 144) but never more. Console: the backend owns a thread at WL_THREAD_PRIO_AUDIO that
// calls `fill` (its stack and buffers come from WL_TAG_STACK and WL_TAG_AUDIO, so wl_mem_init
// must have run); the DMA interrupt only hands out finished blocks (no locks, no mixing in
// interrupt context). `fill` is never called after wl_platform_audio_stop returns. Host stub: no
// thread; the test renders blocks itself (wl/platform/host/audio_host.h).
WlResult wl_platform_audio_start(uint32_t sample_rate, uint32_t block_frames, WlAudioFillFn fill,
                                 void *user);
void wl_platform_audio_stop(void);
// The master gain of the output, applied to what `fill` produced just before it goes to the
// hardware (the audio thread's, in blocks of the hardware's size): ramps linearly from the
// present gain to `gain` (0 silent to 1 untouched; clamped) over `ms` milliseconds of output (0:
// at once), so a program can fade out for a pause menu and back without touching its mixer. Costs
// one test a block while the gain is 1 and not ramping. Callable from any thread, before the
// output starts (it applies from the first block) and after it stops; a new call continues from
// where the ramp is. Host stub: applied in wl_host_audio_render and the test thread.
void wl_platform_audio_fade(float gain, uint32_t ms);
// The gain the output is heading for (the last `gain` given; 1 before any).
float wl_platform_audio_gain(void);
// Blocks the hardware needed and the mixer had not finished (a silent block was played instead).
uint32_t wl_platform_audio_underruns(void);

// Which output path runs: AESND first, ASND when AESND never starts consuming blocks.
typedef enum { WL_AUDIO_BACKEND_AUTO = 0, WL_AUDIO_BACKEND_AESND, WL_AUDIO_BACKEND_ASND } WlAudioBackend;
// Chooses the path for the next wl_platform_audio_start (AUTO = AESND, then ASND as the fallback).
void wl_platform_audio_prefer(WlAudioBackend backend);
// True when `fill` runs on its own thread (console); false on the host stub, where the caller of
// wl_host_audio_render is the audio thread.
bool wl_platform_audio_threaded(void);

typedef struct {
    uint32_t backend;        // 0 none, 1 AESND, 2 ASND, 3 host stub
    uint32_t hw_block_frames; // frames per hardware block (AESND 288, ASND 1024); host stub: its block
    uint32_t blocks_filled;  // hardware blocks completed by the audio thread (the host stub: `fill` calls)
    uint32_t blocks_played;  // blocks handed to the hardware by the DMA/voice callback
    uint32_t underruns;      // same as wl_platform_audio_underruns
    uint32_t fill_us_max;    // longest time to build one hardware block (all its `fill` calls)
    uint32_t fill_us_total;  // sum over hardware blocks (mean = total / blocks_filled)
} WlPlatformAudioStats;
void wl_platform_audio_stats(WlPlatformAudioStats *out);

// ---------------------------------------------------------------------------------------------
// Logging: the platform's sink for wl/base logging (memory ring for a crash screen or overlay,
// the EXI UART that Dolphin shows as OSREPORT, the on-screen console in debug builds, a file on
// SD). Host stub: the ring plus stderr.
// ---------------------------------------------------------------------------------------------

void wl_platform_log_sink(void *user, WlLogLevel level, const char *tag, const char *line);

// Last `max` lines of the in-memory ring for a crash screen or overlay; returns the count.
uint32_t wl_platform_log_tail(const char **lines, uint32_t max);

// Installs wl_platform_log_sink with wl_log_set_sink. Idempotent. `sd_dir` (e.g.
// "sd:/apps/myapp/logs") enables the SD file sink: the directory is created as needed and the
// file is <sd_dir>/log-NN.log, NN the first unused run number in 00..99 (00 is overwritten when
// all are taken). NULL, or no SD card, keeps the file sink off. A later call with a directory
// opens the file sink if the first had none (a program that reloads IOS first, and mounts the card
// after it), lines logged before it not being in the file. Ignored by the host stub.
void wl_platform_log_init(const char *sd_dir);
// Writes buffered lines to the SD file and syncs it to the card. The sink does this itself when its
// buffer is full and at the first line 2 s after the last time, so a program need not call it per
// frame; call it before a power-off the program causes itself (wl_platform_exit does).
void wl_platform_log_flush(void);
// Flushes and closes the SD file (also registered with atexit).
void wl_platform_log_shutdown(void);
// The crash report: a CPU exception (an access fault, an alignment or program exception) writes
// its kind, the registers, the return addresses up the stack and the log ring's last lines to the
// EXI UART (Dolphin's OSREPORT) before libogc's panic screen; not to the SD file (it cannot be
// written safely from the crashed thread: it may hold libfat's lock). Idempotent; a no-op on the host.
void wl_platform_crash_report_install(void);
