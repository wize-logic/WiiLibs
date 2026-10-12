// SPDX-License-Identifier: MIT
// Drawing for a frame overlay: see overlay.h.
#include "wl/platform/overlay.h"

#include <string.h>

#include "wl/platform/platform.h"

#define GLYPH_ROWS 7
#define GLYPH_COLUMNS 5
#define CELL_WIDTH 6
#define CELL_HEIGHT 9
#define BATCH_QUADS 128u

// The glyphs, 7 rows of 5 pixels each (the leftmost pixel the highest of the low 5 bits).
static const struct {
    char c;
    uint8_t rows[GLYPH_ROWS];
} k_font[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {';', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'\'', {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
    {'>', {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10}},
    {'<', {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01}},
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
    {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'*', {0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00}},
};

static const uint8_t k_box[GLYPH_ROWS] = {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F};

static const uint8_t *find_glyph(char c) {
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    for (size_t i = 0; i < sizeof k_font / sizeof k_font[0]; i++) {
        if (k_font[i].c == c) {
            return k_font[i].rows;
        }
    }
    return NULL;
}

const uint8_t *wl_overlay_glyph(char c) {
    const uint8_t *rows = find_glyph(c);
    return rows != NULL ? rows : k_box;
}

bool wl_overlay_text_drawable(const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        if (*p != '\n' && find_glyph(*p) == NULL) {
            return false;
        }
    }
    return true;
}

// ---- the quads: collected, and sent in batches

static WlGxVertex g_quads[BATCH_QUADS * 4u];
static uint32_t g_count; // vertices

void wl_overlay_begin(void) {
    g_count = 0;
    wl_gx_set_cull(WL_GX_CULL_NONE);
    wl_gx_set_blend(WL_GX_BLEND_ALPHA);
    wl_gx_set_zmode(false, WL_GX_ALWAYS, false);
    wl_gx_set_alpha_test(WL_GX_ALWAYS, 0);
    wl_gx_set_recipe(WL_GX_RECIPE_VERTEX_COLOR);
}

void wl_overlay_end(void) {
    if (g_count != 0u) {
        wl_gx_draw(WL_GX_QUADS, g_quads, g_count);
        g_count = 0;
    }
}

void wl_overlay_rect(int x, int y, int w, int h, uint32_t rgba) {
    if (w <= 0 || h <= 0 || (rgba & 0xFFu) == 0u) {
        return;
    }
    if (g_count + 4u > sizeof g_quads / sizeof g_quads[0]) {
        wl_overlay_end();
    }
    WlGxVertex *v = &g_quads[g_count];
    const float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    // clockwise on a y-down canvas (GX's front face; culling is off anyway)
    v[0] = (WlGxVertex){x0, y0, 0.0f, rgba, 0.0f, 0.0f};
    v[1] = (WlGxVertex){x1, y0, 0.0f, rgba, 0.0f, 0.0f};
    v[2] = (WlGxVertex){x1, y1, 0.0f, rgba, 0.0f, 0.0f};
    v[3] = (WlGxVertex){x0, y1, 0.0f, rgba, 0.0f, 0.0f};
    g_count += 4u;
}

void wl_overlay_frame(int x, int y, int w, int h, int thickness, uint32_t rgba) {
    if (thickness * 2 > w || thickness * 2 > h) {
        wl_overlay_rect(x, y, w, h, rgba);
        return;
    }
    wl_overlay_rect(x, y, w, thickness, rgba);
    wl_overlay_rect(x, y + h - thickness, w, thickness, rgba);
    wl_overlay_rect(x, y + thickness, thickness, h - 2 * thickness, rgba);
    wl_overlay_rect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, rgba);
}

void wl_overlay_gauge(int x, int y, int w, int h, float fraction, uint32_t fill_rgba, uint32_t back_rgba) {
    if (!(fraction > 0.0f)) {
        fraction = 0.0f;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    wl_overlay_rect(x, y, w, h, back_rgba);
    wl_overlay_rect(x + 1, y + 1, (int)((float)(w - 2) * fraction), h - 2, fill_rgba);
}

// ---- text

static int clamp_scale(int scale) {
    return scale < 1 ? 1 : scale > 8 ? 8 : scale;
}

// the text's lines: its longest, in characters, and how many
static void measure(const char *text, int *columns, int *lines) {
    int longest = 0, current = 0, count = 1;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            count++;
            current = 0;
        } else if (++current > longest) {
            longest = current;
        }
    }
    *columns = longest;
    *lines = count;
}

int wl_overlay_text_width(int scale, const char *text) {
    int columns, lines;
    measure(text, &columns, &lines);
    return columns == 0 ? 0 : (columns * CELL_WIDTH - 1) * clamp_scale(scale);
}

int wl_overlay_text_height(int scale, const char *text) {
    int columns, lines;
    measure(text, &columns, &lines);
    return (lines * CELL_HEIGHT - 2) * clamp_scale(scale);
}

// one pass of the text in one colour at an offset: each row of each glyph as runs of pixels
static void draw_pass(int x, int y, int scale, uint32_t rgba, const char *text) {
    int cx = x, cy = y;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            cx = x;
            cy += CELL_HEIGHT * scale;
            continue;
        }
        const uint8_t *rows = wl_overlay_glyph(*p);
        for (int row = 0; row < GLYPH_ROWS; row++) {
            int column = 0;
            while (column < GLYPH_COLUMNS) {
                if ((rows[row] & (0x10u >> column)) == 0u) {
                    column++;
                    continue;
                }
                int run = 1;
                while (column + run < GLYPH_COLUMNS && (rows[row] & (0x10u >> (column + run))) != 0u) {
                    run++;
                }
                wl_overlay_rect(cx + column * scale, cy + row * scale, run * scale, scale, rgba);
                column += run;
            }
        }
        cx += CELL_WIDTH * scale;
    }
}

int wl_overlay_text(int x, int y, int scale, uint32_t rgba, const char *text) {
    scale = clamp_scale(scale);
    // the shadow: black at half the colour's opacity, one glyph pixel down and to the right
    const uint32_t shadow = (rgba & 0xFFu) / 2u;
    draw_pass(x + scale, y + scale, scale, shadow, text);
    draw_pass(x, y, scale, rgba, text);
    return wl_overlay_text_width(scale, text);
}
