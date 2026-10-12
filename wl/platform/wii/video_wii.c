// SPDX-License-Identifier: MIT
// Wii video and frame pacing: preferred mode from the console configuration, two or three XFBs
// in a display queue (present.h) that the retrace interrupt advances, so presenting never waits
// for the display; frame / late-frame counters.
// Only this directory may include <ogc/*.h>.
#include <ogc/cache.h>
#include <ogc/conf.h>
#include <ogc/irq.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <ogc/video_types.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/platform/hwcount.h"
#include "wl/platform/present.h"

#define TAG "VIDEO"
// Retraces to wait for a free XFB before deciding the GPU will not finish (a quarter second at 60 Hz)
#define WAIT_RETRACES 15u

static GXRModeObj *g_rmode;
static void *g_xfb[WL_PRESENT_MAX_BUFFERS];
static uint32_t g_buffers = 2;  // wl_platform_video_set_buffers, before init
static uint32_t g_interval = 1; // wl_platform_video_set_interval
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

// After: what the interface scans now (a committed frame has reached the screen).
static void on_retrace(u32 retrace_count) {
    (void)retrace_count;
    g_retraces++;
    if (g_inited) {
        wl_present_retrace_end(&g_q, xfb_index(VIDEO_GetCurrentFramebuffer()));
    }
}

static bool mode_is_pal50(const GXRModeObj *m) {
    return (m->viTVMode >> 2) == VI_PAL;
}

static void fill_info(const GXRModeObj *m, bool widescreen, WlVideoInfo *out) {
    out->fb_width = m->fbWidth;
    out->efb_height = m->efbHeight;
    out->xfb_height = m->xfbHeight;
    out->pal = mode_is_pal50(m);
    out->refresh_mhz = out->pal ? 50000u : 59940u;
    out->progressive = (m->viTVMode & VI_NON_INTERLACE) != 0 && m->xfbMode == VI_XFBMODE_SF;
    out->widescreen = widescreen;
}

WlResult wl_platform_video_init(void) {
    if (g_inited) {
        return WL_OK;
    }
    VIDEO_Init();
    CONF_Init(); // idempotent; VIDEO_GetPreferredMode reads SYSCONF through it
    // libogc applies SYSCONF here: progressive scan only with a component cable, EuRGB60 for
    // PAL consoles with it enabled, 50 Hz 576i otherwise, 480i for NTSC.
    g_rmode = VIDEO_GetPreferredMode(NULL);
    if (g_rmode == NULL) {
        return WL_ERR_UNSUPPORTED;
    }
    // The preferred modes scan 640 pixels out of a 720-pixel line, which leaves black bars at the
    // sides of a 16:9 TV. There the VI widens the same 640-wide framebuffer to 704 (centred), so
    // the picture fills the screen; rendering is unchanged. The mode object is libogc's own static
    // one, so later VIDEO_GetPreferredMode calls (GX setup) see the same values.
    if (CONF_GetAspectRatio() == CONF_ASPECT_16_9) {
        g_rmode->viWidth = 704;
        g_rmode->viXOrigin = (VI_MAX_WIDTH_NTSC - 704) / 2;
    }
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
    (void)wl_present_init(&g_q, g_buffers, g_interval, 0); // XFB 0 on the screen
    g_last_present = g_retraces;
    g_inited = true;
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
    g_interval = retraces;
    if (g_inited) {
        u32 level = IRQ_Disable();
        wl_present_set_interval(&g_q, retraces);
        IRQ_Restore(level);
    }
}

void wl_platform_video_info(WlVideoInfo *out) {
    memset(out, 0, sizeof *out);
    if (g_rmode != NULL) {
        fill_info(g_rmode, CONF_GetAspectRatio() == CONF_ASPECT_16_9, out);
    }
}

void wl_platform_video_expected(WlVideoInfo *out) {
    memset(out, 0, sizeof *out);
    s32 video = CONF_GetVideo();
    bool prog = CONF_GetProgressiveScan() > 0 && VIDEO_HaveComponentCable();
    bool pal = (video == CONF_VIDEO_PAL) && CONF_GetEuRGB60() <= 0;
    out->fb_width = 640;
    out->pal = pal;
    out->efb_height = pal ? 528 : 480;
    out->refresh_mhz = pal ? 50000u : 59940u;
    out->progressive = prog;
    out->widescreen = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
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
                uint64_t us = ticks_to_microsecs(gettime()) - t0;
                g_waits++;
                g_wait_us += us;
                g_wait_us_max = us > g_wait_us_max ? us : g_wait_us_max;
            }
            return g_xfb[i];
        }
        if (waited == 0) {
            t0 = ticks_to_microsecs(gettime());
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

uint32_t wl_platform_video_dropped_count(void) {
    return g_q.late;
}
