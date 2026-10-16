// SPDX-License-Identifier: MIT
// GX bootstrap and state shadow: the 1 MiB command FIFO, the default GX state, setters that drop
// redundant writes, debug validation and the frame copy to the XFB. Declarations are in
// platform.h. Only this directory may include <ogc/*.h>.
//
// Rules this file enforces for the code above it:
//  - the FIFO lives in WL_TAG_GXFIFO (MEM1, 32-byte aligned);
//  - every GPU-read buffer reaches it through wl_gpu_buffer_commit (textures and vertex arrays
//    are only referenced here, never copied; their alignment is validated);
//  - GX state is changed through the shadowed setters, so a repeat costs nothing.
// Also: vertex layouts and display lists, all texture formats with TLUTs and mips, the
// per-sampler state with a global filter override, the TEV recipes (tev.c builds them; konst,
// lighting, fog).
#include <ogc/cache.h>
#include <ogc/gu.h>
#include <ogc/gx.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/video.h>
#include <stddef.h>
#include <string.h>

#include "wl/platform/gx_shadow.h"
#include "wl/platform/gx_target_internal.h"
#include "wl/platform/hwcount.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "wl/platform/timebase.h"
#include "wl/platform/tev.h"
#include "wl/platform/wii/gx_wii_internal.h"

#define TAG "GX"
#define WAIT_TIMEOUT_US 250000u
#define FRAME_FENCES 8u // the end-of-frame tokens of the last frames (wl_gx_frame_retired)
#define MAX_Z24 0x00FFFFFFu
#define EFB_MAX_HEIGHT 528u // the EFB's rows, whatever the video mode shows of them (a copy may reach below the picture)

_Static_assert(sizeof(GXTexObj) == sizeof(((WlGxTexObj *)0)->opaque), "WlGxTexObj must hold a GXTexObj");
_Static_assert(sizeof(GXTlutObj) == sizeof(((WlGxTlut *)0)->opaque), "WlGxTlut must hold a GXTlutObj");
_Static_assert(sizeof(WlGxVertex) == 24, "WlGxVertex is the array stride");
_Static_assert(sizeof(WlGxLayout) == 8, "WlGxLayout is compared bytewise");
_Static_assert((int)WL_GX_FILTER_LINEAR_MIP_LINEAR == GX_LIN_MIP_LIN && (int)WL_GX_FILTER_NEAREST_MIP_NEAREST == GX_NEAR_MIP_NEAR,
               "WlGxFilter follows the hardware order");

// Which shadow fields hold what the GPU really has.
enum {
    K_VIEWPORT = 1u << 0,
    K_SCISSOR = 1u << 1,
    K_PROJ = 1u << 2,
    K_MODELVIEW = 1u << 3,
    K_CULL = 1u << 4,
    K_BLEND = 1u << 5,
    K_ZMODE = 1u << 6,
    K_ALPHA = 1u << 7,
    K_CWRITE = 1u << 8,
    K_RECIPE = 1u << 9,
    K_TEX = 1u << 10,
    K_LAYOUT = 1u << 11,
    K_KONST = 1u << 12,
    K_MATCOL = 1u << 13,
    K_FOG = 1u << 14,
    K_LIGHTS = 1u << 15,
    K_CHAN = 1u << 16,
    K_TEXMTX = 1u << 17,
    K_TEXGEN = 1u << 18
};

#define LIGHT_DISTANCE 100000.0f // directional lights are point lights this far away (view space)
#define TLUT_SLOTS 16u

typedef struct {
    uint32_t mode, ortho;
    float start, end, znear, zfar;
    uint32_t rgba;
} FogKey;

typedef struct {
    uint32_t lit, mask, mat_reg;
} ChanKey;

typedef struct {
    const void *data;
    uint16_t count;
    uint8_t format;
    bool valid;
} TlutSlot;

static struct {
    bool ready;
    bool validate;
    bool efb_alpha; // RGBA6_Z24: a destination alpha, at 6 bits a channel
    void *fifo;
    uint32_t fifo_bytes;
    uint32_t fifo_peak_logged; // the highest 64 KiB step of the peak fill the log has been told
    uint32_t fb_w, efb_h;
    uint32_t pillar_x, pillar_w; // the picture narrowed into these columns by end_frame (0: not)
    WlGxOverlayFn overlay;
    void *overlay_user;
    uint32_t known;

    int viewport[4];
    int scissor[4];
    f32 proj[16];
    u8 proj_type;
    float proj_near, proj_far;
    f32 modelview[12];
    bool nrm_valid; // the normal matrix in slot 0 matches modelview
    WlGxCull cull;
    WlGxBlend blend;
    struct {
        bool test;
        WlGxCompare func;
        bool write;
    } z;
    struct {
        WlGxCompare func;
        uint8_t ref;
    } alpha;
    struct {
        bool rgb, a;
    } cwrite;
    WlGxRecipe recipe;
    GXTexObj tex;
    bool tex_bound;
    uint8_t tex_format;            // WlGxTexFormat of the bound texture
    WlGxFilterOverride filter_override;
    TlutSlot tlut[TLUT_SLOTS];
    WlGxLayout hw_layout;          // what the GPU has (shadow K_LAYOUT)
    WlGxLayout user_layout;        // wl_gx_set_layout
    bool array_set[WL_GX_ARRAY_COUNT];
    uint32_t array_count[WL_GX_ARRAY_COUNT];
    uint32_t konst, matcol;
    bool texmtx_on;                // texture coordinate 0 goes through TEXMTX0 (wl_gx_set_tex_matrix)
    float texmtx[6];
    struct {
        WlGxFog mode;
        float start, end;
        uint32_t rgba;
    } fog;       // wanted
    FogKey fog_hw; // what the GPU has (shadow K_FOG)
    uint32_t ambient;
    uint32_t light_count;
    WlGxDirLight lights[WL_GX_MAX_LIGHTS];
    ChanKey chan;
    bool dl_active;
    WlGxDisplayList *dl;

    uint32_t clear_next; // colour the EFB holds after the last copy-clear
    uint32_t clear_frame;
    bool in_frame;
    bool dirty; // GX commands submitted since the last sync
    bool overlap; // wl_gx_set_frame_overlap: end_frame does not wait for the GPU
    uint32_t token; // the last draw-sync token sent (the GPU's token register holds its low 16 bits)
    uint32_t frame_token[FRAME_FENCES]; // the token after frame n's display copy, at n % FRAME_FENCES
    WlGxStats stats;
} g;

static void verr(const char *what) {
    g.stats.validation_errors++;
    WL_LOG_E(TAG, "validation: %s", what);
}

#define VCHECK(cond, what)                                                                         \
    (((cond) || !g.validate) ? true : (verr(what), false))

static uint64_t now_us(void) {
    return wl_timebase_us(wl_timebase_read());
}

// Counts the setter call and says whether the GPU must be told (shadow unknown or different).
static bool need(uint32_t bit, bool same) {
    if (g.dl_active) {
        verr("state change while recording a display list");
        return false;
    }
    g.stats.state_calls++;
    if (same && (g.known & bit) != 0) {
        return false;
    }
    g.known |= bit;
    g.stats.state_writes++;
    wl_gx_shadow_forget(); // (the direct layer's and the TEV's shadow: GX set behind them)
    return true;
}

static GXColor unpack(uint32_t rgba) {
    GXColor c = {(u8)(rgba >> 24), (u8)(rgba >> 16), (u8)(rgba >> 8), (u8)rgba};
    return c;
}

// ---- setup ------------------------------------------------------------------------------------

