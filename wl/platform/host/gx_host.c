// SPDX-License-Identifier: MIT
// Host stub of the GX layer (gx_host.h). Also the host's video info, which the renderer reads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/gx_direct.h"
#include "wl/platform/gx_shadow.h"
#include "wl/platform/gx_target_internal.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/host/gx_target_host.h"
#include "wl/platform/host/gx_renderer_host.h"

static WlHostGxState s;
static WlHostGxRenderer g_renderer;
static bool g_renderer_set;
static bool g_wide;
static WlWidescreen g_widescreen = WL_WIDESCREEN_PILLARBOX;
static bool g_validate = true;
static uint64_t g_copy_bytes; // (WlGxStats.efb_copy_bytes)

const WlHostGxState *wl_host_gx_state(void) {
    return &s;
}

// The renderer is the host's own stand-in for the console's GPU: what it allocates is kept out of
// the host heap's count and cap (mem_census.h's wl_platform_heap_exempt; weak, so that a program
// without the census does not get its malloc), which imitate the Wii's heap. So the renderer set
// is called through these, each exempting the call.
static WlHostGxRenderer g_inner;

void wl_platform_heap_exempt(bool on) __attribute__((weak));

static void exempt(bool on) {
    if (wl_platform_heap_exempt != NULL) {
        wl_platform_heap_exempt(on);
    }
}

static void r_frame_begin(void *user, uint32_t clear_rgba) {
    exempt(true);
    g_inner.frame_begin(user, clear_rgba);
    exempt(false);
}

static void r_frame_end(void *user, uint32_t width, uint32_t height) {
    exempt(true);
    g_inner.frame_end(user, width, height);
    exempt(false);
}

static void r_draw(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count) {
    exempt(true);
    g_inner.draw(user, prim, vertices, count);
    exempt(false);
}

static void r_copy(void *user, int x, int y, uint32_t w, uint32_t h, void *dst, unsigned format, bool clear) {
    exempt(true);
    g_inner.copy(user, x, y, w, h, dst, format, clear);
    exempt(false);
}

static void r_restore(void *user, const void *colour, const void *depth, int x, uint32_t w, uint32_t h) {
    exempt(true);
    g_inner.restore(user, colour, depth, x, w, h);
    exempt(false);
}

static void r_clear(void *user, bool colour, bool depth, uint32_t rgba, float z, int x, int y, int w, int h) {
    exempt(true);
    g_inner.clear(user, colour, depth, rgba, z, x, y, w, h);
    exempt(false);
}

static void r_squeeze(void *user, uint32_t back_w, uint32_t back_h, uint32_t x, uint32_t w) {
    exempt(true);
    g_inner.squeeze(user, back_w, back_h, x, w);
    exempt(false);
}

static uint32_t r_peek(void *user, int x, int y) {
    exempt(true);
    uint32_t v = g_inner.peek(user, x, y);
    exempt(false);
    return v;
}

static uint32_t r_peek_z(void *user, int x, int y) {
    exempt(true);
    uint32_t v = g_inner.peek_z(user, x, y);
    exempt(false);
    return v;
}

static void r_efb_alpha(void *user, bool on) {
    exempt(true);
    g_inner.efb_alpha(user, on);
    exempt(false);
}

static void r_display_rect(void *user, uint32_t src_x, uint32_t src_w, uint32_t dst_x) {
    exempt(true);
    g_inner.display_rect(user, src_x, src_w, dst_x);
    exempt(false);
}

static void r_texture_invalidate(void *user) {
    exempt(true);
    g_inner.texture_invalidate(user);
    exempt(false);
}

// the EFB's format as wl_gx_set_efb_alpha last chose it (kept across wl_gx_init, as the Wii's): a
// renderer set later (the play harness's screenshots attach the software GX mid-run) is told it
static bool g_efb_alpha;
static void display_rect_tell(void);

