// SPDX-License-Identifier: MIT
// Wii video and frame pacing: preferred mode from the console configuration (video_mode.h chooses,
// libogc's VIDEO_GetPreferredMode is the object that gets used and is checked against it), two or
// three XFBs in a display queue (present.h) that the retrace interrupt advances, so presenting
// never waits for the display; frame / late-frame counters.
// Only this directory may include <ogc/*.h>.
#include <ogc/cache.h>
#include <ogc/conf.h>
#include <ogc/irq.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <ogc/video_types.h>
#include <stdio.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/platform/timebase.h"
#include "wl/platform/hwcount.h"
#include "wl/platform/present.h"
#include "wl/platform/video_mode.h"
#include "wl/platform/wii/input_wii.h"

#define TAG "VIDEO"
// Retraces to wait for a free XFB before deciding the GPU will not finish (a quarter second at 60 Hz)
#define WAIT_RETRACES 15u

static GXRModeObj *g_rmode;
static void *g_xfb[WL_PRESENT_MAX_BUFFERS];
static uint32_t g_buffers = 2;  // wl_platform_video_set_buffers, before init
static uint32_t g_num = 1, g_den = 1; // wl_platform_video_set_interval / set_rate: retraces a frame, as a ratio
static uint32_t g_rate_hz;            // wl_platform_video_set_rate (0: every retrace, or an interval's)
static WlWidescreen g_widescreen = WL_WIDESCREEN_PILLARBOX;
static WlVideoChoice g_choice;        // what init chose
static char g_description[200];
static WlPresentQueue g_q;      // changed by the retrace interrupt: read and written with it disabled
static bool g_inited;
static volatile uint32_t g_retraces;
static uint32_t g_last_present;
static uint32_t g_waits;
static uint64_t g_wait_us, g_wait_us_max;

static int xfb_index(const void *fb) {
    for (uint32_t i = 0; i < g_buffers; i++) {
        if (g_xfb[i] == fb) {
            return (int)i;
        }
    }
    return -1;
}

// Before libogc writes the video interface's registers at a retrace: the next frame due, if any,
// becomes the framebuffer it scans from this retrace on.
static void on_pre_retrace(u32 retrace_count) {
    (void)retrace_count;
    if (!g_inited) {
        return;
    }
    int i = wl_present_retrace_begin(&g_q);
    if (i >= 0) {
        VIDEO_SetNextFramebuffer(g_xfb[i]);
        VIDEO_Flush();
    }
}

static WlRetraceHook volatile g_hook;
static void *volatile g_hook_user;

// After: what the interface scans now (a committed frame has reached the screen).
static void on_retrace(u32 retrace_count) {
    (void)retrace_count;
    g_retraces++;
    WlRetraceHook hook = g_hook;
    if (hook != NULL) {
        hook(g_hook_user);
    }
    // (the GameCube pads are read at every retrace, so that a press shorter than a frame is seen)
    wl_input_wii_retrace();
    if (g_inited) {
        wl_present_retrace_end(&g_q, xfb_index(VIDEO_GetCurrentFramebuffer()));
    }
}

static bool mode_is_pal50(const GXRModeObj *m) {
    return (m->viTVMode >> 2) == VI_PAL;
}

static bool mode_is_progressive(const GXRModeObj *m) {
    return (m->viTVMode & 3) == VI_PROGRESSIVE;
}

// What the console is set to (SYSCONF through libogc, and the cable the interface sees).
static void read_settings(WlVideoSettings *s) {
    memset(s, 0, sizeof *s);
    s32 video = CONF_GetVideo();
    s->tv = video == CONF_VIDEO_PAL ? WL_TV_PAL : video == CONF_VIDEO_MPAL ? WL_TV_MPAL : WL_TV_NTSC;
    s->progressive_setting = CONF_GetProgressiveScan() > 0;
    s->component_cable = VIDEO_HaveComponentCable() != 0;
    s->eurgb60_setting = CONF_GetEuRGB60() > 0;
    s->wide_setting = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
    s8 offset = 0;
    s->display_offset_h = CONF_GetDisplayOffsetH(&offset) == 0 ? (int)offset : 0;
    s->widescreen = g_widescreen;
}

static void fill_info_from(const WlVideoChoice *c, WlVideoInfo *out) {
    out->fb_width = c->fb_width;
    out->efb_height = c->efb_height;
    out->xfb_height = c->xfb_height;
    out->pal = c->system == WL_VIDEO_PAL50;
    out->refresh_mhz = (uint16_t)c->refresh_mhz;
    out->progressive = c->progressive;
    out->widescreen = c->wide;
    out->pillarbox = c->pillarbox;
    out->picture_x = c->picture_x;
    out->picture_width = c->picture_width;
    out->offset_h = c->offset_h;
}

// The ratio of retraces to a frame for the rate asked (an interval's whole number when none is).
static void apply_ratio(void) {
    uint32_t num = g_num, den = g_den;
    if (g_rate_hz != 0u && g_inited) {
        wl_video_rate_ratio(g_choice.refresh_mhz, g_rate_hz, &num, &den);
    }
    wl_present_set_ratio(&g_q, num, den);
}