static void setup_copy(GXRModeObj *rm) {
    f32 ys = GX_GetYScaleFactor(rm->efbHeight, rm->xfbHeight);
    u32 xfb_h = GX_SetDispCopyYScale(ys);
    GX_SetDispCopySrc(0, 0, rm->fbWidth, rm->efbHeight);
    GX_SetDispCopyDst(rm->fbWidth, (u16)xfb_h);
    GX_SetCopyFilter(rm->aa, rm->sample_pattern, GX_TRUE, rm->vfilter);
    GX_SetFieldMode(rm->field_rendering, (rm->viHeight == 2 * rm->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(rm->aa ? GX_PF_RGB565_Z16 : g.efb_alpha ? GX_PF_RGBA6_Z24 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
}

void wl_gx_set_efb_alpha(bool on) {
    g.efb_alpha = on;
    if (g.ready) {
        GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
        if (rm != NULL && !rm->aa)
            GX_SetPixelFmt(on ? GX_PF_RGBA6_Z24 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    }
}

WlResult wl_gx_init(void) {
    if (g.ready) {
        return WL_ERR_STATE;
    }
    WlVideoInfo vi;
    wl_platform_video_info(&vi);
    if (vi.fb_width == 0) {
        WL_LOG_E(TAG, "video not initialised");
        return WL_ERR_STATE;
    }
    if (!wl_mem_ready()) {
        WL_LOG_E(TAG, "memory system not initialised");
        return WL_ERR_STATE;
    }
    uint32_t fifo_bytes = wl_gx_fifo_bytes();
    void *fifo = fifo_bytes != 0u ? WL_MEM_ALLOC_GPU(WL_TAG_GXFIFO, fifo_bytes) : NULL;
    if (fifo == NULL) {
        WL_LOG_E(TAG, "cannot allocate the %u byte FIFO", (unsigned)fifo_bytes);
        return WL_ERR_NOMEM;
    }
    memset(fifo, 0, fifo_bytes);
    wl_gpu_buffer_commit(fifo, fifo_bytes);

    bool efb_alpha = g.efb_alpha; // (set before init: kept)
    memset(&g, 0, sizeof g);
    g.efb_alpha = efb_alpha;
#ifndef NDEBUG
    g.validate = true;
#endif
    g.fifo = fifo;
    g.fifo_bytes = fifo_bytes;
    wl_hwcount_set_fifo_size(fifo_bytes);
    g.fb_w = vi.fb_width;
    g.efb_h = vi.efb_height;
    if (vi.pillarbox && vi.picture_width != 0u && vi.picture_width < vi.fb_width) {
        g.pillar_x = vi.picture_x;
        g.pillar_w = vi.picture_width;
    }

    GX_Init(fifo, fifo_bytes);
    // The mode object is the one video init selected (a pure function of SYSCONF).
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    WL_ASSERT(rm != NULL && rm->fbWidth == vi.fb_width && rm->efbHeight == vi.efb_height);
    setup_copy(rm);

    // Vertex layouts are applied on demand (wl_gx_set_layout, VAT format 0). Channel 0 is configured by the
    // recipe and the layout; channel 1 is never used.
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_InvVtxCache();
    GX_InvalidateTexAll();

    g.clear_next = 0x000000FFu;
    g.clear_frame = g.clear_next;
    GX_SetCopyClear(unpack(g.clear_next), MAX_Z24);
    g.ready = true;
    wl_gx_reset_state();
    // Start from a cleared EFB (GX_Init leaves it undefined): copy-clear into the back XFB. The
    // sync's token is the first one, waited for exactly: from then on the token register holds
    // the last token passed, and older tokens compare as passed (token_passed).
    GX_CopyDisp(wl_platform_video_back_buffer(), GX_TRUE);
    g.dirty = true;
    (void)wl_gx_sync();
    wl_gx_wii_hang_init();
    WL_LOG_I(TAG, "up: FIFO %u bytes at %p, EFB %ux%u", (unsigned)fifo_bytes, fifo,
             (unsigned)g.fb_w, (unsigned)g.efb_h);
    return WL_OK;
}

void wl_gx_shutdown(void) {
    if (!g.ready) {
        return;
    }
    (void)wl_gx_sync();
    wl_hwcount_stop();
    wl_gx_target_shutdown();
    WL_LOG_I(TAG, "down: FIFO peak fill %u of %u bytes, %u samples above the high-water mark",
             (unsigned)g.stats.fifo_peak, (unsigned)g.fifo_bytes, (unsigned)g.stats.fifo_hiwater);
    g.ready = false;
    if (wl_mem_ready()) {
        wl_mem_free(g.fifo);
    }
    g.fifo = NULL;
}

bool wl_gx_ready(void) {
    return g.ready;
}

void wl_gx_set_validation(bool on) {
    g.validate = on;
}

void wl_gx_stats(WlGxStats *out) {
    *out = g.stats;
}

void wl_gx_invalidate_shadow(void) {
    g.known = 0;
    wl_gx_shadow_forget();
    wl_gx_wii_texmaps_forget(0xFFu);
    g.nrm_valid = false;
    memset(g.tlut, 0, sizeof g.tlut);
}

// ---- sync and frames --------------------------------------------------------------------------

static void note_wait(uint64_t t0) {
    uint64_t d = now_us() - t0;
    uint32_t us = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
    if (us > g.stats.gpu_wait_us_max) {
        g.stats.gpu_wait_us_max = us;
    }
}

// Whether the GPU has passed token `t` (sent no more than 32767 tokens before the last one). Safe in an
// interrupt (one register read): the display queue polls it at each retrace.
static bool token_passed(uint32_t t) {
    return (uint16_t)(GX_GetDrawSync() - (uint16_t)t) < 0x8000u;
}

static uint32_t send_token(void) {
    g.token++;
    GX_SetDrawSync((u16)g.token);
    GX_Flush();
    wl_gx_wii_hang_event('S', g.token, 0);
    return g.token;
}

void wl_gx_wii_hang_state(uint32_t *token_sent, uint32_t *frames_ended, bool *in_frame) {
    *token_sent = g.token;
    *frames_ended = g.stats.frames;
    *in_frame = g.in_frame;
}

// libogc suspends the thread GX_Init ran on at the FIFO's high-water mark, not the one writing: a
// title whose frames another thread draws (a recompiled title's guest thread) would otherwise
// write on past the GPU's read pointer, over commands it has not read (a GPU hang), while the
// thread that started GX is the one stopped.
void wl_gx_wii_own_thread(void) {
    lwp_t self = LWP_GetSelf();
    if (GX_GetCurrentGXThread() != self) {
        (void)GX_SetCurrentGXThread();
    }
}

static bool wait_token(uint32_t t) {
    uint64_t t0 = now_us();
    bool ok = true;
    g.stats.gpu_waits++;
    wl_hwcount_wait_begin(WL_HWWAIT_GPU); // (the processor's counters leave the wait out)
    // (the first token after init is waited for exactly: the register held anything before it)
    while (g.token == 1u ? GX_GetDrawSync() != 1u : !token_passed(t)) {
        if (now_us() - t0 > WAIT_TIMEOUT_US) {
            ok = false;
            g.stats.gpu_timeouts++;
            WL_LOG_E(TAG, "GPU did not reach token %u within %u us", (unsigned)(t & 0xFFFFu), WAIT_TIMEOUT_US);
            break;
        }
    }
    note_wait(t0);
    wl_hwcount_wait_end(WL_HWWAIT_GPU);
    return ok;
}

// A draw-sync token for hwcount.h's timeline: the GPU stamps the time it passes it.
uint32_t wl_gx_wii_mark_token(void) {
    return g.ready ? send_token() : 0u;
}

bool wl_gx_sync(void) {
    if (!g.ready) {
        return false;
    }
    bool ok = wait_token(send_token());
    g.dirty = false;
    return ok;
}

void wl_gx_set_frame_overlap(bool on) {
    g.overlap = on;
}

uint32_t wl_gx_frame_index(void) {
    return g.stats.frames;
}

bool wl_gx_frame_retired(uint32_t index) {
    uint32_t ended = g.stats.frames;
    if (index >= ended) {
        return false; // (not ended: its commands are still being given)
    }
    return ended - index > FRAME_FENCES || token_passed(g.frame_token[index % FRAME_FENCES]);
}

bool wl_gx_frame_wait(uint32_t index) {
    if (!g.ready || wl_gx_frame_retired(index)) {
        return true;
    }
    if (index >= g.stats.frames) {
        return wl_gx_sync(); // (the frame being built: everything given so far)
    }
    return wait_token(g.frame_token[index % FRAME_FENCES]);
}

static void clear_efb(uint32_t rgba) {
    GX_SetCopyClear(unpack(rgba), MAX_Z24);
    GX_CopyDisp(wl_platform_video_back_buffer(), GX_TRUE); // the copy is overwritten by end_frame
    g.clear_next = rgba;
    g.dirty = true;
}

void wl_gx_begin_frame(uint32_t clear_rgba) {
    WL_ASSERT(g.ready);
    VCHECK(!g.in_frame, "begin_frame inside a frame");
    wl_hwcount_frame_begin(); // (closes the last frame's window, opens this one's: the counters' selections queue here)
    wl_gx_wii_own_thread();
    wl_gx_wii_hang_event('F', g.stats.frames, 0);
    wl_gx_reset_state();
    if (clear_rgba != g.clear_next) {
        clear_efb(clear_rgba);
    }
    g.clear_frame = clear_rgba;
    g.in_frame = true;
    wl_gx_target_frame_begun();
}

void wl_gx_set_overlay(WlGxOverlayFn fn, void *user) {
    g.overlay_user = user;
    g.overlay = fn;
}

uint32_t wl_gx_end_frame(void) {
    WL_ASSERT(g.ready);
    VCHECK(g.in_frame, "end_frame outside a frame");
    wl_gx_target_frame_ending(); // (the back buffer has the EFB for the display copy)
    if (g.overlay != NULL) {
        // (the program's overlay over the finished image: the state reset before it and after)
        wl_gx_reset_state();
        g.overlay(g.overlay_user, g.fb_w, g.efb_h);
        wl_gx_reset_state();
    }
    if (g.pillar_w != 0u) {
        // (a 4:3 picture on a 16:9 screen: narrowed into the middle three quarters, with the bars black; the
        // video interface then widens the framebuffer to the screen)
        (void)wl_gx_target_squeeze(g.pillar_x, g.pillar_w);
    }
    // FIFO health sample before the GPU drains it.
    GXFifoObj f;
    GX_GetGPFifo(&f);
    u32 fill = GX_GetFifoCount(&f);
    if (fill > g.stats.fifo_peak) {
        g.stats.fifo_peak = fill;
        // (a new peak is told to the log by 64 KiB steps: how close the FIFO comes to its size)
        if (fill / 65536u > g.fifo_peak_logged) {
            g.fifo_peak_logged = fill / 65536u;
            WL_LOG_I(TAG, "FIFO fill peaked at %u of %u bytes", (unsigned)fill, (unsigned)g.fifo_bytes);
        }
    }
    u8 overhi, underlow, ridle, cidle, brk;
    GX_GetGPStatus(&overhi, &underlow, &ridle, &cidle, &brk);
    if (overhi != 0) {
        g.stats.fifo_hiwater++;
        VCHECK(false, "FIFO above its high-water mark (CPU is outrunning the GPU)");
    }
    // The copy-clear writes colour, alpha and depth only where the update enables are on: a frame that ends in a pass with
    // depth writes off (transparent, UI) would otherwise leave this frame's depth for the next one to fail against.
    wl_gx_set_zmode(true, WL_GX_LEQUAL, true);
    wl_gx_set_color_write(true, true);
    GX_SetCopyClear(unpack(g.clear_frame), MAX_Z24);
    // (the back buffer waits only when every XFB is on the screen or waiting to be: the GPU is
    // given all the frame's draws first, to work on meanwhile)
    GX_Flush();
    void *xfb = wl_platform_video_back_buffer();
    GX_CopyDisp(xfb, GX_TRUE); // clears the EFB for the next frame
    g.clear_next = g.clear_frame;
    // The copy's token: the display queue shows the XFB once the GPU has passed it (polled at each
    // retrace), so neither the CPU nor the GPU waits for the display. Without overlap the CPU waits
    // here for the GPU to finish the frame (the caller may then reuse what it drew from); with it,
    // the next frame's commands queue behind the copy at once.
    uint32_t t = send_token();
    g.frame_token[g.stats.frames % FRAME_FENCES] = t;
    wl_gx_wii_hang_event('E', g.stats.frames, t);
    if (g.overlap) {
        // (the GPU at most this one frame behind: the frame before it retired, as a double-buffered
        // frame arena needs, whatever the number of XFBs)
        if (g.stats.frames != 0u && !wl_gx_frame_retired(g.stats.frames - 1u)) {
            (void)wait_token(g.frame_token[(g.stats.frames - 1u) % FRAME_FENCES]);
        }
        g.dirty = true;
    } else {
        (void)wait_token(t);
        g.dirty = false;
    }
    g.in_frame = false;
    g.stats.frames++;
    wl_hwcount_frame_end();
    return wl_platform_video_present_when(token_passed, t);
}

uint32_t wl_gx_peek_rgba(int x, int y) {
    WL_ASSERT(g.ready);
    if (!VCHECK(x >= 0 && y >= 0 && (uint32_t)x < g.fb_w && (uint32_t)y < g.efb_h, "peek outside the EFB")) {
        return 0;
    }
    if (g.dirty) {
        (void)wl_gx_sync();
    }
    GXColor c;
    GX_PeekARGB((u16)x, (u16)y, &c);
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}

static void copy_efb_texture(int x, int y, int w, int h, void *dst, WlGxTexFormat fmt, bool clear, bool invalidate) {
    WL_ASSERT(g.ready);
    // I4 and I8 copy the red channel straight (0 to 255 stay 0 to 255; the luminance copy would map black to 16 and white to 235)
    static const u8 tf[WL_GX_TEX_FORMAT_COUNT] = {GX_CTF_R4, GX_CTF_R8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565,
                                                  GX_TF_RGB5A3, GX_TF_RGBA8, 0xFF, 0xFF, 0xFF};
    if (!VCHECK(x >= 0 && y >= 0 && w > 0 && h > 0 && ((x | y | w | h) & 1) == 0 && (uint32_t)(x + w) <= g.fb_w &&
                    (uint32_t)(y + h) <= EFB_MAX_HEIGHT && (uintptr_t)dst % 32u == 0 &&
                    (unsigned)fmt < WL_GX_TEX_FORMAT_COUNT && tf[fmt] != 0xFF,
                "bad EFB texture copy")) {
        return;
    }
    if (invalidate) {
        wl_gpu_buffer_invalidate(dst, wl_gx_tex_bytes(fmt, (uint32_t)w, (uint32_t)h));
    }
    if (clear) {
        wl_gx_set_zmode(true, WL_GX_LEQUAL, true);
        wl_gx_set_color_write(true, true);
        GX_SetCopyClear(unpack(g.clear_frame), MAX_Z24);
    }
    wl_gx_wii_copy_filter(false);
    GX_SetTexCopySrc((u16)x, (u16)y, (u16)w, (u16)h);
    GX_SetTexCopyDst((u16)w, (u16)h, tf[fmt], GX_FALSE);
    GX_CopyTex(dst, clear ? GX_TRUE : GX_FALSE);
    GX_PixModeSync();
    wl_gx_wii_hang_event('C', (uint32_t)(uintptr_t)dst, (uint32_t)w << 16 | (uint32_t)h);
    wl_gx_wii_copy_filter(true);
    GX_InvalidateTexAll();
    g.stats.efb_copies++;
    g.dirty = true;
}

void wl_gx_copy_efb_texture(int x, int y, int w, int h, void *dst, WlGxTexFormat fmt, bool clear) {
    copy_efb_texture(x, y, w, h, dst, fmt, clear, true);
}

void wl_gx_copy_efb_texture_again(int x, int y, int w, int h, void *dst, WlGxTexFormat fmt, bool clear) {
    copy_efb_texture(x, y, w, h, dst, fmt, clear, false);
}

// The copies to textures are unfiltered: the display copy's vertical (deflicker) filter, which stays set for it,
// would blur them.
void wl_gx_wii_copy_filter(bool display) {
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    if (display && rm != NULL) {
        GX_SetCopyFilter(rm->aa, rm->sample_pattern, GX_TRUE, rm->vfilter);
    } else {
        GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
    }
}

void wl_gx_wii_note_commands(void) {
    g.dirty = true;
}

void wl_gx_wii_note_efb_copy(void) {
    g.stats.efb_copies++;
}

// ---- shadowed state ---------------------------------------------------------------------------

void wl_gx_set_viewport(int x, int y, int w, int h) {
    int v[4] = {x, y, w, h};
    if (need(K_VIEWPORT, memcmp(v, g.viewport, sizeof v) == 0)) {
        memcpy(g.viewport, v, sizeof v);
        GX_SetViewport((f32)x, (f32)y, (f32)w, (f32)h, 0.0f, 1.0f);
        g.dirty = true;
    }
}

void wl_gx_set_scissor(int x, int y, int w, int h) {
    int v[4] = {x, y, w, h};
    if (!VCHECK(x >= 0 && y >= 0 && w >= 0 && h >= 0 && (uint32_t)(x + w) <= g.fb_w &&
                    (uint32_t)(y + h) <= g.efb_h,
                "scissor outside the EFB")) {
        return;
    }
    if (need(K_SCISSOR, memcmp(v, g.scissor, sizeof v) == 0)) {
        memcpy(g.scissor, v, sizeof v);
        GX_SetScissor((u32)x, (u32)y, (u32)w, (u32)h);
        g.dirty = true;
    }
}

// ---- projection, matrices, fog ----------------------------------------------------------------

static void apply_fog(void) {
    FogKey k;
    memset(&k, 0, sizeof k);
    k.mode = (uint32_t)g.fog.mode;
    if (k.mode != WL_GX_FOG_OFF) {
        k.ortho = g.proj_type == GX_ORTHOGRAPHIC ? 1u : 0u;
        k.start = g.fog.start;
        k.end = g.fog.end;
        k.znear = g.proj_near;
        k.zfar = g.proj_far;
        k.rgba = g.fog.rgba;
    }
    if (!need(K_FOG, memcmp(&k, &g.fog_hw, sizeof k) == 0)) {
        return;
    }
    g.fog_hw = k;
    static const u8 type[WL_GX_FOG_COUNT] = {GX_FOG_NONE, GX_FOG_PERSP_LIN, GX_FOG_PERSP_EXP, GX_FOG_PERSP_EXP2};
    u8 t = (u8)(type[k.mode] + (k.ortho != 0 ? 8 : 0));
    if (k.mode == WL_GX_FOG_OFF) {
        GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, unpack(0));
    } else {
        GX_SetFog(t, k.start, k.end, k.znear, k.zfar, unpack(k.rgba));
    }
    g.dirty = true;
}

void wl_gx_set_fog(WlGxFog mode, float start, float end, uint32_t rgba) {
    if (!VCHECK((unsigned)mode < WL_GX_FOG_COUNT, "bad fog mode") ||
        !VCHECK(mode == WL_GX_FOG_OFF || end > start, "fog end must be beyond start")) {
        return;
    }
    g.fog.mode = mode;
    g.fog.start = start;
    g.fog.end = end;
    g.fog.rgba = rgba;
    apply_fog();
}

static void load_proj(const Mtx44 m, u8 type, float znear, float zfar) {
    if (need(K_PROJ, g.proj_type == type && memcmp(m, g.proj, sizeof g.proj) == 0)) {
        memcpy(g.proj, m, sizeof g.proj);
        g.proj_type = type;
        g.proj_near = znear;
        g.proj_far = zfar;
        GX_LoadProjectionMtx((f32(*)[4])m, type);
        g.dirty = true;
        apply_fog(); // fog depends on the projection's range and kind
    }
}

void wl_gx_set_ortho(float left, float top, float right, float bottom, float znear, float zfar) {
    Mtx44 m;
    guOrtho(m, top, bottom, left, right, znear, zfar);
    load_proj(m, GX_ORTHOGRAPHIC, znear, zfar);
}

void wl_gx_set_perspective(float fovy_degrees, float aspect, float znear, float zfar) {
    Mtx44 m;
    guPerspective(m, fovy_degrees, aspect, znear, zfar);
    load_proj(m, GX_PERSPECTIVE, znear, zfar);
}

void wl_gx_set_modelview(const float m[12]) {
    static const f32 ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const f32 *src = m != NULL ? m : ident;
    if (need(K_MODELVIEW, memcmp(src, g.modelview, sizeof g.modelview) == 0)) {
        memcpy(g.modelview, src, sizeof g.modelview);
        GX_LoadPosMtxImm((f32(*)[4])g.modelview, GX_PNMTX0);
        g.nrm_valid = false;
        g.dirty = true;
    }
}

void wl_gx_load_matrix_slot(uint32_t slot, const float m[12]) {
    WL_ASSERT(g.ready);
    if (!VCHECK(slot >= 1u && slot <= 9u && m != NULL, "bad matrix slot") || !VCHECK(!g.dl_active, "matrix load while recording a display list")) {
        return;
    }
    Mtx n;
    if (guMtxInvXpose((f32(*)[4])m, n) == 0) {
        memset(n, 0, sizeof n);
        n[0][0] = n[1][1] = n[2][2] = 1.0f;
    }
    GX_LoadPosMtxImm((f32(*)[4])m, GX_PNMTX0 + 3u * slot);
    GX_LoadNrmMtxImm(n, GX_PNMTX0 + 3u * slot);
    g.dirty = true;
}

// The normal matrix (inverse transpose of the model-view) is needed by lit draws only, so it is
// computed lazily, once per model-view change.
static void ensure_nrm(void) {
    if (g.nrm_valid) {
        return;
    }
    Mtx n;
    if (guMtxInvXpose((f32(*)[4])g.modelview, n) == 0) {
        memset(n, 0, sizeof n);
        n[0][0] = n[1][1] = n[2][2] = 1.0f;
    }
    GX_LoadNrmMtxImm(n, GX_PNMTX0);
    g.nrm_valid = true;
    g.dirty = true;
}

// ---- fixed-function state ---------------------------------------------------------------------

void wl_gx_set_cull(WlGxCull cull) {
    static const u8 map[] = {GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_ALL};
    if (!VCHECK((unsigned)cull <= WL_GX_CULL_ALL, "bad cull mode")) {
        return;
    }
    if (need(K_CULL, cull == g.cull)) {
        g.cull = cull;
        GX_SetCullMode(map[cull]);
        g.dirty = true;
    }
}

void wl_gx_set_blend(WlGxBlend blend) {
    if (!VCHECK((unsigned)blend < WL_GX_BLEND_COUNT, "bad blend mode")) {
        return;
    }
    if (need(K_BLEND, blend == g.blend)) {
        g.blend = blend;
        switch (blend) {
        case WL_GX_BLEND_ALPHA:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            break;
        case WL_GX_BLEND_ADD:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
            break;
        case WL_GX_BLEND_MULTIPLY:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_ZERO, GX_LO_CLEAR);
            break;
        case WL_GX_BLEND_PREMULT:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            break;
        default:
            GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
            break;
        }
        g.dirty = true;
    }
}

void wl_gx_set_zmode(bool test, WlGxCompare func, bool write) {
    if (!VCHECK((unsigned)func <= WL_GX_ALWAYS, "bad compare function")) {
        return;
    }
    VCHECK(test || !write, "z write without z test is ignored by the GPU (use func ALWAYS)");
    if (need(K_ZMODE, test == g.z.test && func == g.z.func && write == g.z.write)) {
        g.z.test = test;
        g.z.func = func;
        g.z.write = write;
        GX_SetZMode(test ? GX_TRUE : GX_FALSE, (u8)func, write ? GX_TRUE : GX_FALSE);
        g.dirty = true;
    }
}

void wl_gx_set_alpha_test(WlGxCompare func, uint8_t ref) {
    if (!VCHECK((unsigned)func <= WL_GX_ALWAYS, "bad compare function")) {
        return;
    }
    if (need(K_ALPHA, func == g.alpha.func && ref == g.alpha.ref)) {
        g.alpha.func = func;
        g.alpha.ref = ref;
        GX_SetAlphaCompare((u8)func, ref, GX_AOP_AND, GX_ALWAYS, 0);
        // The alpha test needs the texture's alpha: with it on, depth is tested after texturing, or
        // a rejected pixel would still write Z (cut-outs punched into the depth buffer).
        GX_SetZCompLoc(func == WL_GX_ALWAYS ? GX_TRUE : GX_FALSE);
        g.dirty = true;
    }
}

void wl_gx_set_color_write(bool rgb, bool alpha) {
    if (need(K_CWRITE, rgb == g.cwrite.rgb && alpha == g.cwrite.a)) {
        g.cwrite.rgb = rgb;
        g.cwrite.a = alpha;
        GX_SetColorUpdate(rgb ? GX_TRUE : GX_FALSE);
        GX_SetAlphaUpdate(alpha ? GX_TRUE : GX_FALSE);
        g.dirty = true;
    }
}

void wl_gx_set_konst(uint32_t rgba) {
    if (need(K_KONST, rgba == g.konst)) {
        g.konst = rgba;
        GX_SetTevKColor(GX_KCOLOR0, unpack(rgba));
        g.dirty = true;
    }
}

void wl_gx_set_material_color(uint32_t rgba) {
    if (need(K_MATCOL, rgba == g.matcol)) {
        g.matcol = rgba;
        GX_SetChanMatColor(GX_COLOR0A0, unpack(rgba));
        g.dirty = true;
    }
}

// ---- colour channel: lighting and the material colour source ---------------------------------

static bool recipe_textured(WlGxRecipe r) {
    return r != WL_GX_RECIPE_VERTEX_COLOR && r != WL_GX_RECIPE_KONST; // a flat konst colour reads no texture
}

static bool recipe_lit(WlGxRecipe r) {
    return r == WL_GX_RECIPE_TEX_LIT;
}

// Channel 0 follows the recipe (lit or not), the number of lights and whether vertices carry colours.
static void apply_chan(void) {
    bool lit = recipe_lit(g.recipe);
    ChanKey k = {lit ? 1u : 0u, lit ? (1u << g.light_count) - 1u : 0u,
                 g.hw_layout.color == WL_GX_COLOR_NONE ? 1u : 0u};
    if (!need(K_CHAN, memcmp(&k, &g.chan, sizeof k) == 0)) {
        return;
    }
    g.chan = k;
    u8 matsrc = k.mat_reg != 0 ? GX_SRC_REG : GX_SRC_VTX;
    if (lit) {
        GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, matsrc, (u8)k.mask, GX_DF_CLAMP, GX_AF_NONE);
        GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, matsrc, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    } else {
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, matsrc, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    }
    g.dirty = true;
}

void wl_gx_set_lighting(uint32_t ambient_rgba, const WlGxDirLight *lights, uint32_t count) {
    if (!VCHECK(count <= WL_GX_MAX_LIGHTS && (lights != NULL || count == 0), "bad lighting arguments")) {
        return;
    }
    WlGxDirLight tmp[WL_GX_MAX_LIGHTS];
    memset(tmp, 0, sizeof tmp);
    if (count > 0) {
        memcpy(tmp, lights, count * sizeof tmp[0]);
    }
    bool same = ambient_rgba == g.ambient && count == g.light_count && memcmp(tmp, g.lights, sizeof tmp) == 0;
    if (!need(K_LIGHTS, same)) {
        return;
    }
    g.ambient = ambient_rgba;
    g.light_count = count;
    memcpy(g.lights, tmp, sizeof tmp);
    GX_SetChanAmbColor(GX_COLOR0, unpack(ambient_rgba));
    for (uint32_t i = 0; i < count; i++) {
        GXLightObj lo;
        const float *d = tmp[i].dir;
        GX_InitLightPos(&lo, -d[0] * LIGHT_DISTANCE, -d[1] * LIGHT_DISTANCE, -d[2] * LIGHT_DISTANCE);
        GX_InitLightColor(&lo, unpack(tmp[i].rgba));
        GX_InitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        GX_LoadLightObj(&lo, (u8)(GX_LIGHT0 << i));
    }
    g.dirty = true;
    apply_chan();
}

// ---- recipes ----------------------------------------------------------------------------------

// The generator of texture coordinate 0 every recipe uses: the vertex's first texture coordinate through the identity or, while a texture matrix is set, TEXMTX0.
static void texgen0(void) {
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, g.texmtx_on ? GX_TEXMTX0 : GX_IDENTITY);
}