void wl_host_gx_set_renderer(const WlHostGxRenderer *renderer) {
    memset(&g_renderer, 0, sizeof g_renderer);
    if (renderer != NULL) {
        g_inner = *renderer;
        g_renderer.user = renderer->user;
#define WRAP(m) g_renderer.m = renderer->m != NULL ? r_##m : NULL
        WRAP(frame_begin);
        WRAP(frame_end);
        WRAP(draw);
        WRAP(copy);
        WRAP(restore);
        WRAP(clear);
        WRAP(squeeze);
        WRAP(peek);
        WRAP(peek_z);
        WRAP(efb_alpha);
        WRAP(texture_invalidate);
        WRAP(display_rect);
#undef WRAP
    }
    g_renderer_set = renderer != NULL;
    if (g_renderer_set && g_renderer.efb_alpha != NULL) {
        g_renderer.efb_alpha(g_renderer.user, g_efb_alpha);
    }
    display_rect_tell(); // (a renderer set mid-run: the display copy's rectangle as it is)
}

const WlHostGxRenderer *wl_host_gx_renderer(void) {
    return g_renderer_set ? &g_renderer : NULL;
}

// (a host texture object keeps its image's address in its opaque words)
const void *wl_host_gx_tex_image(const WlGxTexObj *tex) {
    const void *image;

    memcpy(&image, tex->opaque, sizeof image);
    return image;
}

void wl_host_gx_set_widescreen(bool widescreen) {
    g_wide = widescreen;
}

void wl_host_gx_clear_counters(void) {
    s.draw_calls = s.vertices = s.dl_calls = s.validation_errors = s.sync_calls = 0;
    s.tlut_loads = s.tex_binds = s.slot_loads = s.texmtx_loads = 0;
    s.begin_frames = s.end_frames = 0;
    s.ramp_binds = 0;
}

static bool check(bool cond) {
    if (!cond && g_validate) {
        s.validation_errors++;
    }
    return cond;
}

void wl_platform_video_info(WlVideoInfo *out) {
    memset(out, 0, sizeof *out);
    out->fb_width = 640;
    out->efb_height = 480;
    out->xfb_height = 480;
    out->refresh_mhz = 59940;
    out->widescreen = g_wide;
    out->picture_width = 640;
    if (g_wide && g_widescreen == WL_WIDESCREEN_PILLARBOX) {
        out->pillarbox = true;
        out->picture_x = 80;
        out->picture_width = 480;
    }
}

WlResult wl_platform_video_set_widescreen(WlWidescreen mode) {
    if (mode != WL_WIDESCREEN_PILLARBOX && mode != WL_WIDESCREEN_STRETCH) {
        return WL_ERR_ARG;
    }
    g_widescreen = mode;
    return WL_OK;
}

static uint32_t g_rate_hz;

void wl_platform_video_set_rate(uint32_t hz) {
    g_rate_hz = hz;
}

uint32_t wl_host_gx_rate(void) {
    return g_rate_hz;
}

void wl_platform_video_describe(char *out, size_t size) {
    WlVideoInfo vi;
    wl_platform_video_info(&vi);
    (void)snprintf(out, size, "host stub: %ux%u%s", (unsigned)vi.fb_width, (unsigned)vi.xfb_height,
                   vi.pillarbox ? ", pillarbox" : "");
}

// Video: a fixed 640x480 NTSC mode, no framebuffer memory; present counts one retrace per frame
// and the frame as shown at once (the stub GPU has always finished).
static uint32_t g_frames;
static uint32_t g_interval = 1;
static uint32_t g_frames_ended; // wl_gx_frame_index (not reset with the counters)
static bool g_overlap;
// the frames ended without overlap: the console's processor waits for the GPU at each
static uint32_t g_frame_waits;

WlResult wl_platform_video_init(void) {
    return WL_OK;
}

WlResult wl_platform_video_set_buffers(uint32_t count) {
    return count >= 2u && count <= WL_PRESENT_MAX_BUFFERS ? WL_OK : WL_ERR_ARG;
}

void wl_platform_video_set_interval(uint32_t retraces) {
    g_interval = retraces < 1u ? 1u : retraces > 15u ? 15u : retraces;
    g_rate_hz = 0;
}

void wl_platform_video_expected(WlVideoInfo *out) {
    wl_platform_video_info(out);
}

void *wl_platform_video_back_buffer(void) {
    return NULL;
}

uint32_t wl_platform_video_present(void) {
    g_frames++;
    return g_interval;
}

uint32_t wl_platform_video_present_when(WlPresentDone done, uint32_t done_arg) {
    (void)done;
    (void)done_arg;
    return wl_platform_video_present();
}

bool wl_platform_video_wait_shown(void) {
    return true;
}

