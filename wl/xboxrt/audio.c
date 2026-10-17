// SPDX-License-Identifier: MIT
// The audio hardware behind the title (devices.h): wl/xboxapu's APU and AC'97 models as devices,
// their interrupt lines, the service tick that runs the APU's frames and the AC'97's bus masters,
// and where the sound goes: the platform's audio output at 48 kHz (the Wii's AESND voice takes
// 48 kHz as it is), a WAV file on the host, an MMIO trace on the host, checksums of the mixbins and
// the output on the host.
//
// Pacing. The APU makes 1500 frames of 32 samples a second. With the platform output running on
// its own thread (the Wii), the tick keeps `latency_frames` frames queued ahead of it, so the
// guest's audio clock follows the output's. Without one (the host's stub), frames follow the
// wall clock. Either way a tick does at most 64 frames: the guest falling far behind skips time
// rather than catching up in a burst. A run ends early after a frame that trapped or notified a
// stream's list (the ISR answers first, as on the Xbox, where it runs within the frame); the
// frames it left are due still, and the next tick makes them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/platform/ring.h"
#include "wl/base/endian.h"
#include "wl/xboxapu/ac97.h"
#include "wl/xboxapu/apu.h"
#include "wl/xboxrt/page_census.h"
#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"

#define TAG "xbaudio"
#define FRAME_SAMPLES (WL_XBOX_APU_FRAME * 2u) // stereo int16 a frame
#define RING_FRAMES 128u                       // 85 ms
#define TICK_MAX_FRAMES 64u

static WlXboxApu *g_apu;
static WlXboxAc97 *g_ac97;
static WlXbAudioConfig g_cfg;
static WlSpscRing g_ring;
static int16_t g_ring_store[RING_FRAMES * FRAME_SAMPLES];
static int16_t g_tick_buf[TICK_MAX_FRAMES * FRAME_SAMPLES];
static bool g_output;          // the platform output runs (on its own thread)
static uint64_t g_start_us;    // wall-clock pacing: when frame 0 was due
static uint64_t g_frames_made; // frames the tick made (wall-clock pacing)
static const int16_t *g_cur;   // the audio thread: the frame being played out ...
static uint32_t g_cur_off;     // ... and how much of it
static volatile uint32_t g_underruns;
#if !defined(__wii__)
static FILE *g_wav, *g_trace, *g_hash;
static uint64_t g_wav_frames;
static struct {
    uint64_t frames;
    uint32_t bins, out; // FNV-1a over every frame's 32 x 32 bin words and 64 output samples
} g_sum;
#endif
static struct {
    uint64_t run_us, run_frames;
    uint32_t tick_us_max;
    uint64_t next_status_us;
} g_cost;

static uint8_t *phys_ptr(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    const uint32_t ram = wl_xb_ram_bytes();
    if (phys >= ram || bytes > ram - phys) {
        return NULL;
    }
    wl_xb_census_phys(phys, bytes, WL_XB_CU_APU);
    return wl_xb_phys_ptr(phys);
}

// ---- The trace (host) --------------------------------------------------------------------------

static void trace(const char *dev, char rw, uint32_t off, unsigned size, uint32_t v) {
#if !defined(__wii__)
    if (g_trace == NULL) {
        return;
    }
    WlXbSymbol sym;
    const uintptr_t pc = wl_xb_mmio_fault_pc();
    uint32_t fn = 0, at = 0;
    if (pc != 0 && wl_xb_symbolize(pc, &sym)) {
        fn = sym.guest_va;
        at = (uint32_t)(pc - sym.host);
    }
    fprintf(g_trace, "%llu %s %c %05x %u %08x sub_%08X+%x\n", // host only
            (unsigned long long)wl_platform_time_us(), dev, rw, (unsigned)off, size, (unsigned)v,
            (unsigned)fn, (unsigned)at);
#else
    (void)dev;
    (void)rw;
    (void)off;
    (void)size;
    (void)v;
#endif
}

// ---- The checksums (host) ----------------------------------------------------------------------

#if !defined(__wii__)
static uint32_t fnv_words(uint32_t h, const uint32_t *w, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t b = 0; b < 32u; b += 8u) {
            h = (h ^ ((w[i] >> b) & 0xFFu)) * 16777619u;
        }
    }
    return h;
}