void wl_gx_set_tex_matrix(const float m[6]) {
    WL_ASSERT(g.ready);
    if (g.dl_active) {
        verr("state change while recording a display list");
        return;
    }
    g.stats.state_calls++;
    bool on = m != NULL;
    // the matrix register keeps its contents while the generator does not use it, so a matrix that comes back unchanged is not loaded again
    if (on && ((g.known & K_TEXMTX) == 0 || memcmp(m, g.texmtx, sizeof g.texmtx) != 0)) {
        // rows of the 2x4: {a b 0 tu} {c d 0 tv}; the input is (s, t, 1, 1), so the third and fourth columns both add a constant: the third stays 0
        Mtx tm = {{m[0], m[1], 0.0f, m[2]}, {m[3], m[4], 0.0f, m[5]}, {0.0f, 0.0f, 1.0f, 0.0f}};
        GX_LoadTexMtxImm(tm, GX_TEXMTX0, GX_MTX2x4);
        memcpy(g.texmtx, m, sizeof g.texmtx);
        g.known |= K_TEXMTX;
        g.stats.state_writes++;
        g.stats.tex_mtx_loads++;
        g.dirty = true;
    }
    if (on != g.texmtx_on || (g.known & K_TEXGEN) == 0) {
        g.texmtx_on = on;
        texgen0();
        g.known |= K_TEXGEN;
        g.stats.state_writes++;
        g.dirty = true;
    }
}