void wl_platform_video_stats(WlVideoStats *out) {
    memset(out, 0, sizeof *out);
    out->presented = out->shown = g_frames;
    out->last_gap = g_frames > 1u ? g_interval : 0u;
    out->interval = g_interval;
    out->buffers = 2;
    out->retraces = g_frames * g_interval;
}

uint32_t wl_platform_video_frame_count(void) {
    return g_frames;
}

uint32_t wl_platform_video_shown_count(void) {
    return g_frames;
}

uint32_t wl_platform_video_retrace_count(void) {
    return g_frames * g_interval;
}

// The host has no display to keep time: the hook is kept and never called.
void wl_platform_video_set_retrace_hook(WlRetraceHook fn, void *user) {
    (void)fn;
    (void)user;
}

uint32_t wl_platform_video_dropped_count(void) {
    return 0;
}

void wl_gx_hang_where(unsigned slot, const char *name, uint32_t value) {
    (void)slot;
    (void)name;
    (void)value;
}

WlResult wl_gx_init(void) {
    memset(&s, 0, sizeof s);
    s.ready = true;
    // (WL_XB_WIDE: the console's 16:9 setting, as wl_host_gx_set_widescreen(true), for a title's host run)
    if (getenv("WL_XB_WIDE") != NULL) {
        g_wide = true;
    }
    return WL_OK;
}

void wl_gx_shutdown(void) {
    wl_gx_target_shutdown();
    s.ready = false;
}

bool wl_gx_ready(void) {
    return s.ready;
}

void wl_gx_set_validation(bool on) {
    g_validate = on;
}

void wl_gx_set_efb_alpha(bool on) {
    g_efb_alpha = on;
    if (g_renderer_set && g_renderer.efb_alpha != NULL) {
        g_renderer.efb_alpha(g_renderer.user, on);
    }
}

void wl_gx_stats(WlGxStats *out) {
    memset(out, 0, sizeof *out);
    out->frames = s.end_frames;
    out->draw_calls = s.draw_calls;
    out->vertices = s.vertices;
    out->dl_calls = s.dl_calls;
    out->validation_errors = s.validation_errors;
    out->tex_loads = s.tex_binds;
    out->tlut_loads = s.tlut_loads;
    out->tex_mtx_loads = s.texmtx_loads;
    out->gpu_waits = s.sync_calls + g_frame_waits;
    out->efb_copies = s.efb_copies + wl_host_gx_target_copies();
    out->efb_copy_bytes = g_copy_bytes;
}

bool wl_gx_sync(void) {
    s.sync_calls++;
    return true;
}

uint32_t wl_gx_fence(void) {
    static uint32_t fences;

    return ++fences != 0u ? fences : ++fences;
}

bool wl_gx_fence_passed(uint32_t fence) {
    (void)fence;
    return true;
}

bool wl_gx_fence_wait(uint32_t fence) {
    (void)fence;
    return true;
}

void wl_gx_set_frame_overlap(bool on) {
    g_overlap = on;
}

void wl_gx_set_wait_work(WlGxWaitWorkFn work, void *user) {
    (void)work, (void)user; // (the host's GPU is done when it is given a command: no wait to fill)
}

void wl_gx_set_wait_idle(WlGxWaitIdleFn idle, void *user) {
    (void)idle, (void)user; // (the same)
}

uint32_t wl_gx_frame_index(void) {
    return g_frames_ended;
}

bool wl_gx_frame_retired(uint32_t index) {
    return index < g_frames_ended; // (an ended frame: the stub GPU has finished it)
}

bool wl_gx_frame_wait(uint32_t index) {
    if (index >= g_frames_ended) {
        s.sync_calls++;
    }
    return true;
}

void wl_gx_invalidate_shadow(void) {
    wl_gx_shadow_forget();
    wl_gx_shadow_texture_forget(0xFFu); // (the console's: its maps too)
}

void wl_gx_reset_state(void) {
    s.viewport[2] = 640;
    s.viewport[3] = 480;
    s.scissor[0] = s.scissor[1] = 0;
    s.scissor[2] = 640;
    s.scissor[3] = 480;
    s.ortho = true;
    s.cull = WL_GX_CULL_NONE;
    s.blend = WL_GX_BLEND_OFF;
    s.z.test = true;
    s.z.func = WL_GX_LEQUAL;
    s.z.write = true;
    s.alpha_func = WL_GX_ALWAYS;
    s.alpha_ref = 0;
    s.recipe = WL_GX_RECIPE_VERTEX_COLOR;
    s.texmtx_on = false;
    s.konst = s.matcol = s.ambient = 0xFFFFFFFFu;
    s.light_count = 0;
    s.fog.mode = WL_GX_FOG_OFF;
    memset(s.modelview, 0, sizeof s.modelview);
    s.modelview[0] = s.modelview[5] = s.modelview[10] = 1.0f;
    WlGxLayout std = WL_GX_LAYOUT_STD;
    s.layout = std;
}