static void hash_frame(void *user, const int32_t *bins, const int16_t *out) {
    (void)user;
    uint32_t w[WL_XBOX_APU_BINS * WL_XBOX_APU_FRAME];
    const uint32_t nb = WL_XBOX_APU_BINS * WL_XBOX_APU_FRAME;
    for (uint32_t i = 0; i < nb; i++) {
        w[i] = (uint32_t)bins[i];
    }
    g_sum.bins = fnv_words(g_sum.bins, w, nb);
    if (out != NULL) {
        for (uint32_t i = 0; i < FRAME_SAMPLES; i++) {
            w[i] = (uint16_t)out[i];
        }
        g_sum.out = fnv_words(g_sum.out, w, FRAME_SAMPLES);
    }
    if (++g_sum.frames % (WL_XBOX_APU_RATE / WL_XBOX_APU_FRAME) == 0) {
        fprintf(g_hash, "frame %llu bins %08x out %08x\n", (unsigned long long)g_sum.frames,
                (unsigned)g_sum.bins, (unsigned)g_sum.out); // host only
        fflush(g_hash);
    }
}
#endif

// ---- The devices -------------------------------------------------------------------------------

static uint32_t apu_read(void *user, uint32_t off, unsigned size) {
    (void)user;
    const uint32_t v = wl_xbox_apu_read(g_apu, off, size);
    trace("apu", 'R', off, size, v);
    return v;
}

static void apu_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    (void)user;
    trace("apu", 'W', off, size, value);
    wl_xbox_apu_write(g_apu, off, value, size);
}

static uint32_t ac97_read(void *user, uint32_t off, unsigned size) {
    (void)user;
    const uint32_t v = wl_xbox_ac97_read(g_ac97, off, size);
    trace("ac97", 'R', off, size, v);
    return v;
}

static void ac97_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    (void)user;
    trace("ac97", 'W', off, size, value);
    wl_xbox_ac97_write(g_ac97, off, value, size);
}

static bool apu_irq(void *user) {
    (void)user;
    return wl_xbox_apu_irq(g_apu);
}

static bool ac97_irq(void *user) {
    (void)user;
    return wl_xbox_ac97_irq(g_ac97);
}

// ---- The output --------------------------------------------------------------------------------

// The audio thread: plays the queued frames out, silence when there are none.
static void fill(void *user, int16_t *stereo, uint32_t frames) {
    (void)user;
    while (frames > 0) {
        if (g_cur == NULL) {
            g_cur = wl_ring_peek(&g_ring);
            g_cur_off = 0;
            if (g_cur == NULL) {
                memset(stereo, 0, frames * 4u);
                g_underruns++;
                return;
            }
        }
        uint32_t take = WL_XBOX_APU_FRAME - g_cur_off;
        take = take < frames ? take : frames;
        memcpy(stereo, g_cur + g_cur_off * 2u, take * 4u);
        stereo += take * 2u;
        frames -= take;
        g_cur_off += take;
        if (g_cur_off == WL_XBOX_APU_FRAME) {
            wl_ring_discard(&g_ring);
            g_cur = NULL;
        }
    }
}

#if !defined(__wii__)
static void wav_header(void) {
    uint8_t h[44];
    const uint32_t data = (uint32_t)(g_wav_frames * 4u);
    memcpy(h, "RIFF", 4);
    wl_wr_le32(h + 4, 36u + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    wl_wr_le32(h + 16, 16);
    wl_wr_le16(h + 20, 1); // PCM
    wl_wr_le16(h + 22, 2); // stereo
    wl_wr_le32(h + 24, WL_XBOX_APU_RATE);
    wl_wr_le32(h + 28, WL_XBOX_APU_RATE * 4u);
    wl_wr_le16(h + 32, 4);
    wl_wr_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    wl_wr_le32(h + 40, data);
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, g_wav);
    fseek(g_wav, 0, SEEK_END);
}

static void wav_write(const int16_t *pcm, uint32_t frames) {
    uint8_t buf[TICK_MAX_FRAMES * FRAME_SAMPLES * 2u];
    const uint32_t n = frames * FRAME_SAMPLES;
    for (uint32_t i = 0; i < n; i++) {
        wl_wr_le16(buf + 2u * i, (uint16_t)pcm[i]);
    }
    fwrite(buf, 2, n, g_wav);
    const uint64_t before = g_wav_frames / WL_XBOX_APU_RATE;
    g_wav_frames += (uint64_t)frames * WL_XBOX_APU_FRAME;
    if (g_wav_frames / WL_XBOX_APU_RATE != before) {
        wav_header(); // a valid file each second, whenever the run ends
        fflush(g_wav);
    }
}
#endif

// ---- The tick ----------------------------------------------------------------------------------

