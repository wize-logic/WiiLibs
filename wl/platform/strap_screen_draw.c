// SPDX-License-Identifier: MIT
// The wrist strap reminder drawn with the frame overlay's helpers: see strap_screen.h.
#include "wl/platform/overlay.h"
#include "wl/platform/strap_screen.h"

#define COLOR_BLACK 0x000000FFu
#define COLOR_TEXT 0xFFFFFFFFu
#define COLOR_WARNING 0xFFD25AFFu
#define COLOR_GOOD 0x58D26CFFu
#define COLOR_DIM 0x303848FFu

static void text_centered(uint32_t width, int y, int scale, uint32_t rgba, const char *text) {
    wl_overlay_text(((int)width - wl_overlay_text_width(scale, text)) / 2, y, scale, rgba, text);
}

void wl_strap_screen_draw(const WlStrapScreen *strap, uint32_t width, uint32_t height, uint64_t now_us) {
    int y = (int)height / 5;
    const char *line;

    wl_overlay_rect(0, 0, (int)width, (int)height, COLOR_BLACK);
    text_centered(width, y, 4, COLOR_WARNING, wl_strap_screen_title(strap));
    y += 90;
    for (int index = 0; (line = wl_strap_screen_line(strap, index)) != NULL; index++) {
        text_centered(width, y, 2, COLOR_TEXT, line);
        y += 36;
    }
    text_centered(width, (int)height - 90, 3, wl_strap_screen_ready(strap, now_us) ? COLOR_GOOD : COLOR_DIM,
                  wl_strap_screen_prompt(strap));
}