void wl_gx_begin_frame(uint32_t clear_rgba) {
    wl_gx_reset_state();
    s.clear_rgba = clear_rgba;
    s.begin_frames++;
    s.in_frame = true;
    wl_gx_target_frame_begun(clear_rgba);
    if (g_renderer_set && g_renderer.frame_begin != NULL) {
        g_renderer.frame_begin(g_renderer.user, clear_rgba);
    }
}

void wl_gx_ramp_bind(uint32_t texmap) {
    if (check(texmap < 8u)) {
        s.ramp_binds++;
        s.ramp_texmap = texmap;
        wl_host_gx_direct_ramp(texmap);
    }
}

static WlGxOverlayFn g_overlay;
static void *g_overlay_user;

void wl_gx_set_overlay(WlGxOverlayFn fn, void *user) {
    g_overlay_user = user;
    g_overlay = fn;
}

void wl_gx_shot_request(void *dst) {
    (void)dst; // (the host's screenshots are wl/gxsoft's)
}

bool wl_gx_shot_done(void) {
    return false;
}

// wl_gx_draw_narrow: the program draws the picture into the pillarbox's columns; with the display copy
// narrow (as the Wii's: its source the EFB's last picture_width columns, shown at picture_x), into those
static bool g_narrow, g_display_narrow;
static uint32_t g_pic_x;

static void display_rect_tell(void) {
    if (g_renderer_set && g_renderer.display_rect != NULL) {
        WlVideoInfo vi;
        wl_platform_video_info(&vi);
        g_renderer.display_rect(g_renderer.user, g_display_narrow ? g_pic_x : 0u,
                                g_display_narrow ? vi.picture_width : 0u, g_display_narrow ? vi.picture_x : 0u);
    }
}

bool wl_gx_draw_narrow(bool on) {
    WlVideoInfo vi;
    wl_platform_video_info(&vi);
    g_narrow = on && vi.pillarbox && vi.picture_width != 0u && vi.picture_width < vi.fb_width &&
               !wl_gx_bisect()->pillarbox;
    g_display_narrow = g_narrow && !wl_gx_bisect()->narrow_mid && vi.picture_x % 16u == 0u &&
                       vi.picture_width % 16u == 0u;
    g_pic_x = g_display_narrow ? vi.fb_width - vi.picture_width : vi.picture_x;
    wl_gx_target_set_picture(g_narrow ? g_pic_x : 0u, g_narrow ? vi.picture_width : 0u);
    wl_gx_target_set_bars_shown(!g_display_narrow);
    display_rect_tell();
    return g_narrow;
}

uint32_t wl_gx_end_frame(void) {
    wl_gx_target_frame_ending();
    if (g_overlay != NULL) {
        wl_gx_reset_state();
        if (g_narrow) {
            // (the overlay into the picture's columns, as the console's)
            uint32_t x, w;
            (void)wl_gx_target_picture(&x, &w);
            wl_gx_set_viewport((int)x, 0, (int)w, 480);
            wl_gx_set_scissor((int)x, 0, (int)w, 480);
        }
        g_overlay(g_overlay_user, 640u, 480u);
        wl_gx_reset_state();
    }
    {
        WlVideoInfo vi;
        wl_platform_video_info(&vi);
        if (vi.pillarbox && !g_narrow) {
            (void)wl_gx_target_squeeze(vi.picture_x, vi.picture_width);
        }
        if (g_renderer_set && g_renderer.frame_end != NULL) {
            g_renderer.frame_end(g_renderer.user, vi.fb_width, vi.efb_height);
        }
    }
    s.in_frame = false;
    s.end_frames++;
    g_frames_ended++;
    // (the stub GPU has finished every frame: overlap changes nothing but the waits counted)
    g_frame_waits += g_overlap ? 0u : 1u;
    return wl_platform_video_present();
}