static void tick(void *user) {
    (void)user;
    uint32_t want;
    const uint64_t now = wl_xb_time_us();
    if (g_output && !wl_xb_deterministic()) { // the deterministic mode: by the (virtual) clock
        const uint32_t queued = wl_ring_count(&g_ring);
        want = queued < g_cfg.latency_frames ? g_cfg.latency_frames - queued : 0;
    } else {
        const uint64_t due = (now - g_start_us) * (WL_XBOX_APU_RATE / WL_XBOX_APU_FRAME) / 1000000u;
        if (due > g_frames_made + 4u * TICK_MAX_FRAMES) {
            g_frames_made = due - TICK_MAX_FRAMES; // far behind: skip ahead
        }
        want = due > g_frames_made ? (uint32_t)(due - g_frames_made) : 0;
    }
    want = want < TICK_MAX_FRAMES ? want : TICK_MAX_FRAMES;
    if (want == 0) {
        return;
    }
    const uint64_t t0 = wl_platform_time_us();
    const uint32_t done = wl_xbox_apu_run(g_apu, want, g_tick_buf);
    const uint32_t spent = (uint32_t)(wl_platform_time_us() - t0);
    g_cost.run_us += spent;
    g_cost.run_frames += done;
    g_cost.tick_us_max = spent > g_cost.tick_us_max ? spent : g_cost.tick_us_max;
    g_frames_made += done;
    wl_xbox_ac97_advance(g_ac97, done * WL_XBOX_APU_FRAME);
    if (g_output) {
        for (uint32_t i = 0; i < done; i++) {
            if (!wl_ring_push(&g_ring, g_tick_buf + i * FRAME_SAMPLES)) {
                break; // full: the output stopped consuming
            }
        }
    }
#if !defined(__wii__)
    if (g_wav != NULL) {
        wav_write(g_tick_buf, done);
    }
#endif
    if (now >= g_cost.next_status_us) {
        g_cost.next_status_us = now + 10000000u;
        wl_xb_audio_log_status();
    }
}

void wl_xb_audio_log_status(void) {
    if (g_apu == NULL) {
        return;
    }
    WlXboxApuStats s;
    wl_xbox_apu_stats(g_apu, &s);
    WlXboxAc97Stats a;
    wl_xbox_ac97_stats(g_ac97, &a);
    const uint64_t frames = g_cost.run_frames ? g_cost.run_frames : 1u;
    const uint64_t live = s.frames > s.frames_silent ? s.frames - s.frames_silent : 1u;
    // 32-bit numbers only (the counts last 800 hours): the Wii's printf need not know long long
    WL_LOG_I(TAG,
             "status: %u frames (%u silent), voices %u.%01u a frame (max %u), %u ns a frame, tick "
             "max %u us; %u notifies, %u traps, %u GP commands, %u starved, %u clipped, %u bad "
             "addresses; output %u underruns; AC'97 %u+%u buffers",
             (unsigned)s.frames, (unsigned)s.frames_silent, (unsigned)(s.voice_frames / live),
             (unsigned)(s.voice_frames * 10u / live % 10u), (unsigned)s.voices_max,
             (unsigned)(g_cost.run_us * 1000u / frames), (unsigned)g_cost.tick_us_max,
             (unsigned)s.notifies, (unsigned)s.traps, (unsigned)s.gp_commands,
             (unsigned)s.stream_starved, (unsigned)s.clipped, (unsigned)s.bad_memory,
             (unsigned)g_underruns, (unsigned)a.buffers_done[0], (unsigned)a.buffers_done[1]);
    if (s.trap_repeats >= 16u) {
        // an idle voice the guest's handler leaves in its list: what its record says
        const uint32_t vpv = wl_xbox_apu_read(g_apu, 0x202Cu, 4);
        const uint8_t *rec = vpv != 0 ? phys_ptr(NULL, vpv + s.trap_voice * 0x80u, 0x80u) : NULL;
        if (rec != NULL) {
            WL_LOG_W(TAG,
                     "idle voice %u trapped %u times in a row: fmt %08x state %08x link %08x cbo "
                     "%08x ebo %08x; lists %04x %04x %04x",
                     (unsigned)s.trap_voice, (unsigned)s.trap_repeats, (unsigned)wl_rd_le32(rec + 4),
                     (unsigned)wl_rd_le32(rec + 0x54), (unsigned)wl_rd_le32(rec + 0x7C),
                     (unsigned)wl_rd_le32(rec + 0x58), (unsigned)wl_rd_le32(rec + 0x5C),
                     (unsigned)wl_xbox_apu_read(g_apu, 0x2054u, 4),
                     (unsigned)wl_xbox_apu_read(g_apu, 0x2060u, 4),
                     (unsigned)wl_xbox_apu_read(g_apu, 0x206Cu, 4));
        }
    }
}