void wl_gx_set_recipe(WlGxRecipe recipe) {
    if (!VCHECK((unsigned)recipe < WL_GX_RECIPE_COUNT, "bad recipe")) {
        return;
    }
    if (!need(K_RECIPE, recipe == g.recipe)) {
        return;
    }
    g.recipe = recipe;
    // The stages are a WlTevProgram (tev.c); the colour channels and texture generators follow from
    // what its stages read. Konst 0 is the program's external slot: wl_gx_set_konst writes it.
    WlTevProgram prog;
    wl_tev_recipe(&prog, recipe);
    wl_tev_apply_unchecked(&prog);
    GX_SetNumChans((u8)wl_tev_channels_used(&prog));
    uint32_t texgens = wl_tev_texcoords_used(&prog);
    GX_SetNumTexGens((u8)texgens);
    if (texgens != 0) {
        texgen0();
    }
    g.dirty = true;
    apply_chan();
}

// ---- vertex layouts and arrays ----------------------------------------------------------------

static bool layout_valid(const WlGxLayout *l) {
    bool pos = l->pos == WL_GX_COMP_F32 || l->pos == WL_GX_COMP_S16 || l->pos == WL_GX_COMP_S8;
    bool nrm = l->normal == WL_GX_COMP_NONE || l->normal == WL_GX_COMP_F32 || l->normal == WL_GX_COMP_S16 ||
               l->normal == WL_GX_COMP_S8;
    bool uv = l->uv <= WL_GX_COMP_U8;
    return pos && nrm && uv && l->color <= WL_GX_COLOR_RGB8 && l->pos_shift <= 15 && l->uv_shift <= 15 &&
           l->indexed <= 1 && l->mtx_idx <= 1;
}