uint32_t wl_gx_peek_rgba(int x, int y) {
    if (g_renderer_set && g_renderer.peek != NULL) {
        return g_renderer.peek(g_renderer.user, x, y);
    }
    return 0;
}

uint32_t wl_gx_peek_z(int x, int y) {
    if (g_renderer_set && g_renderer.peek_z != NULL) {
        return g_renderer.peek_z(g_renderer.user, x, y);
    }
    return 0xFFFFFFFFu;
}

void wl_gx_copy_efb_texture(int x, int y, int w, int h, void *dst, WlGxTexFormat fmt, bool clear) {
    (void)x;
    (void)y;
    if (!check(w > 0 && h > 0 && ((x | y | w | h) & 1) == 0 && ((uintptr_t)dst % 32u) == 0 && (unsigned)fmt < WL_GX_TEX_FORMAT_COUNT)) {
        return;
    }
    if (g_renderer_set && g_renderer.copy != NULL) {
        g_renderer.copy(g_renderer.user, x, y, (uint32_t)w, (uint32_t)h, dst, (unsigned)fmt, clear);
    } else {
        memset(dst, 0, wl_gx_tex_bytes(fmt, (uint32_t)w, (uint32_t)h)); // the stub draws nothing: an empty image
    }
    s.efb_copies++;
    s.efb_copy_clears += clear ? 1u : 0u;
    g_copy_bytes += wl_gx_tex_bytes(fmt, (uint32_t)w, (uint32_t)h);
    s.efb_copy_rect[0] = x;
    s.efb_copy_rect[1] = y;
    s.efb_copy_rect[2] = w;
    s.efb_copy_rect[3] = h;
}

void wl_gx_copy_efb_texture_again(int x, int y, int w, int h, void *dst, WlGxTexFormat fmt, bool clear) {
    wl_gx_copy_efb_texture(x, y, w, h, dst, fmt, clear); // (no processor cache to keep in step)
}

void wl_gx_set_viewport(int x, int y, int w, int h) {
    s.viewport[0] = x;
    s.viewport[1] = y;
    s.viewport[2] = w;
    s.viewport[3] = h;
}

void wl_gx_set_scissor(int x, int y, int w, int h) {
    s.scissor[0] = x;
    s.scissor[1] = y;
    s.scissor[2] = w;
    s.scissor[3] = h;
    s.scissor_calls++;
}

void wl_gx_set_ortho(float left, float top, float right, float bottom, float znear, float zfar) {
    s.ortho = true;
    float a[6] = {left, top, right, bottom, znear, zfar};
    memcpy(s.proj_args, a, sizeof a);
}

void wl_gx_set_perspective(float fovy, float aspect, float znear, float zfar) {
    s.ortho = false;
    float a[6] = {fovy, aspect, znear, zfar, 0, 0};
    memcpy(s.proj_args, a, sizeof a);
}

void wl_gx_set_modelview(const float m[12]) {
    static const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(s.modelview, m != NULL ? m : ident, sizeof s.modelview);
}

void wl_gx_load_matrix_slot(uint32_t slot, const float m[12]) {
    if (slot >= 1u && slot <= 9u && m != NULL) {
        memcpy(s.slots[slot - 1u], m, sizeof s.slots[0]);
        s.slot_loads++;
    } else {
        s.validation_errors++;
    }
}

void wl_gx_set_tex_matrix(const float m[6]) {
    if (m == NULL) {
        s.texmtx_on = false;
        return;
    }
    if (!s.texmtx_valid || memcmp(s.texmtx, m, sizeof s.texmtx) != 0) { // a matrix that comes back unchanged is not loaded again (as on the console)
        memcpy(s.texmtx, m, sizeof s.texmtx);
        s.texmtx_valid = true;
        s.texmtx_loads++;
    }
    s.texmtx_on = true;
}

void wl_gx_set_cull(WlGxCull cull) {
    if (check((unsigned)cull <= WL_GX_CULL_ALL)) {
        s.cull = cull;
    }
}

void wl_gx_set_blend(WlGxBlend blend) {
    if (check((unsigned)blend < WL_GX_BLEND_COUNT)) {
        s.blend = blend;
    }
}

void wl_gx_set_zmode(bool test, WlGxCompare func, bool write) {
    check(test || !write);
    s.z.test = test;
    s.z.func = func;
    s.z.write = write;
}