WlResult wl_platform_video_init(void) {
    if (g_inited) {
        return WL_OK;
    }
    CONF_Init(); // idempotent; VIDEO_Init reads the display position, and VIDEO_GetPreferredMode the rest, through it
    VIDEO_Init();
    // libogc applies SYSCONF here: progressive scan only with a component cable, EuRGB60 for
    // PAL consoles with it enabled, 50 Hz 576i otherwise, 480i for NTSC. wl_video_choose says the
    // same from the same settings (a test compares them); the mode object is libogc's own.
    g_rmode = VIDEO_GetPreferredMode(NULL);
    if (g_rmode == NULL) {
        return WL_ERR_UNSUPPORTED;
    }
    WlVideoSettings settings;
    read_settings(&settings);
    wl_video_choose(&settings, &g_choice);
    if (mode_is_progressive(g_rmode) != g_choice.progressive || mode_is_pal50(g_rmode) != (g_choice.system == WL_VIDEO_PAL50) ||
        g_rmode->xfbHeight != g_choice.xfb_height || g_rmode->efbHeight != g_choice.efb_height ||
        g_rmode->fbWidth != g_choice.fb_width) {
        WL_LOG_W(TAG, "libogc's mode (%ux%u EFB %u, %s) differs from the settings' (%ux%u EFB %u, %s): libogc's is used",
                 (unsigned)g_rmode->fbWidth, (unsigned)g_rmode->xfbHeight, (unsigned)g_rmode->efbHeight,
                 mode_is_progressive(g_rmode) ? "progressive" : "interlaced", (unsigned)g_choice.fb_width,
                 (unsigned)g_choice.xfb_height, (unsigned)g_choice.efb_height, g_choice.progressive ? "progressive" : "interlaced");
        g_choice.fb_width = g_rmode->fbWidth;
        g_choice.efb_height = g_rmode->efbHeight;
        g_choice.xfb_height = g_rmode->xfbHeight;
        g_choice.progressive = mode_is_progressive(g_rmode);
        g_choice.pillarbox = g_choice.pillarbox && g_rmode->fbWidth == 640;
    }
    // The preferred modes scan 640 pixels out of a 720-pixel line, which leaves black bars at the
    // sides of a 16:9 TV. There the interface widens the same 640-wide framebuffer to 704 pixels
    // (centred), and the picture is either stretched with it or narrowed into the middle three
    // quarters of the framebuffer first (wl_gx_end_frame), so that it comes out 4:3. libogc adds the
    // console's display position to the origin itself. The mode object is libogc's own static
    // one, so later VIDEO_GetPreferredMode calls (GX setup) see the same values.
    g_rmode->viWidth = g_choice.vi_width;
    g_rmode->viXOrigin = g_choice.vi_x_origin;
    wl_video_describe(&g_choice, g_description, sizeof g_description);
    WL_LOG_I(TAG, "%s", g_description);
    for (uint32_t i = 0; i < g_buffers; i++) {
        void *p = SYS_AllocateFramebuffer(g_rmode);
        if (p == NULL) {
            return WL_ERR_NOMEM;
        }
        g_xfb[i] = MEM_K0_TO_K1(p);
        // Black in YUV (Y=0x10, Cb=Cr=0x80). Zero is green, which would make boot frames non-deterministic
        // and visible as a flash; golden-frame tests rely on this.
        uint32_t *px = g_xfb[i];
        for (size_t n = 0; n < (size_t)VIDEO_GetFrameBufferSize(g_rmode) / 4u; n++) {
            px[n] = 0x10801080u;
        }
    }
    VIDEO_Configure(g_rmode);
    VIDEO_SetPreRetraceCallback(on_pre_retrace);
    VIDEO_SetPostRetraceCallback(on_retrace);
    VIDEO_SetNextFramebuffer(g_xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (g_rmode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync(); // the first retrace after a mode change is not yet the new mode
    }
    u32 level = IRQ_Disable();
    (void)wl_present_init(&g_q, g_buffers, 1, 0); // XFB 0 on the screen
    g_inited = true;
    apply_ratio();
    g_last_present = g_retraces;
    IRQ_Restore(level);
    return WL_OK;
}

WlResult wl_platform_video_set_buffers(uint32_t count) {
    if (count < 2u || count > WL_PRESENT_MAX_BUFFERS) {
        return WL_ERR_ARG;
    }
    if (g_inited) {
        return count == g_buffers ? WL_OK : WL_ERR_STATE;
    }
    g_buffers = count;
    return WL_OK;
}

void wl_platform_video_set_interval(uint32_t retraces) {
    g_num = retraces < 1u ? 1u : retraces;
    g_den = 1u;
    g_rate_hz = 0;
    if (g_inited) {
        u32 level = IRQ_Disable();
        apply_ratio();
        IRQ_Restore(level);
    }
}

void wl_platform_video_set_rate(uint32_t hz) {
    g_rate_hz = hz;
    if (g_inited) {
        u32 level = IRQ_Disable();
        apply_ratio();
        IRQ_Restore(level);
    }
}

WlResult wl_platform_video_set_widescreen(WlWidescreen mode) {
    if (mode != WL_WIDESCREEN_PILLARBOX && mode != WL_WIDESCREEN_STRETCH) {
        return WL_ERR_ARG;
    }
    if (g_inited) {
        return mode == g_widescreen ? WL_OK : WL_ERR_STATE;
    }
    g_widescreen = mode;
    return WL_OK;
}

void wl_platform_video_info(WlVideoInfo *out) {
    memset(out, 0, sizeof *out);
    if (g_rmode != NULL) {
        fill_info_from(&g_choice, out);
    }
}

void wl_platform_video_describe(char *out, size_t size) {
    snprintf(out, size, "%s", g_inited ? g_description : "");
}

void wl_platform_video_expected(WlVideoInfo *out) {
    memset(out, 0, sizeof *out);
    WlVideoSettings settings;
    WlVideoChoice choice;
    read_settings(&settings);
    wl_video_choose(&settings, &choice);
    fill_info_from(&choice, out);
}

void *wl_platform_video_back_buffer(void) {
    if (!g_inited) {
        return NULL;
    }
    uint64_t t0 = 0;
    for (uint32_t waited = 0;; waited++) {
        u32 level = IRQ_Disable();
        int i = wl_present_acquire(&g_q);
        IRQ_Restore(level);
        if (i >= 0) {
            if (waited != 0) {
                uint64_t us = wl_timebase_us(wl_timebase_read()) - t0;
                g_waits++;
                g_wait_us += us;
                g_wait_us_max = us > g_wait_us_max ? us : g_wait_us_max;
            }
            return g_xfb[i];
        }
        if (waited == 0) {
            t0 = wl_timebase_us(wl_timebase_read());
        } else if (waited % WAIT_RETRACES == 0) {
            // every XFB still waits for a GPU that has not finished in a quarter second: drop the
            // oldest frame rather than hang
            level = IRQ_Disable();
            int r = wl_present_reclaim(&g_q);
            IRQ_Restore(level);
            WL_LOG_E(TAG, "no free XFB after %u retraces: %s", (unsigned)waited,
                     r >= 0 ? "a waiting frame dropped" : "still waiting");
        }
        wl_hwcount_wait_begin(WL_HWWAIT_DISPLAY);
        VIDEO_WaitVSync(); // (sleeps; the queue moves on in the retrace interrupt)
        wl_hwcount_wait_end(WL_HWWAIT_DISPLAY);
    }
}

uint32_t wl_platform_video_present_when(WlPresentDone done, uint32_t done_arg) {
    if (!g_inited) {
        return 0;
    }
    (void)wl_platform_video_back_buffer(); // (nothing acquired: a free XFB is shown as it is)
    u32 level = IRQ_Disable();
    (void)wl_present_submit(&g_q, done, done_arg);
    IRQ_Restore(level);
    uint32_t now = g_retraces;
    uint32_t n = now - g_last_present;
    g_last_present = now;
    return n;
}

uint32_t wl_platform_video_present(void) {
    return wl_platform_video_present_when(NULL, 0);
}

bool wl_platform_video_wait_shown(void) {
    for (uint32_t waited = 0; g_inited; waited++) {
        u32 level = IRQ_Disable();
        bool idle = wl_present_idle(&g_q);
        IRQ_Restore(level);
        if (idle) {
            return true;
        }
        if (waited == WAIT_RETRACES * 2u) {
            return false;
        }
        VIDEO_WaitVSync();
    }
    return true;
}

void wl_platform_video_stats(WlVideoStats *out) {
    memset(out, 0, sizeof *out);
    u32 level = IRQ_Disable();
    out->presented = g_q.presented;
    out->shown = g_q.latched;
    out->late = g_q.late;
    out->reclaimed = g_q.reclaimed;
    out->last_gap = g_q.last_gap;
    out->interval = g_q.interval;
    IRQ_Restore(level);
    out->buffers = g_buffers;
    out->retraces = g_retraces;
    out->buffer_waits = g_waits;
    out->buffer_wait_us = g_wait_us;
    out->buffer_wait_us_max = g_wait_us_max;
}

uint32_t wl_platform_video_frame_count(void) {
    return g_q.presented;
}

uint32_t wl_platform_video_shown_count(void) {
    return g_q.latched;
}

uint32_t wl_platform_video_retrace_count(void) {
    return g_retraces;
}

void wl_platform_video_set_retrace_hook(WlRetraceHook fn, void *user) {
    u32 level = IRQ_Disable(); // the pair changes between two retraces, never across one
    g_hook = NULL;
    g_hook_user = user;
    g_hook = fn;
    IRQ_Restore(level);
}

uint32_t wl_platform_video_dropped_count(void) {
    return g_q.late;
}
