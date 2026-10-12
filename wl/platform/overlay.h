// SPDX-License-Identifier: MIT
// Drawing for a frame overlay (wl_gx_set_overlay): flat rectangles and a small built-in font, as
// quads through the wl_gx layer, so a program can put a menu, a notice or a gauge over its finished
// frame with no textures, no assets and no renderer of its own. Meant for the overlay callback, which
// runs with the state reset to the whole EFB and an orthographic projection over its pixels (y down);
// anything else must call wl_gx_reset_state first. The host stub records the quads like any draw.
//
// The font is 5 by 7 pixels in a cell of 6 by 9 (a pixel of the glyph is `scale` pixels of the
// screen), capitals, digits and  . , : ; - + ! ? % / ( ) ' _ > < [ ] = *  only: lower case is drawn as
// capitals and any other character as a box. Text is drawn with a one-pixel shadow so it reads over
// any picture.
#pragma once

#include "wl/base/base.h"

// Prepares the state for the calls below: alpha blending, no depth, vertex colours. Call once at the
// start of the overlay.
void wl_overlay_begin(void);
// Sends the quads not yet drawn. The calls below flush by themselves when their batch fills, and
// wl_overlay_end sends the rest; a callback that returns without calling it loses the last few.
void wl_overlay_end(void);

// A rectangle of `rgba` (0xRRGGBBAA, alpha blended) at x, y of w by h pixels.
void wl_overlay_rect(int x, int y, int w, int h, uint32_t rgba);
// A rectangle's outline `thickness` pixels wide, inside the rectangle.
void wl_overlay_frame(int x, int y, int w, int h, int thickness, uint32_t rgba);
// Text with its top left corner at x, y (a newline starts the next line), `scale` pixels to a glyph
// pixel (1 to 8). Returns the width drawn in pixels.
int wl_overlay_text(int x, int y, int scale, uint32_t rgba, const char *text);
// The width and the height text takes at `scale`, without drawing it.
int wl_overlay_text_width(int scale, const char *text);
int wl_overlay_text_height(int scale, const char *text);

// The glyph's rows (7 of them, the low 5 bits of each, the leftmost pixel the highest bit) for a
// character, lower case folded to capitals; unknown characters give a box. For tests and for
// drawing the font some other way.
const uint8_t *wl_overlay_glyph(char c);

// A gauge: a rectangle outlined with `fraction` (0 to 1) of it filled.
void wl_overlay_gauge(int x, int y, int w, int h, float fraction, uint32_t fill_rgba, uint32_t back_rgba);