void wl_gx_set_alpha_test(WlGxCompare func, uint8_t ref) {
    s.alpha_func = func;
    s.alpha_ref = ref;
}

void wl_gx_set_color_write(bool rgb, bool alpha) {
    (void)rgb, (void)alpha;
}

void wl_gx_set_recipe(WlGxRecipe recipe) {
    if (check((unsigned)recipe < WL_GX_RECIPE_COUNT)) {
        s.recipe = recipe;
    }
}

void wl_gx_set_konst(uint32_t rgba) {
    s.konst = rgba;
}

void wl_gx_set_material_color(uint32_t rgba) {
    s.matcol = rgba;
}

void wl_gx_set_fog(WlGxFog mode, float start, float end, uint32_t rgba) {
    if (check((unsigned)mode < WL_GX_FOG_COUNT)) {
        s.fog.mode = mode;
        s.fog.start = start;
        s.fog.end = end;
        s.fog.rgba = rgba;
    }
}

void wl_gx_set_lighting(uint32_t ambient_rgba, const WlGxDirLight *lights, uint32_t count) {
    if (!check(count <= WL_GX_MAX_LIGHTS)) {
        return;
    }
    s.ambient = ambient_rgba;
    s.light_count = count;
    memset(s.lights, 0, sizeof s.lights);
    if (count > 0 && lights != NULL) {
        memcpy(s.lights, lights, count * sizeof lights[0]);
    }
}

WlResult wl_gx_set_layout(const WlGxLayout *layout) {
    if (layout == NULL) {
        return WL_ERR_ARG;
    }
    s.layout = *layout;
    return WL_OK;
}

void wl_gx_set_array(WlGxArray which, const void *base, uint32_t stride, uint32_t count) {
    (void)count;
    check((unsigned)which < WL_GX_ARRAY_COUNT && base != NULL && stride > 0 && ((uintptr_t)base & 31u) == 0);
}

void wl_gx_set_vertex_array(const WlGxVertex *verts, uint32_t count) {
    check(verts != NULL && count > 0 && ((uintptr_t)verts & 31u) == 0);
}

static WlHostGxDraw *g_rec;
static uint32_t g_rec_cap, g_rec_n;

void wl_host_gx_record(WlHostGxDraw *buf, uint32_t cap) {
    g_rec = buf;
    g_rec_cap = cap;
    g_rec_n = 0;
}

uint32_t wl_host_gx_recorded(void) {
    return g_rec_n;
}

void wl_gx_draw(WlGxPrim prim, const WlGxVertex *verts, uint32_t count) {
    if (g_rec != NULL) {
        if (g_rec_n < g_rec_cap && count > 0) {
            WlHostGxDraw *d = &g_rec[g_rec_n];
            memset(d, 0, sizeof *d);
            d->prim = (uint8_t)prim;
            d->count = count;
            d->x0 = d->x1 = verts[0].x;
            d->y0 = d->y1 = verts[0].y;
            d->u0 = d->u1 = verts[0].u;
            d->v0 = d->v1 = verts[0].v;
            for (uint32_t i = 1; i < count; i++) {
                if (verts[i].x < d->x0) d->x0 = verts[i].x;
                if (verts[i].x > d->x1) d->x1 = verts[i].x;
                if (verts[i].y < d->y0) d->y0 = verts[i].y;
                if (verts[i].y > d->y1) d->y1 = verts[i].y;
                if (verts[i].u < d->u0) d->u0 = verts[i].u;
                if (verts[i].u > d->u1) d->u1 = verts[i].u;
                if (verts[i].v < d->v0) d->v0 = verts[i].v;
                if (verts[i].v > d->v1) d->v1 = verts[i].v;
            }
            d->rgba = verts[0].rgba;
            d->recipe = s.recipe;
            d->tex = s.tex;
            d->konst = s.konst;
            memcpy(d->scissor, s.scissor, sizeof d->scissor);
        }
        g_rec_n++;
    }
    (void)prim;
    s.draw_calls++;
    s.vertices += count;
}

void wl_gx_draw_indexed(WlGxPrim prim, const uint16_t *indices, uint32_t count) {
    (void)prim, (void)indices;
    s.draw_calls++;
    s.vertices += count;
}

WlResult wl_gx_dl_begin(WlGxDisplayList *dl, void *buf, uint32_t capacity, const WlGxLayout *layout) {
    (void)capacity;
    memset(dl, 0, sizeof *dl);
    dl->data = buf;
    dl->layout = *layout;
    return WL_OK;
}