static u8 comp_type(uint8_t c) {
    switch (c) {
    case WL_GX_COMP_S16: return GX_S16;
    case WL_GX_COMP_U16: return GX_U16;
    case WL_GX_COMP_S8: return GX_S8;
    case WL_GX_COMP_U8: return GX_U8;
    default: return GX_F32;
    }
}

static void apply_layout(const WlGxLayout *l) {
    u8 how = l->indexed != 0 ? GX_INDEX16 : GX_DIRECT;
    GX_ClearVtxDesc();
    if (l->mtx_idx != 0) {
        GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
    }
    GX_SetVtxDesc(GX_VA_POS, how);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, comp_type(l->pos), l->pos_shift);
    if (l->normal != WL_GX_COMP_NONE) {
        GX_SetVtxDesc(GX_VA_NRM, how);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, comp_type(l->normal), 0);
    }
    if (l->color != WL_GX_COLOR_NONE) {
        static const u8 cnt[] = {0, GX_CLR_RGBA, GX_CLR_RGB, GX_CLR_RGBA, GX_CLR_RGB};
        static const u8 typ[] = {0, GX_RGBA8, GX_RGB565, GX_RGBA4, GX_RGB8};
        GX_SetVtxDesc(GX_VA_CLR0, how);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, cnt[l->color], typ[l->color], 0);
    }
    if (l->uv != WL_GX_COMP_NONE) {
        GX_SetVtxDesc(GX_VA_TEX0, how);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, comp_type(l->uv), l->uv_shift);
    }
}

