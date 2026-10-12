// SPDX-License-Identifier: MIT
// The HOME menu and its notices drawn with the frame overlay's helpers: see home_menu.h. Text in
// overlay.h's small font.
#include <stdio.h>

#include "wl/platform/home_internal.h"
#include "wl/platform/home_menu.h"
#include "wl/platform/overlay.h"

#define COLOR_DIM 0x000000B8u
#define COLOR_PANEL 0x10182CE0u
#define COLOR_FRAME 0x7FA6FFFFu
#define COLOR_SELECTED 0x2B5FD8F0u
#define COLOR_TEXT 0xFFFFFFFFu
#define COLOR_GREY 0xB4B8C4FFu
#define COLOR_WARNING 0xFFD25AFFu
#define COLOR_GOOD 0x58D26CFFu
#define COLOR_LOW 0xE8504CFFu
#define COLOR_GAUGE_BACK 0x303848FFu

static void text_centered(uint32_t width, int y, int scale, uint32_t rgba, const char *text) {
    int x = ((int)width - wl_overlay_text_width(scale, text)) / 2;

    wl_overlay_text(x, y, scale, rgba, text);
}

// one row of a list: highlighted when chosen
static int draw_row(uint32_t width, int panel_x, int panel_w, int row_h, int y, bool chosen, const char *label) {
    if (chosen) {
        wl_overlay_rect(panel_x + 16, y - 6, panel_w - 32, row_h - 6, COLOR_SELECTED);
        wl_overlay_text(panel_x + 28, y + 2, 3, COLOR_TEXT, ">");
    }
    text_centered(width, y + 2, 3, chosen ? COLOR_TEXT : COLOR_GREY, label);
    return y + row_h;
}

void wl_home_menu_draw(const WlHomeMenu *menu, uint32_t width, uint32_t height) {
    int panel_w = 420;
    int panel_x = ((int)width - panel_w) / 2;
    int panel_y = 56;
    int row_h = 42;
    int y;
    char line[64];

    if (menu->state == WL_HOME_CLOSED) {
        return;
    }
    wl_overlay_rect(0, 0, (int)width, (int)height, COLOR_DIM);
    wl_overlay_rect(panel_x, panel_y, panel_w, (int)height - 2 * panel_y, COLOR_PANEL);
    wl_overlay_frame(panel_x, panel_y, panel_w, (int)height - 2 * panel_y, 2, COLOR_FRAME);
    text_centered(width, panel_y + 20, 4, COLOR_TEXT, menu->config.title);
    y = panel_y + 80;
    if (menu->state == WL_HOME_CONFIRM) {
        text_centered(width, y, 2, COLOR_WARNING, wl_home_menu_confirm_text(menu));
        y += 44;
        for (int index = 0; index < WL_HOME_CHOICES; index++) {
            y = draw_row(width, panel_x, panel_w, row_h, y, index == menu->selection,
                         wl_home_menu_choice_label(menu, index));
        }
    } else {
        for (int index = 0; index < menu->item_count; index++) {
            y = draw_row(width, panel_x, panel_w, row_h, y, index == menu->selection,
                         wl_home_menu_item_label(menu, menu->items[index]));
        }
    }

    // the Remotes' batteries
    y = (int)height - panel_y - 150;
    text_centered(width, y, 2, COLOR_GREY, menu->config.batteries_heading);
    y += 28;
    for (int port = 0; port < WL_HOME_PORTS; port++) {
        int gauge_x = panel_x + 150;
        float fraction = menu->battery[port] / 100.0f;

        if (!menu->present[port]) {
            continue;
        }
        wl_home_fill_template(menu->config.remote_label, port + 1, menu->battery[port], line, sizeof line);
        wl_overlay_text(panel_x + 24, y, 2, COLOR_TEXT, line);
        wl_overlay_gauge(gauge_x, y, 160, 14, fraction,
                         menu->battery[port] < menu->config.battery_low ? COLOR_LOW : COLOR_GOOD, COLOR_GAUGE_BACK);
        snprintf(line, sizeof line, "%d%%", menu->battery[port]);
        wl_overlay_text(gauge_x + 172, y, 2, COLOR_TEXT, line);
        y += 22;
    }
    text_centered(width, (int)height - panel_y - 30, 2, COLOR_GREY,
                  menu->state == WL_HOME_CONFIRM ? menu->config.hint_confirm : menu->config.hint_menu);
}

void wl_home_menu_draw_notices(const WlHomeMenu *menu, uint32_t width, uint64_t now_us) {
    char text[256];
    unsigned length = wl_home_menu_notice_text(menu, now_us, text, sizeof text);
    int box_w;
    int box_h;
    int y = 14;

    if (!length) {
        return;
    }
    // (the last newline is not a line)
    text[length - 1] = '\0';
    box_w = wl_overlay_text_width(2, text) + 32;
    box_h = wl_overlay_text_height(2, text) + 24;
    wl_overlay_rect(((int)width - box_w) / 2, y, box_w, box_h, COLOR_PANEL);
    wl_overlay_frame(((int)width - box_w) / 2, y, box_w, box_h, 2, COLOR_WARNING);
    wl_overlay_text(((int)width - box_w) / 2 + 16, y + 12, 2, COLOR_WARNING, text);
}