WlResult wl_gx_dl_end(WlGxDisplayList *dl) {
    dl->size = 32;
    return WL_OK;
}

WlResult wl_gx_dl_init(WlGxDisplayList *dl, const void *data, uint32_t size, uint32_t vertices, const WlGxLayout *layout) {
    dl->data = data;
    dl->size = size;
    dl->vertices = vertices;
    dl->layout = *layout;
    return WL_OK;
}

void wl_gx_dl_call(const WlGxDisplayList *dl) {
    s.layout = dl->layout;
    s.draw_calls++;
    s.dl_calls++;
    s.vertices += dl->vertices;
}

WlResult wl_gx_tex_init_ex(WlGxTexObj *tex, const WlGxTexDesc *d) {
    if (tex == NULL || d == NULL || d->image == NULL || d->format >= WL_GX_TEX_FORMAT_COUNT) {
        return WL_ERR_ARG;
    }
    memset(tex, 0, sizeof *tex);
    memcpy(tex->opaque, &d->image, sizeof d->image);
    tex->sampler = d->sampler;
    tex->width = d->width;
    tex->height = d->height;
    tex->format = d->format;
    tex->mips = d->mips == 0 ? 1u : d->mips;
    tex->tlut = d->tlut;
    return WL_OK;
}

WlResult wl_gx_tex_init(WlGxTexObj *tex, const void *image, uint32_t width, uint32_t height, WlGxTexFormat fmt,
                        bool repeat, bool linear) {
    WlGxTexDesc d;
    memset(&d, 0, sizeof d);
    d.image = image;
    d.width = (uint16_t)width;
    d.height = (uint16_t)height;
    d.format = (uint8_t)fmt;
    d.mips = 1;
    d.sampler.wrap_s = d.sampler.wrap_t = repeat ? (uint8_t)WL_GX_WRAP_REPEAT : (uint8_t)WL_GX_WRAP_CLAMP;
    d.sampler.min_filter = d.sampler.mag_filter = linear ? (uint8_t)WL_GX_FILTER_LINEAR : (uint8_t)WL_GX_FILTER_NEAREST;
    return wl_gx_tex_init_ex(tex, &d);
}

WlResult wl_gx_tlut_init(WlGxTlut *tlut, const void *entries, WlGxTlutFormat fmt, uint16_t count) {
    if (tlut == NULL || entries == NULL || count == 0) {
        return WL_ERR_ARG;
    }
    memset(tlut, 0, sizeof *tlut);
    tlut->entries = entries;
    tlut->count = count;
    tlut->format = (uint8_t)fmt;
    return WL_OK;
}

void wl_gx_tlut_load(const WlGxTlut *tlut, uint32_t slot) {
    (void)slot;
    s.tlut = tlut;
    s.tlut_loads++;
}

void wl_gx_tlut_invalidate(void) {}

void wl_gx_set_filter_override(WlGxFilterOverride mode) {
    s.filter_override = mode;
}

WlGxFilterOverride wl_gx_get_filter_override(void) {
    return s.filter_override;
}

void wl_gx_tex_bind_ex(const WlGxTexObj *tex, const WlGxSampler *sampler, const WlGxTlut *tlut) {
    if (!check(tex != NULL)) {
        return;
    }
    const WlGxSampler *sm = sampler != NULL ? sampler : &tex->sampler;
    wl_gx_shadow_texture_forget(1u); // (map 0, behind the direct layer's shadow as on the console)
    wl_gx_resolve_filters(sm, tex->mips, s.filter_override, &s.tex_min, &s.tex_mag);
    s.tex_wrap_s = sm->wrap_s;
    s.tex_wrap_t = sm->wrap_t;
    s.tex_bound = true;
    s.tex = tex;
    s.tex_binds++;
    if (tlut != NULL) {
        wl_gx_tlut_load(tlut, tex->tlut);
    }
}

void wl_gx_tex_bind(const WlGxTexObj *tex) {
    wl_gx_tex_bind_ex(tex, NULL, NULL);
}

void wl_gx_tex_invalidate(void) {
    if (g_renderer_set && g_renderer.texture_invalidate != NULL) {
        g_renderer.texture_invalidate(g_renderer.user);
    }
}