// Makes `l` the hardware layout (through the shadow). While a display list records, the layout was
// fixed by dl_begin and only has to match.
static bool use_layout(const WlGxLayout *l) {
    if (g.dl_active) {
        return VCHECK(memcmp(l, &g.hw_layout, sizeof *l) == 0, "draw layout differs from the display list's");
    }
    if (need(K_LAYOUT, memcmp(l, &g.hw_layout, sizeof *l) == 0)) {
        g.hw_layout = *l;
        apply_layout(l);
        g.dirty = true;
    }
    apply_chan(); // the material colour source depends on the layout
    return true;
}

static const WlGxLayout k_std_direct = WL_GX_LAYOUT_STD;

WlResult wl_gx_set_layout(const WlGxLayout *layout) {
    if (layout == NULL || !layout_valid(layout)) {
        verr("set_layout: bad layout");
        return WL_ERR_ARG;
    }
    g.user_layout = *layout;
    return WL_OK;
}

void wl_gx_set_array(WlGxArray which, const void *base, uint32_t stride, uint32_t count) {
    WL_ASSERT(g.ready);
    static const u8 attr[WL_GX_ARRAY_COUNT] = {GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0};
    if (!VCHECK((unsigned)which < WL_GX_ARRAY_COUNT && base != NULL && stride > 0 && stride <= 255u, "bad vertex array") ||
        !VCHECK(((uintptr_t)base & 31u) == 0, "vertex array not 32-byte aligned")) {
        return;
    }
    g.array_set[which] = true;
    g.array_count[which] = count;
    GX_SetArray(attr[which], (void *)base, (u8)stride);
    GX_InvVtxCache(); // the array's contents may have changed since it was last used
    g.dirty = true;
}

void wl_gx_set_vertex_array(const WlGxVertex *verts, uint32_t count) {
    WL_ASSERT(g.ready);
    if (!VCHECK(verts != NULL && count > 0 && count <= 65536u, "bad vertex array") ||
        !VCHECK(((uintptr_t)verts & 31u) == 0, "vertex array not 32-byte aligned")) {
        return;
    }
    const u8 stride = (u8)sizeof(WlGxVertex);
    g.array_set[WL_GX_ARRAY_POS] = g.array_set[WL_GX_ARRAY_COLOR] = g.array_set[WL_GX_ARRAY_UV] = true;
    g.array_count[WL_GX_ARRAY_POS] = g.array_count[WL_GX_ARRAY_COLOR] = g.array_count[WL_GX_ARRAY_UV] = count;
    g.array_set[WL_GX_ARRAY_NORMAL] = false;
    GX_SetArray(GX_VA_POS, (void *)verts, stride);
    GX_SetArray(GX_VA_CLR0, (u8 *)verts + offsetof(WlGxVertex, rgba), stride);
    GX_SetArray(GX_VA_TEX0, (u8 *)verts + offsetof(WlGxVertex, u), stride);
    GX_InvVtxCache();
    g.user_layout = k_std_direct; // the WlGxVertex arrays are the STD layout
    g.dirty = true;
}

void wl_gx_reset_state(void) {
    WL_ASSERT(g.ready);
    wl_gx_set_viewport(0, 0, (int)g.fb_w, (int)g.efb_h);
    wl_gx_set_scissor(0, 0, (int)g.fb_w, (int)g.efb_h);
    wl_gx_set_ortho(0.0f, 0.0f, (float)g.fb_w, (float)g.efb_h, -1.0f, 1.0f); // 2D canvas, y down
    wl_gx_set_modelview(NULL);
    wl_gx_set_cull(WL_GX_CULL_NONE);
    wl_gx_set_blend(WL_GX_BLEND_OFF);
    wl_gx_set_zmode(true, WL_GX_LEQUAL, true);
    wl_gx_set_alpha_test(WL_GX_ALWAYS, 0);
    wl_gx_set_color_write(true, true);
    wl_gx_set_fog(WL_GX_FOG_OFF, 0.0f, 1.0f, 0);
    wl_gx_set_konst(0xFFFFFFFFu);
    wl_gx_set_material_color(0xFFFFFFFFu);
    wl_gx_set_lighting(0xFFFFFFFFu, NULL, 0);
    wl_gx_set_tex_matrix(NULL);
    wl_gx_set_recipe(WL_GX_RECIPE_VERTEX_COLOR);
    g.user_layout = k_std_direct;
    (void)use_layout(&k_std_direct);
}

// ---- textures ---------------------------------------------------------------------------------

