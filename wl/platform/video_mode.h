// SPDX-License-Identifier: MIT
// Which video mode the console's settings ask for, as a pure function of those settings: the TV
// standard, progressive scan with a component cable, EuRGB60, the 16:9 setting and the display
// position. wii/video_wii.c reads the settings from SYSCONF (through libogc), asks here, applies
// the result to libogc's mode object and checks that libogc's own choice (VIDEO_GetPreferredMode)
// agrees; the host tests drive it with every combination.
//
// The picture an application draws is 640x480 (4:3). On a 16:9 TV the video interface stretches
// whatever line it scans across the whole screen, so a 4:3 picture is either stretched sideways
// (WL_WIDESCREEN_STRETCH) or drawn into the middle three quarters of the framebuffer with black on
// each side, which the interface then stretches to 16:9 (WL_WIDESCREEN_PILLARBOX: the picture is
// 4:3 again, the default). The interface can only widen a framebuffer, never narrow one, so the
// pillarbox is made in the framebuffer by the GX layer (wl_gx_end_frame), not by the interface.
#pragma once

#include "wl/base/base.h"

typedef enum {
    WL_TV_NTSC = 0, // the console's video standard (SYSCONF), as libogc's CONF_VIDEO_*
    WL_TV_PAL,
    WL_TV_MPAL
} WlTvStandard;

typedef enum {
    WL_WIDESCREEN_PILLARBOX = 0, // a 16:9 console setting: the 4:3 picture in the middle, black at the sides
    WL_WIDESCREEN_STRETCH        // ...the 4:3 picture stretched over the whole 16:9 screen
} WlWidescreen;

typedef enum {
    WL_VIDEO_NTSC = 0, // 59.94 Hz, 480 lines
    WL_VIDEO_EURGB60,  // a PAL console's 60 Hz mode: NTSC's timing, RGB colour
    WL_VIDEO_PAL50,    // 50 Hz, 576 lines
    WL_VIDEO_MPAL      // 59.94 Hz, 480 lines (Brazil)
} WlVideoSystem;

typedef struct {
    WlTvStandard tv;
    bool progressive_setting; // the console's "480p" setting (CONF_GetProgressiveScan > 0)
    bool component_cable;     // the cable the interface reports (VIDEO_HaveComponentCable)
    bool eurgb60_setting;     // the console's "EuRGB60" setting (a PAL console only)
    bool wide_setting;        // the console's 16:9 setting
    int display_offset_h;     // the console's horizontal display position, pixels (CONF_GetDisplayOffsetH)
    WlWidescreen widescreen;  // what to do with the picture on a 16:9 setting
} WlVideoSettings;

typedef struct {
    WlVideoSystem system;
    bool progressive;
    bool wide;            // the console says 16:9
    bool pillarbox;       // the picture is composed into the middle of the framebuffer (picture_*)
    uint16_t fb_width;    // 640: the XFB's width, and the EFB's
    uint16_t efb_height;  // 480 rows are the picture (the EFB's rows below it are scratch)
    uint16_t xfb_height;  // 480, or 576 at 50 Hz (the display copy scales 480 rows to it)
    uint32_t refresh_mhz; // 59940 or 50000: a retrace is a field in an interlaced mode
    uint16_t vi_width;    // pixels of the 720-pixel line the interface scans the framebuffer into
    uint16_t vi_x_origin; // ...from here
    int16_t offset_h;     // the display position libogc adds to the origin, after limiting it to the line
    uint16_t picture_x;   // the picture within the framebuffer: all of it, or the pillarbox's middle
    uint16_t picture_width;
} WlVideoChoice;

// The mode for `settings`. Pure.
void wl_video_choose(const WlVideoSettings *settings, WlVideoChoice *choice);

// One line saying what was chosen ("480p NTSC 59.94 Hz, XFB 640x480, ...").
void wl_video_describe(const WlVideoChoice *choice, char *out, size_t size);

// The retraces a frame should stay on the screen, as a ratio, for at most `hz` frames a second on
// a display of `refresh_mhz`: 2/1 for 30 on 60 Hz, 5/3 for 30 on 50 Hz (frames alternate between
// two retraces and one), 1/1 where the display cannot go as fast as asked. Reduced; pure.
void wl_video_rate_ratio(uint32_t refresh_mhz, uint32_t hz, uint32_t *num, uint32_t *den);
