// SPDX-License-Identifier: MIT
// The video mode the console's settings ask for (video_mode.h): a pure function.
#include "wl/platform/video_mode.h"

#include <stdio.h>
#include <string.h>

#define LINE_WIDTH 720u    // the video interface's line, in pixels
#define PICTURE_WIDTH 640u // what an application draws
#define PICTURE_HEIGHT 480u
#define WIDE_VI_WIDTH 704u // the 16:9 picture's width on the line (the clean aperture; the rest is overscan)

void wl_video_choose(const WlVideoSettings *s, WlVideoChoice *c) {
    memset(c, 0, sizeof *c);
    c->progressive = s->progressive_setting && s->component_cable;
    c->wide = s->wide_setting;
    c->fb_width = PICTURE_WIDTH;
    c->efb_height = PICTURE_HEIGHT;
    c->xfb_height = PICTURE_HEIGHT;
    c->refresh_mhz = 59940u;
    switch (s->tv) {
    case WL_TV_PAL:
        if (s->eurgb60_setting) {
            c->system = WL_VIDEO_EURGB60;
        } else {
            c->system = WL_VIDEO_PAL50;
            c->xfb_height = 576u;
            c->refresh_mhz = 50000u;
        }
        break;
    case WL_TV_MPAL:
        c->system = WL_VIDEO_MPAL;
        break;
    default:
        c->system = WL_VIDEO_NTSC;
        break;
    }

    // The line: the 640 pixels libogc's modes scan, or the 704 of a 16:9 TV, centred (the interface
    // widens the framebuffer to it).
    c->vi_width = (uint16_t)(c->wide ? WIDE_VI_WIDTH : PICTURE_WIDTH);
    c->vi_x_origin = (uint16_t)((LINE_WIDTH - c->vi_width) / 2u);
    // The display position: libogc adds it to the origin and keeps the result on the line.
    int pos = (int)c->vi_x_origin + s->display_offset_h;
    int most = (int)(LINE_WIDTH - c->vi_width);
    pos = pos < 0 ? 0 : pos > most ? most : pos;
    c->offset_h = (int16_t)(pos - (int)c->vi_x_origin);

    c->pillarbox = c->wide && s->widescreen == WL_WIDESCREEN_PILLARBOX;
    if (c->pillarbox) {
        // 3/4 of the width: 4:3 of a 16:9 screen. Even, so that a pixel pair (the XFB's YUV) is not split.
        c->picture_width = (uint16_t)((c->fb_width / 4u * 3u) & ~1u);
        c->picture_x = (uint16_t)(((c->fb_width - c->picture_width) / 2u) & ~1u);
    } else {
        c->picture_x = 0;
        c->picture_width = c->fb_width;
    }
}

void wl_video_describe(const WlVideoChoice *c, char *out, size_t size) {
    static const char *const system[] = {"NTSC", "EuRGB60", "PAL 50", "MPAL"};
    char shape[96];
    if (!c->wide) {
        (void)snprintf(shape, sizeof shape, "4:3");
    } else if (c->pillarbox) {
        (void)snprintf(shape, sizeof shape, "16:9, the 4:3 picture at %u..%u of %u", (unsigned)c->picture_x,
                       (unsigned)(c->picture_x + c->picture_width), (unsigned)c->fb_width);
    } else {
        (void)snprintf(shape, sizeof shape, "16:9, the picture stretched");
    }
    (void)snprintf(out, size, "%u%c %s %u.%02u Hz, XFB %ux%u, EFB %ux%u, line %u at %d%+d, %s", (unsigned)c->xfb_height,
                   c->progressive ? 'p' : 'i', system[c->system], (unsigned)(c->refresh_mhz / 1000u),
                   (unsigned)(c->refresh_mhz % 1000u / 10u), (unsigned)c->fb_width, (unsigned)c->xfb_height,
                   (unsigned)c->fb_width, (unsigned)c->efb_height, (unsigned)c->vi_width, (int)c->vi_x_origin,
                   (int)c->offset_h, shape);
}

static uint32_t gcd(uint32_t a, uint32_t b) {
    while (b != 0u) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

void wl_video_rate_ratio(uint32_t refresh_mhz, uint32_t hz, uint32_t *num, uint32_t *den) {
    // (the nominal rates: 60 for NTSC's 59.94, so that 30 frames a second is exactly two retraces)
    uint32_t nominal = refresh_mhz >= 55000u ? 60u : 50u;
    if (hz == 0u || hz >= nominal) {
        *num = *den = 1u;
        return;
    }
    uint32_t g = gcd(nominal, hz);
    *num = nominal / g;
    *den = hz / g;
}