static bool is_pow2(uint32_t v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

static bool format_ci(uint32_t fmt) {
    return fmt == WL_GX_TEX_C4 || fmt == WL_GX_TEX_C8;
}

static bool sampler_valid(const WlGxSampler *s) {
    return s->wrap_s <= WL_GX_WRAP_MIRROR && s->wrap_t <= WL_GX_WRAP_MIRROR && s->min_filter < WL_GX_FILTER_COUNT &&
           s->mag_filter <= WL_GX_FILTER_LINEAR;
}

WlResult wl_gx_tex_init_ex(WlGxTexObj *tex, const WlGxTexDesc *d) {
    static const u8 map[WL_GX_TEX_FORMAT_COUNT] = {GX_TF_I4,     GX_TF_I8,     GX_TF_IA4,   GX_TF_IA8,   GX_TF_RGB565,
                                                   GX_TF_RGB5A3, GX_TF_RGBA8, GX_TF_CMPR, GX_TF_CI4,   GX_TF_CI8};
    static const u8 wrap_map[] = {GX_CLAMP, GX_REPEAT, GX_MIRROR};
    if (tex == NULL || d == NULL || d->image == NULL || d->format >= WL_GX_TEX_FORMAT_COUNT || d->width == 0 ||
        d->height == 0 || d->width > 1024u || d->height > 1024u || !sampler_valid(&d->sampler)) {
        verr("tex_init: bad arguments");
        return WL_ERR_ARG;
    }
    if (((uintptr_t)d->image & 31u) != 0) {
        verr("tex_init: image not 32-byte aligned");
        return WL_ERR_ARG;
    }
    uint32_t mips = d->mips == 0 ? 1u : d->mips;
    bool pow2 = is_pow2(d->width) && is_pow2(d->height);
    if ((d->sampler.wrap_s != WL_GX_WRAP_CLAMP || d->sampler.wrap_t != WL_GX_WRAP_CLAMP) && !pow2) {
        verr("tex_init: repeat and mirror need power-of-two dimensions");
        return WL_ERR_ARG;
    }
    uint32_t maxdim = d->width > d->height ? d->width : d->height;
    uint32_t levels = 1;
    while ((maxdim >> levels) != 0) {
        levels++;
    }
    if (mips > 1u && (!pow2 || mips > levels)) {
        verr("tex_init: mip levels need power-of-two dimensions and at most log2(size)+1 levels");
        return WL_ERR_ARG;
    }
    if (format_ci(d->format) && d->tlut >= TLUT_SLOTS) {
        verr("tex_init: TLUT slot out of range");
        return WL_ERR_ARG;
    }
    GXTexObj obj;
    if (format_ci(d->format)) {
        GX_InitTexObjCI(&obj, (void *)d->image, d->width, d->height, map[d->format], wrap_map[d->sampler.wrap_s],
                        wrap_map[d->sampler.wrap_t], mips > 1u ? GX_TRUE : GX_FALSE, d->tlut);
    } else {
        GX_InitTexObj(&obj, (void *)d->image, d->width, d->height, map[d->format], wrap_map[d->sampler.wrap_s],
                      wrap_map[d->sampler.wrap_t], mips > 1u ? GX_TRUE : GX_FALSE);
    }
    u8 minf = d->sampler.min_filter;
    if (mips == 1u && minf >= WL_GX_FILTER_NEAREST_MIP_NEAREST) {
        minf = (minf & 1u) != 0 ? (u8)WL_GX_FILTER_LINEAR : (u8)WL_GX_FILTER_NEAREST;
    }
    GX_InitTexObjLOD(&obj, minf, d->sampler.mag_filter, 0.0f, (f32)(mips - 1u), d->sampler.lod_bias, GX_FALSE,
                     GX_FALSE, GX_ANISO_1);
    memset(tex, 0, sizeof *tex);
    memcpy(tex->opaque, &obj, sizeof obj);
    tex->sampler = d->sampler;
    tex->width = d->width;
    tex->height = d->height;
    tex->format = d->format;
    tex->mips = (uint8_t)mips;
    tex->tlut = d->tlut;
    return WL_OK;
}

WlResult wl_gx_tex_init(WlGxTexObj *tex, const void *image, uint32_t width, uint32_t height,
                        WlGxTexFormat fmt, bool repeat, bool linear) {
    WlGxTexDesc d;
    memset(&d, 0, sizeof d);
    d.image = image;
    d.width = (uint16_t)(width > 0xFFFFu ? 0xFFFFu : width);
    d.height = (uint16_t)(height > 0xFFFFu ? 0xFFFFu : height);
    d.format = (uint8_t)fmt;
    d.mips = 1;
    d.sampler.wrap_s = d.sampler.wrap_t = repeat ? (uint8_t)WL_GX_WRAP_REPEAT : (uint8_t)WL_GX_WRAP_CLAMP;
    d.sampler.min_filter = d.sampler.mag_filter = linear ? (uint8_t)WL_GX_FILTER_LINEAR : (uint8_t)WL_GX_FILTER_NEAREST;
    return wl_gx_tex_init_ex(tex, &d);
}

void wl_gx_set_filter_override(WlGxFilterOverride mode) {
    if (VCHECK((unsigned)mode <= WL_GX_FILTER_OVERRIDE_BILINEAR, "bad filter override")) {
        g.filter_override = mode; // takes effect at the next bind
    }
}

WlGxFilterOverride wl_gx_get_filter_override(void) {
    return g.filter_override;
}

WlResult wl_gx_tlut_init(WlGxTlut *tlut, const void *entries, WlGxTlutFormat fmt, uint16_t count) {
    static const u8 map[] = {GX_TL_IA8, GX_TL_RGB565, GX_TL_RGB5A3};
    if (tlut == NULL || entries == NULL || (unsigned)fmt > WL_GX_TLUT_RGB5A3 || count == 0 || count > 256u) {
        verr("tlut_init: bad arguments");
        return WL_ERR_ARG;
    }
    if (((uintptr_t)entries & 31u) != 0) {
        verr("tlut_init: entries not 32-byte aligned");
        return WL_ERR_ARG;
    }
    GXTlutObj obj;
    GX_InitTlutObj(&obj, (void *)entries, map[fmt], count);
    memset(tlut, 0, sizeof *tlut);
    memcpy(tlut->opaque, &obj, sizeof obj);
    tlut->entries = entries;
    tlut->count = count;
    tlut->format = (uint8_t)fmt;
    return WL_OK;
}

void wl_gx_tlut_load(const WlGxTlut *tlut, uint32_t slot) {
    WL_ASSERT(g.ready);
    if (!VCHECK(tlut != NULL && slot < TLUT_SLOTS, "bad TLUT load")) {
        return;
    }
    g.stats.state_calls++;
    TlutSlot *s = &g.tlut[slot];
    if (s->valid && s->data == tlut->entries && s->count == tlut->count && s->format == tlut->format) {
        return;
    }
    if (!VCHECK(!g.dl_active, "TLUT load while recording a display list")) {
        return;
    }
    GXTlutObj obj;
    memcpy(&obj, tlut->opaque, sizeof obj);
    GX_LoadTlut(&obj, slot);
    s->data = tlut->entries;
    s->count = tlut->count;
    s->format = tlut->format;
    s->valid = true;
    g.stats.state_writes++;
    g.stats.tlut_loads++;
    g.dirty = true;
}

void wl_gx_tlut_invalidate(void) {
    memset(g.tlut, 0, sizeof g.tlut);
}

void wl_gx_tex_bind_ex(const WlGxTexObj *tex, const WlGxSampler *sampler, const WlGxTlut *tlut) {
    static const u8 wrap_map[] = {GX_CLAMP, GX_REPEAT, GX_MIRROR};
    WL_ASSERT(g.ready);
    if (!VCHECK(tex != NULL, "bind of a null texture")) {
        return;
    }
    const WlGxSampler *s = sampler != NULL ? sampler : &tex->sampler;
    if (!VCHECK(sampler_valid(s), "bad sampler")) {
        return;
    }
    bool ci = format_ci(tex->format);
    if (ci) {
        if (tlut != NULL) {
            wl_gx_tlut_load(tlut, tex->tlut);
        }
        if (!VCHECK(g.tlut[tex->tlut].valid, "colour-indexed texture bound without a loaded TLUT")) {
            return;
        }
    }
    u8 wrap_s = s->wrap_s;
    u8 wrap_t = s->wrap_t;
    bool pow2 = is_pow2(tex->width) && is_pow2(tex->height);
    if ((wrap_s != WL_GX_WRAP_CLAMP || wrap_t != WL_GX_WRAP_CLAMP) && !pow2) {
        verr("bind: repeat and mirror need power-of-two dimensions (clamped)");
        wrap_s = wrap_t = WL_GX_WRAP_CLAMP;
    }
    u8 minf;
    u8 magf;
    wl_gx_resolve_filters(s, tex->mips, g.filter_override, &minf, &magf);
    GXTexObj obj;
    memcpy(&obj, tex->opaque, sizeof obj);
    GX_InitTexObjWrapMode(&obj, wrap_map[wrap_s], wrap_map[wrap_t]);
    GX_InitTexObjFilterMode(&obj, minf, magf);
    if (tex->mips > 1u) {
        GX_InitTexObjLODBias(&obj, s->lod_bias);
    }
    if (need(K_TEX, g.tex_bound && memcmp(&obj, &g.tex, sizeof g.tex) == 0)) {
        g.tex = obj;
        g.tex_bound = true;
        GX_LoadTexObj(&g.tex, GX_TEXMAP0);
        wl_gx_wii_texmaps_forget(1u << GX_TEXMAP0);
        g.stats.tex_loads++;
        g.dirty = true;
    }
    g.tex_format = tex->format;
}

void wl_gx_tex_bind(const WlGxTexObj *tex) {
    wl_gx_tex_bind_ex(tex, NULL, NULL);
}

void wl_gx_tex_invalidate(void) {
    GX_InvalidateTexAll();
    g.dirty = true;
}

void wl_gx_ramp_bind(uint32_t texmap) {
    static GXTexObj ramp;
    static bool made;
    WL_ASSERT(g.ready);
    if (!VCHECK(texmap < GX_MAX_TEXMAP, "ramp bound to a texture map that does not exist")) {
        return;
    }
    if (!made) {
        GX_InitTexObj(&ramp, (void *)wl_gx_ramp_image(), WL_GX_RAMP_WIDTH, WL_GX_RAMP_HEIGHT, GX_TF_I8, GX_CLAMP,
                      GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&ramp, GX_NEAR, GX_NEAR);
        made = true;
    }
    GX_LoadTexObj(&ramp, (u8)texmap);
    wl_gx_wii_texmaps_forget(1u << texmap);
    if (texmap == GX_TEXMAP0) {
        g.known &= ~(uint32_t)K_TEX;
    }
    g.dirty = true;
}

// ---- drawing ----------------------------------------------------------------------------------

static bool draw_ok(WlGxPrim prim, uint32_t count, const WlGxLayout *want, u8 *out) {
    static const u8 map[] = {GX_TRIANGLES, GX_TRIANGLESTRIP, GX_QUADS, GX_LINES, GX_POINTS};
    if (!VCHECK((unsigned)prim <= WL_GX_POINTS, "bad primitive")) {
        return false;
    }
    if (!VCHECK(count <= 65535u, "more than 65535 vertices in one draw")) {
        return false;
    }
    bool shape = true;
    switch (prim) {
    case WL_GX_TRIANGLES: shape = count % 3u == 0; break;
    case WL_GX_TRIANGLE_STRIP: shape = count == 0 || count >= 3u; break;
    case WL_GX_QUADS: shape = count % 4u == 0; break;
    case WL_GX_LINES: shape = count % 2u == 0; break;
    default: break;
    }
    if (!VCHECK(shape, "vertex count does not fit the primitive")) {
        return false;
    }
    if (!VCHECK(g.in_frame || g.dl_active, "draw outside begin_frame/end_frame")) {
        return false;
    }
    if (!g.dl_active) {
        if (!VCHECK(!recipe_textured(g.recipe) || g.tex_bound, "textured recipe without a bound texture") ||
            !VCHECK(!recipe_textured(g.recipe) || want->uv != WL_GX_COMP_NONE, "textured recipe with a layout without texture coordinates") ||
            !VCHECK(!recipe_lit(g.recipe) || want->normal != WL_GX_COMP_NONE, "lit recipe with a layout without normals")) {
            return false;
        }
    }
    if (!use_layout(want)) {
        return false;
    }
    if (!g.dl_active && recipe_lit(g.recipe)) {
        ensure_nrm();
    }
    *out = map[prim];
    return true;
}

void wl_gx_draw(WlGxPrim prim, const WlGxVertex *verts, uint32_t count) {
    WL_ASSERT(g.ready);
    u8 p;
    if (!VCHECK(verts != NULL || count == 0, "draw with null vertices") || !draw_ok(prim, count, &k_std_direct, &p) ||
        count == 0) {
        return;
    }
    GX_Begin(p, GX_VTXFMT0, (u16)count);
    for (uint32_t i = 0; i < count; i++) {
        GX_Position3f32(verts[i].x, verts[i].y, verts[i].z);
        GX_Color1u32(verts[i].rgba);
        GX_TexCoord2f32(verts[i].u, verts[i].v);
    }
    GX_End();
    if (g.dl_active) {
        g.dl->vertices += count;
        return;
    }
    g.stats.draw_calls++;
    g.stats.vertices += count;
    g.dirty = true;
}

void wl_gx_draw_indexed(WlGxPrim prim, const uint16_t *indices, uint32_t count) {
    WL_ASSERT(g.ready);
    WlGxLayout want = g.dl_active ? g.hw_layout : g.user_layout;
    want.indexed = 1;
    u8 p;
    if (!VCHECK(indices != NULL || count == 0, "draw with null indices") || !draw_ok(prim, count, &want, &p) ||
        count == 0) {
        return;
    }
    if (!g.dl_active) {
        bool ok = g.array_set[WL_GX_ARRAY_POS] &&
                  (want.normal == WL_GX_COMP_NONE || g.array_set[WL_GX_ARRAY_NORMAL]) &&
                  (want.color == WL_GX_COLOR_NONE || g.array_set[WL_GX_ARRAY_COLOR]) &&
                  (want.uv == WL_GX_COMP_NONE || g.array_set[WL_GX_ARRAY_UV]);
        if (!VCHECK(ok, "indexed draw without a vertex array for every attribute")) {
            return;
        }
    }
    if (g.validate) {
        uint32_t limit = 0xFFFFFFFFu;
        for (int a = 0; a < WL_GX_ARRAY_COUNT; a++) {
            if (g.array_set[a] && g.array_count[a] != 0 && g.array_count[a] < limit) {
                limit = g.array_count[a];
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (indices[i] >= limit) {
                verr("index beyond the vertex array");
                return;
            }
        }
    }
    GX_Begin(p, GX_VTXFMT0, (u16)count);
    for (uint32_t i = 0; i < count; i++) {
        if (want.mtx_idx != 0) {
            GX_MatrixIndex1x8(GX_PNMTX0);
        }
        GX_Position1x16(indices[i]);
        if (want.normal != WL_GX_COMP_NONE) {
            GX_Normal1x16(indices[i]);
        }
        if (want.color != WL_GX_COLOR_NONE) {
            GX_Color1x16(indices[i]);
        }
        if (want.uv != WL_GX_COMP_NONE) {
            GX_TexCoord1x16(indices[i]);
        }
    }
    GX_End();
    if (g.dl_active) {
        g.dl->vertices += count;
        return;
    }
    g.stats.draw_calls++;
    g.stats.vertices += count;
    g.dirty = true;
}

// ---- display lists ----------------------------------------------------------------------------

WlResult wl_gx_dl_begin(WlGxDisplayList *dl, void *buf, uint32_t capacity, const WlGxLayout *layout) {
    WL_ASSERT(g.ready);
    if (dl == NULL || buf == NULL || layout == NULL || !layout_valid(layout) || capacity < 64u || (capacity & 31u) != 0 ||
        ((uintptr_t)buf & 31u) != 0 || g.dl_active) {
        verr("dl_begin: bad arguments, unaligned buffer or a list is already recording");
        return WL_ERR_ARG;
    }
    WlGxLayout l = *layout;
    (void)use_layout(&l); // before recording: the layout is then also what the GPU has when the list is replayed
    memset(dl, 0, sizeof *dl);
    dl->layout = l;
    g.dl = dl;
    GX_BeginDispList(buf, capacity);
    g.dl_active = true;
    dl->data = buf;
    return WL_OK;
}

WlResult wl_gx_dl_end(WlGxDisplayList *dl) {
    WL_ASSERT(g.ready);
    if (!VCHECK(g.dl_active && g.dl == dl, "dl_end without a matching dl_begin")) {
        return WL_ERR_STATE;
    }
    u32 size = GX_EndDispList();
    g.dl_active = false;
    g.dl = NULL;
    if (size == 0) {
        dl->data = NULL;
        verr("dl_end: display list overflowed its buffer");
        return WL_ERR_RANGE;
    }
    dl->size = size;
    wl_gpu_buffer_commit((void *)dl->data, (size + 31u) & ~31u);
    return WL_OK;
}

WlResult wl_gx_dl_init(WlGxDisplayList *dl, const void *data, uint32_t size, uint32_t vertices,
                       const WlGxLayout *layout) {
    if (dl == NULL || data == NULL || layout == NULL || !layout_valid(layout) || size == 0 || (size & 31u) != 0 ||
        ((uintptr_t)data & 31u) != 0) {
        verr("dl_init: bad arguments (data and size must be 32-byte aligned)");
        return WL_ERR_ARG;
    }
    dl->data = data;
    dl->size = size;
    dl->vertices = vertices;
    dl->layout = *layout;
    return WL_OK;
}

void wl_gx_dl_call(const WlGxDisplayList *dl) {
    WL_ASSERT(g.ready);
    if (!VCHECK(dl != NULL && dl->data != NULL && dl->size != 0, "call of an empty display list") ||
        !VCHECK(g.in_frame && !g.dl_active, "display list call outside a frame or inside a recording")) {
        return;
    }
    if (!VCHECK(!recipe_textured(g.recipe) || g.tex_bound, "textured recipe without a bound texture") ||
        !VCHECK(!recipe_textured(g.recipe) || dl->layout.uv != WL_GX_COMP_NONE, "textured recipe with a list without texture coordinates") ||
        !VCHECK(!recipe_lit(g.recipe) || dl->layout.normal != WL_GX_COMP_NONE, "lit recipe with a list without normals")) {
        return;
    }
    if (!use_layout(&dl->layout)) {
        return;
    }
    if (recipe_lit(g.recipe)) {
        ensure_nrm();
    }
    GX_CallDispList((void *)dl->data, dl->size);
    g.stats.draw_calls++;
    g.stats.dl_calls++;
    g.stats.vertices += dl->vertices;
    g.dirty = true;
}