// ---- Setup -------------------------------------------------------------------------------------

void wl_xb_audio_config_default(WlXbAudioConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->tick_us = 4000;
    cfg->latency_frames = 48;
    cfg->output = true;
#if !defined(__wii__)
    cfg->wav_path = getenv("WL_XB_AUDIO_WAV");
    cfg->trace_path = getenv("WL_XB_AUDIO_TRACE");
    cfg->hash_path = getenv("WL_XB_AUDIO_HASH");
    const char *off = getenv("WL_XB_AUDIO");
    cfg->off = off != NULL && strcmp(off, "0") == 0;
#endif
}

WlResult wl_xb_audio_attach(const WlXbAudioConfig *cfg) {
    if (cfg != NULL) {
        g_cfg = *cfg;
    } else {
        wl_xb_audio_config_default(&g_cfg);
    }
    if (g_cfg.off) {
        WL_LOG_I(TAG, "audio off: the idle APU and a codec that never gets ready");
        return WL_OK;
    }
    if (g_cfg.latency_frames == 0 || g_cfg.latency_frames > RING_FRAMES - 8u) {
        g_cfg.latency_frames = 48;
    }
    WlXboxApuConfig ac;
    wl_xbox_apu_config_default(&ac);
    ac.phys_ptr = phys_ptr;
#if !defined(__wii__)
    if (g_cfg.hash_path != NULL && g_cfg.hash_path[0] != '\0') {
        g_hash = fopen(g_cfg.hash_path, "w");
        if (g_hash != NULL) {
            g_sum.bins = g_sum.out = 2166136261u;
            ac.on_frame = hash_frame;
        } else {
            WL_LOG_W(TAG, "cannot write %s", g_cfg.hash_path);
        }
    }
#endif
    g_apu = wl_xbox_apu_create(&ac);
    WlXboxAc97Config cc = {phys_ptr, NULL, true};
    g_ac97 = wl_xbox_ac97_create(&cc);
    if (g_apu == NULL || g_ac97 == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_ring_init(&g_ring, g_ring_store, FRAME_SAMPLES * sizeof(int16_t), RING_FRAMES);
    // later registrations win: these replace the idle devices of wl_xb_devices_register_mcpx
    WlXbDevice apu = {"apu", WL_XB_APU_BASE, WL_XB_APU_SIZE, apu_read, apu_write, NULL};
    WlXbDevice aci = {"ac97", WL_XB_AC97_BASE, WL_XB_AC97_SIZE, ac97_read, ac97_write, NULL};
    if (r == WL_OK) {
        r = wl_xb_mmio_register(&apu);
    }
    if (r == WL_OK) {
        r = wl_xb_mmio_register(&aci);
    }
    if (r == WL_OK) {
        r = wl_xb_service_add_tick(tick, NULL, g_cfg.tick_us);
    }
    if (r != WL_OK) {
        return r;
    }
    wl_xb_irq_set_level(WL_XB_IRQ_APU, apu_irq, NULL);
    wl_xb_irq_set_level(WL_XB_IRQ_AC97, ac97_irq, NULL);
    g_start_us = wl_xb_time_us();
    g_cost.next_status_us = g_start_us + 10000000u;
    if (g_cfg.output && wl_platform_audio_start(WL_XBOX_APU_RATE, 256, fill, NULL) == WL_OK &&
        wl_platform_audio_threaded()) {
        g_output = true;
    }
#if !defined(__wii__)
    if (g_cfg.wav_path != NULL && g_cfg.wav_path[0] != '\0') {
        g_wav = fopen(g_cfg.wav_path, "wb");
        if (g_wav != NULL) {
            wav_header();
        } else {
            WL_LOG_W(TAG, "cannot write %s", g_cfg.wav_path);
        }
    }
    if (g_cfg.trace_path != NULL && g_cfg.trace_path[0] != '\0') {
        g_trace = fopen(g_cfg.trace_path, "w");
        if (g_trace != NULL) {
            setvbuf(g_trace, NULL, _IOLBF, 1u << 16); // whole lines even if the run is killed
        }
    }
    const char *wav = g_wav != NULL ? g_cfg.wav_path : NULL;
#else
    const char *wav = NULL;
#endif
    WL_LOG_I(TAG, "APU and AC'97 models attached: output %s, %u ms ahead%s%s",
             g_output ? "48 kHz" : "by the clock",
             (unsigned)(g_cfg.latency_frames * WL_XBOX_APU_FRAME * 1000u / WL_XBOX_APU_RATE),
             wav != NULL ? ", WAV " : "", wav != NULL ? wav : "");
    return WL_OK;
}
