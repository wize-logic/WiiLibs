// SPDX-License-Identifier: MIT
// The MCPX APU model (apu.h): registers, the front end's methods, notifiers and interrupts, the
// GP's command word, the frame loop and the stereo mix of the mixbins. The voice processor itself
// is apu_vp.c. A clean implementation from the hardware facts listed in apu_regs.h.
#include <stdlib.h>
#include <string.h>

#include "wl/xboxapu/apu_internal.h"

// ---- Guest memory ------------------------------------------------------------------------------

uint8_t *wl_apu_mem(WlXboxApu *apu, uint32_t phys, uint32_t bytes) {
    uint8_t *p = apu->cfg.phys_ptr != NULL ? apu->cfg.phys_ptr(apu->cfg.user, phys, bytes) : NULL;
    if (p == NULL) {
        apu->stats.bad_memory++;
    }
    return p;
}

uint32_t wl_apu_rd32(WlXboxApu *apu, uint32_t phys) {
    const uint8_t *p = wl_apu_mem(apu, phys, 4);
    return p != NULL ? wl_rd_le32(p) : 0;
}

void wl_apu_wr32(WlXboxApu *apu, uint32_t phys, uint32_t v) {
    uint8_t *p = wl_apu_mem(apu, phys, 4);
    if (p != NULL) {
        wl_wr_le32(p, v);
    }
}

uint8_t *wl_apu_voice_rec(WlXboxApu *apu, uint32_t v) {
    const uint32_t base = wl_apu_reg(apu, WL_APU_VPVADDR);
    if (base == 0 || v >= WL_XBOX_APU_VOICES) {
        return NULL;
    }
    return wl_apu_mem(apu, base + v * WL_APU_VOICE_SIZE, WL_APU_VOICE_SIZE);
}

static void rec_set(uint8_t *rec, uint32_t off, uint32_t mask, uint32_t value) {
    wl_wr_le32(rec + off, (wl_rd_le32(rec + off) & ~mask) | (value & mask));
}

// ---- Setup -------------------------------------------------------------------------------------

void wl_xbox_apu_config_default(WlXboxApuConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->gp_command_word = 0x810u;
    // DirectSound's speaker bins: 0 front left, 1 front right, 2 centre, 3 LFE, 4 rear left, 5 rear
    // right, 6..9 the 3D (crosstalk-cancelled) front left, front right, rear left, rear right.
    const uint16_t full = 32768, half = 16384, root = 23170; // 1.0, 0.5, 0.707
    cfg->mix_left[0] = full;
    cfg->mix_right[1] = full;
    cfg->mix_left[2] = cfg->mix_right[2] = root;
    cfg->mix_left[3] = cfg->mix_right[3] = half;
    cfg->mix_left[4] = root;
    cfg->mix_right[5] = root;
    cfg->mix_left[6] = full;
    cfg->mix_right[7] = full;
    cfg->mix_left[8] = root;
    cfg->mix_right[9] = root;
}

void wl_apu_voice_reset(WlApuVoice *vs) {
    const uint32_t base0 = vs->ssl_base[0], base1 = vs->ssl_base[1];
    const uint8_t count0 = vs->ssl_count[0], count1 = vs->ssl_count[1];
    memset(vs, 0, sizeof *vs);
    vs->ssl_base[0] = base0;
    vs->ssl_base[1] = base1;
    vs->ssl_count[0] = count0;
    vs->ssl_count[1] = count1;
    vs->blk_next = 0xFFu;
    vs->pitch_seen = 0x7FFFFFFF;
    vs->seg_entry = 0xFFFFFFFFu;
}

static void reset(WlXboxApu *apu) {
    memset(apu->regs, 0, sizeof apu->regs);
    memset(apu->dsp_key, 0, sizeof apu->dsp_key);
    for (uint32_t list = 0; list < 3u; list++) {
        for (uint32_t r = 0; r < 3u; r++) {
            wl_apu_set_reg(apu, WL_APU_TVL2D + list * WL_APU_LIST_STRIDE + r * 4u, WL_APU_NO_VOICE);
        }
    }
    for (uint32_t v = 0; v < WL_XBOX_APU_VOICES; v++) {
        memset(&apu->voice[v], 0, sizeof apu->voice[v]);
        wl_apu_voice_reset(&apu->voice[v]);
    }
    memset(apu->mix, 0, sizeof apu->mix);
}

WlXboxApu *wl_xbox_apu_create(const WlXboxApuConfig *cfg) {
    WlXboxApu *apu = calloc(1, sizeof *apu);
    if (apu == NULL) {
        return NULL;
    }
    if (cfg != NULL) {
        apu->cfg = *cfg;
    } else {
        wl_xbox_apu_config_default(&apu->cfg);
    }
    for (uint32_t b = 0; b < WL_XBOX_APU_BINS; b++) { // at most 1.0: the mix stays in 32 bits
        apu->cfg.mix_left[b] = apu->cfg.mix_left[b] > 32768u ? 32768u : apu->cfg.mix_left[b];
        apu->cfg.mix_right[b] = apu->cfg.mix_right[b] > 32768u ? 32768u : apu->cfg.mix_right[b];
        apu->out_left |= (uint32_t)(apu->cfg.mix_left[b] != 0) << b;
        apu->out_right |= (uint32_t)(apu->cfg.mix_right[b] != 0) << b;
    }
    wl_apu_vp_init();
    reset(apu);
    return apu;
}

void wl_xbox_apu_destroy(WlXboxApu *apu) {
    free(apu);
}

// ---- Interrupts --------------------------------------------------------------------------------

bool wl_xbox_apu_irq(const WlXboxApu *apu) {
    const uint32_t ien = wl_apu_reg(apu, WL_APU_IEN);
    return (ien & WL_APU_IRQ_GLOBAL) != 0 &&
           (wl_apu_reg(apu, WL_APU_ISTS) & ien & ~WL_APU_IRQ_GLOBAL) != 0;
}

static void raise(WlXboxApu *apu, uint32_t bits) {
    wl_apu_set_reg(apu, WL_APU_ISTS, wl_apu_reg(apu, WL_APU_ISTS) | bits);
}

bool wl_xbox_apu_running(const WlXboxApu *apu) {
    return (wl_apu_reg(apu, WL_APU_SECTL) & WL_APU_SECTL_COUNT) != 0 &&
           (wl_apu_reg(apu, WL_APU_FECTL) & WL_APU_FECTL_MODE) == 0;
}

// ---- Notifiers ---------------------------------------------------------------------------------

static void write_notifier(WlXboxApu *apu, uint32_t index) {
    const uint32_t base = wl_apu_reg(apu, WL_APU_FENADDR);
    if (base == 0) {
        return;
    }
    uint8_t *n = wl_apu_mem(apu, base + index * WL_APU_NOTIFIER_SIZE, WL_APU_NOTIFIER_SIZE);
    if (n != NULL) {
        n[14] = 1;
        n[15] = WL_APU_NOTIFY_DONE;
        apu->stats.notifies++;
    }
}

void wl_apu_notify_voice(WlXboxApu *apu, uint32_t v, uint32_t which) {
    write_notifier(apu, WL_APU_NOTIFIER_VOICE_BASE + v * WL_APU_NOTIFIERS_PER_VOICE + which);
    raise(apu, WL_APU_IRQ_FE_VOICE | WL_APU_IRQ_FE_NOTIFY);
}

void wl_apu_voice_off(WlXboxApu *apu, uint32_t v) {
    uint8_t *rec = wl_apu_voice_rec(apu, v);
    if (rec == NULL) {
        return;
    }
    rec_set(rec, WL_APU_V_STATE, WL_APU_STATE_ACTIVE, 0);
    const bool stream = (wl_rd_le32(rec + WL_APU_V_FMT) & WL_APU_FMT_STREAM) != 0;
    wl_apu_notify_voice(apu, v, stream ? apu->voice[v].ssl : 0u);
}

// The voice processor found a voice in a list that is not active: with FETFORCE1's idle-voice bit
// the front end traps on it, and the guest's interrupt handler takes the voice out of its list
// (DirectSound reads FEDECMETH/FEDECPARAM). One trap at a time: the others come in later frames.
// An idle voice met in a list traps the front end, one trap at a time, for the first such voice in
// list order, as the voice processor walks its lists. Deliberately so, not rotated among idle
// voices: a voice the driver never takes out of its list (state it got wrong) then traps every
// frame, which the repeat counter below makes visible ("idle voice N trapped M times in a row" in
// the status), instead of being hidden behind the others.
void wl_apu_idle_voice(WlXboxApu *apu, uint32_t v) {
    if ((wl_apu_reg(apu, WL_APU_FETFORCE1) & WL_APU_FETFORCE1_IDLE_VOICE) == 0 ||
        apu->trap_pending ||
        (wl_apu_reg(apu, WL_APU_FECTL) & WL_APU_FECTL_MODE) == WL_APU_FECTL_TRAPPED) {
        return;
    }
    const uint32_t fectl =
        wl_apu_reg(apu, WL_APU_FECTL) & ~(WL_APU_FECTL_MODE | WL_APU_FECTL_REASON);
    wl_apu_set_reg(apu, WL_APU_FECTL, fectl | WL_APU_FECTL_TRAPPED | WL_APU_FECTL_REASON);
    wl_apu_set_reg(apu, WL_APU_FEDECMETH, WL_APU_M_IDLE_VOICE);
    wl_apu_set_reg(apu, WL_APU_FEDECPARAM, v);
    raise(apu, WL_APU_IRQ_FE_TRAP);
    apu->trap_pending = true;
    apu->stats.traps++;
    apu->stats.trap_repeats = apu->stats.trap_voice == v ? apu->stats.trap_repeats + 1u : 1u;
    apu->stats.trap_voice = v;
}

// ---- Front end methods -------------------------------------------------------------------------

// Methods that set a field of the current voice's record: method, record offset, mask, shift of
// the parameter into the field.
static const struct {
    uint16_t method;
    uint8_t off;
    uint8_t shift;
    uint32_t mask;
} k_voice_fields[] = {
    {0x300, WL_APU_V_VBIN, 0, 0xFFFFFFFFu},
    {0x304, WL_APU_V_FMT, 0, 0xFFFFFFFFu},
    {0x308, WL_APU_V_ENV0, 0, 0xFFFFFFFFu},
    {0x30C, WL_APU_V_ENVA, 0, 0xFFFFFFFFu},
    {0x310, WL_APU_V_ENV1, 0, 0xFFFFFFFFu},
    {0x314, WL_APU_V_ENVF, 0, 0xFFFFFFFFu},
    {0x318, WL_APU_V_MISC, 0, 0xFFFFFFFFu},
    {0x31C, WL_APU_V_HRTF, 0, 0x0000FFFFu},
    {0x350, 0x50, 0, 0xFFFFFFFFu},
    {0x360, WL_APU_V_VOLA, 0, 0xFFFFFFFFu},
    {0x364, WL_APU_V_VOLB, 0, 0xFFFFFFFFu},
    {0x368, WL_APU_V_VOLC, 0, 0xFFFFFFFFu},
    {0x36C, WL_APU_V_LFO_ENV, 0, 0xFFFFFFFFu},
    {0x370, 0x70, 0, 0xFFFFFFFFu},
    {0x374, 0x74, 0, 0xFFFFFFFFu},
    {0x378, 0x78, 0, 0xFFFFFFFFu},
    {0x37C, WL_APU_V_PITCH_LINK, 0, 0xFFFF0000u},
    {0x3A0, WL_APU_V_BASE, 0, 0x00FFFFFFu},
    {0x3A4, WL_APU_V_LBO, 0, 0x00FFFFFFu},
    {0x3D8, WL_APU_V_CBO, 0, 0x00FFFFFFu},
    {0x3DC, WL_APU_V_EBO, 0, 0x00FFFFFFu},
};

// Inserts voice v into a list: at the head of list 1..3 (2D, 3D, multipass), or after the
// antecedent voice (list 0).
static void link_voice(WlXboxApu *apu, uint8_t *rec, uint32_t v) {
    const uint32_t feav = wl_apu_reg(apu, WL_APU_FEAV);
    const uint32_t list = (feav >> 16) & 3u;
    if (list != 0) {
        const uint32_t top = WL_APU_TVL2D + (list - 1u) * WL_APU_LIST_STRIDE;
        rec_set(rec, WL_APU_V_PITCH_LINK, 0xFFFFu, wl_apu_reg(apu, top));
        wl_apu_set_reg(apu, top, v);
        return;
    }
    uint8_t *ante = wl_apu_voice_rec(apu, feav & 0xFFFFu);
    if (ante == NULL || (feav & 0xFFFFu) == v) {
        return;
    }
    rec_set(rec, WL_APU_V_PITCH_LINK, 0xFFFFu, wl_rd_le32(ante + WL_APU_V_PITCH_LINK));
    rec_set(ante, WL_APU_V_PITCH_LINK, 0xFFFFu, v);
}

// An envelope's start: its stage, and the count that stage starts from (delay and hold count
// down from 16 x their time, attack up from 0).
static uint32_t env_start(uint32_t stage, uint32_t cfg0, uint32_t cfga) {
    switch (stage) {
    case WL_APU_ENV_DELAY:
        return ((cfg0 >> 12) & 0xFFFu) * 16u;
    case WL_APU_ENV_HOLD:
        return ((cfga >> 12) & 0xFFFu) * 16u;
    default:
        return 0;
    }
}

static void voice_on(WlXboxApu *apu, uint32_t arg) {
    const uint32_t v = arg & 0xFFFFu;
    uint8_t *rec = wl_apu_voice_rec(apu, v);
    if (rec == NULL) {
        return;
    }
    apu->stats.voice_ons++;
    link_voice(apu, rec, v);
    const uint32_t ea = (arg >> 28) & 0xFu, ef = (arg >> 24) & 0xFu;
    const uint32_t ea_count =
        env_start(ea, wl_rd_le32(rec + WL_APU_V_ENV0), wl_rd_le32(rec + WL_APU_V_ENVA));
    const uint32_t ef_count =
        env_start(ef, wl_rd_le32(rec + WL_APU_V_ENV1), wl_rd_le32(rec + WL_APU_V_ENVF));
    wl_wr_le32(rec + WL_APU_V_ECNT, (ea_count & 0xFFFFu) | (ef_count << 16));
    rec_set(rec, WL_APU_V_STATE, 0xFF000000u | WL_APU_STATE_ACTIVE | WL_APU_STATE_NEW,
            ea << 28 | ef << 24 | WL_APU_STATE_ACTIVE | WL_APU_STATE_NEW);
    rec_set(rec, WL_APU_V_CBO, 0x00FFFFFFu, 0);
    WlApuVoice *vs = &apu->voice[v];
    wl_apu_voice_reset(vs);
    vs->cbo_written = 0;
}

static void voice_release(WlXboxApu *apu, uint32_t v) {
    uint8_t *rec = wl_apu_voice_rec(apu, v);
    if (rec == NULL) {
        return;
    }
    const uint32_t ea_count = (wl_rd_le32(rec + WL_APU_V_LFO_ENV) & 0xFFFu) * 16u;
    const uint32_t ef_count = (wl_rd_le32(rec + WL_APU_V_MISC) & 0xFFFu) * 16u;
    wl_wr_le32(rec + WL_APU_V_ECNT, (ea_count & 0xFFFFu) | (ef_count << 16));
    rec_set(rec, WL_APU_V_STATE, 0xFF000000u, WL_APU_ENV_RELEASE << 28 | WL_APU_ENV_RELEASE << 24);
}

static void method(WlXboxApu *apu, uint32_t m, uint32_t arg) {
    apu->stats.methods++;
    if ((wl_apu_reg(apu, WL_APU_FECTL) & WL_APU_FECTL_MODE) != WL_APU_FECTL_TRAPPED) {
        wl_apu_set_reg(apu, WL_APU_FEDECMETH, m);
        wl_apu_set_reg(apu, WL_APU_FEDECPARAM, arg);
    }
    const uint32_t cur = wl_apu_reg(apu, WL_APU_FECV) & 0xFFFFu;
    for (size_t i = 0; i < WL_ARRAY_COUNT(k_voice_fields); i++) {
        if (k_voice_fields[i].method == m) {
            uint8_t *rec = wl_apu_voice_rec(apu, cur);
            if (rec != NULL) {
                rec_set(rec, k_voice_fields[i].off, k_voice_fields[i].mask,
                        arg << k_voice_fields[i].shift);
            }
            return;
        }
    }
    switch (m) {
    case WL_APU_M_NOTIFY:
        write_notifier(apu, 0);
        if (arg & 1u) {
            raise(apu, WL_APU_IRQ_FE_NOTIFY);
        }
        return;
    case WL_APU_M_SET_ANTECEDENT:
        wl_apu_set_reg(apu, WL_APU_FEAV, arg);
        return;
    case WL_APU_M_VOICE_ON:
        voice_on(apu, arg);
        return;
    case WL_APU_M_VOICE_OFF:
        apu->stats.voice_offs++;
        wl_apu_voice_off(apu, arg & 0xFFFFu);
        return;
    case WL_APU_M_VOICE_RELEASE:
        voice_release(apu, arg & 0xFFFFu);
        return;
    case WL_APU_M_VOICE_PAUSE: {
        uint8_t *rec = wl_apu_voice_rec(apu, arg & 0xFFFFu);
        if (rec != NULL) {
            rec_set(rec, WL_APU_V_STATE, WL_APU_STATE_PAUSED, arg & WL_APU_STATE_PAUSED);
        }
        return;
    }
    case WL_APU_M_SET_SSL:
        apu->ssl_page = arg;
        return;
    case WL_APU_M_HRTF_SUBMIXES:
        for (uint32_t i = 0; i < 4u; i++) {
            apu->hrtf_submix[i] = (uint8_t)((arg >> (8u * i)) & 0x1Fu);
        }
        return;
    case WL_APU_M_SET_CURRENT_VOICE:
        wl_apu_set_reg(apu, WL_APU_FECV, arg);
        return;
    case WL_APU_M_VOICE_LOCK:
        if (cur < WL_XBOX_APU_VOICES) {
            apu->voice[cur].locked = (arg & 1u) != 0;
        }
        return;
    case WL_APU_M_VOICE_SSL_A:
    case WL_APU_M_VOICE_SSL_B: {
        // The list's first segment and count; the record keeps them too (A where a buffer has its
        // base, B where it has its end).
        const uint32_t which = m == WL_APU_M_VOICE_SSL_A ? 0u : 1u;
        if (cur < WL_XBOX_APU_VOICES) {
            WlApuVoice *vs = &apu->voice[cur];
            vs->ssl_base[which] = arg >> 8; // a stream waiting for this list finds it next frame
            vs->ssl_count[which] = (uint8_t)arg;
            uint8_t *rec = wl_apu_voice_rec(apu, cur);
            if (rec != NULL) {
                rec_set(rec, which == 0 ? WL_APU_V_BASE : WL_APU_V_EBO, 0x00FFFFFFu, arg);
            }
        }
        return;
    }
    case WL_APU_M_INBUF_SGE:
        apu->inbuf_sge = arg;
        return;
    case WL_APU_M_INBUF_SGE_ADDR: {
        const uint32_t table = wl_apu_reg(apu, WL_APU_VPSGEADDR);
        if (table != 0) {
            wl_apu_wr32(apu, table + apu->inbuf_sge * 8u, arg & 0xFFFFF000u);
            apu->sge_gen++;
        }
        return;
    }
    case WL_APU_M_OUTBUF_SGE:
        apu->outbuf_sge = arg;
        return;
    case WL_APU_M_OUTBUF_SGE_ADDR: {
        // The pages of the GP's FIFO memory (DirectSound sets GPFMAXSGE just before), which the
        // voice processor's output buffers write into.
        const uint32_t table = wl_apu_reg(apu, WL_APU_GPFADDR);
        if (table != 0) {
            wl_apu_wr32(apu, table + apu->outbuf_sge * 8u, arg & 0xFFFFF000u);
        }
        return;
    }
    default:
        break;
    }
    if (m >= WL_APU_M_SSL_SEGMENTS && m < WL_APU_M_SSL_SEGMENTS_END) {
        const uint32_t table = wl_apu_reg(apu, WL_APU_VPSSLADDR);
        if (table != 0) {
            wl_apu_wr32(apu, table + apu->ssl_page * 8u + (m - WL_APU_M_SSL_SEGMENTS), arg);
            apu->sge_gen++;
        }
    }
    // the rest (headroom, HRTF tables, output buffers, context DMA, synchronisation) changes
    // nothing this model computes
}

// ---- Register window ---------------------------------------------------------------------------

static uint32_t *dsp_word(WlXboxApu *apu, uint32_t off, bool create) {
    const uint32_t key = (off & ~3u) | 1u;
    uint32_t i = ((off >> 2) * 2654435761u) & (WL_APU_DSP_SLOTS - 1u);
    for (uint32_t n = 0; n < WL_APU_DSP_SLOTS; n++) {
        if (apu->dsp_key[i] == key) {
            return &apu->dsp_val[i];
        }
        if (apu->dsp_key[i] == 0) {
            if (!create || n > WL_APU_DSP_SLOTS * 3u / 4u) {
                return NULL;
            }
            apu->dsp_key[i] = key;
            apu->dsp_val[i] = 0;
            return &apu->dsp_val[i];
        }
        i = (i + 1u) & (WL_APU_DSP_SLOTS - 1u);
    }
    return NULL;
}

static uint32_t read32(WlXboxApu *apu, uint32_t off) {
    if (off >= WL_APU_PIO_BASE && off < WL_APU_GP_BASE) {
        return (off - WL_APU_PIO_BASE) == WL_APU_PIO_FREE ? 0x80u : 0; // the FIFO is always empty
    }
    if (off >= WL_APU_GP_BASE) {
        const uint32_t *w = dsp_word(apu, off, false);
        return w != NULL ? *w : 0;
    }
    if (off < WL_APU_GLOBAL_LO || off >= WL_APU_GLOBAL_HI) {
        return 0;
    }
    uint32_t v = wl_apu_reg(apu, off);
    if (off == WL_APU_ISTS) {
        v = (v & ~WL_APU_IRQ_GLOBAL) | (wl_xbox_apu_irq(apu) ? WL_APU_IRQ_GLOBAL : 0u);
    }
    return v;
}

uint32_t wl_xbox_apu_read(WlXboxApu *apu, uint32_t offset, unsigned size) {
    if (offset >= WL_XBOX_APU_SIZE) {
        return 0;
    }
    const uint32_t w = read32(apu, offset & ~3u);
    if (size >= 4) {
        return w;
    }
    const uint32_t shift = (offset & 3u) * 8u;
    return (w >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
}

void wl_xbox_apu_write(WlXboxApu *apu, uint32_t offset, uint32_t value, unsigned size) {
    if (offset >= WL_XBOX_APU_SIZE) {
        return;
    }
    const uint32_t off = offset & ~3u;
    if (size < 4) { // merge a narrow write into the word
        const uint32_t shift = (offset & 3u) * 8u;
        const uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
        uint32_t old = 0;
        if (off >= WL_APU_GP_BASE || (off >= WL_APU_GLOBAL_LO && off < WL_APU_GLOBAL_HI)) {
            old = read32(apu, off);
        }
        value = (old & ~mask) | ((value << shift) & mask);
    }
    if (off >= WL_APU_PIO_BASE && off < WL_APU_GP_BASE) {
        method(apu, off - WL_APU_PIO_BASE, value);
        return;
    }
    if (off >= WL_APU_GP_BASE) {
        uint32_t *w = dsp_word(apu, off, true);
        if (w != NULL) {
            *w = value;
        }
        return;
    }
    if (off < WL_APU_GLOBAL_LO || off >= WL_APU_GLOBAL_HI) {
        return;
    }
    switch (off) {
    case WL_APU_ISTS:
        wl_apu_set_reg(apu, off, wl_apu_reg(apu, off) & ~value);
        return;
    case WL_APU_VPSGEADDR:
    case WL_APU_VPSSLADDR:
        apu->sge_gen++;
        break;
    case WL_APU_FEMEMDATA: {
        const uint32_t addr = wl_apu_reg(apu, WL_APU_FEMEMADDR);
        if (addr != 0) {
            wl_apu_wr32(apu, addr, value);
        }
        break;
    }
    default:
        break;
    }
    wl_apu_set_reg(apu, off, value);
}

// ---- The GP's command word ---------------------------------------------------------------------

// DirectSound hands its effects program a command in the GP's scratch memory and waits until the
// program clears the command word. The program does not run here; the word is cleared for it.
static void gp_command(WlXboxApu *apu) {
    const uint32_t off = apu->cfg.gp_command_word;
    const uint32_t table = wl_apu_reg(apu, WL_APU_GPSADDR);
    if (off == 0 || table == 0) {
        return;
    }
    const uint32_t *rst = dsp_word(apu, WL_APU_GPRST, false);
    if (rst == NULL || (*rst & 3u) != 3u) {
        return;
    }
    const uint32_t page = wl_apu_rd32(apu, table + (off >> 12) * 8u) & 0xFFFFF000u;
    if (page == 0) {
        return;
    }
    const uint32_t addr = page + (off & 0xFFFu);
    if (wl_apu_rd32(apu, addr) != 0) {
        wl_apu_wr32(apu, addr, 0);
        apu->stats.gp_commands++;
    }
}

// ---- Frames ------------------------------------------------------------------------------------

// A bin's sample for the stereo mix: a bin beyond twice full scale clips anyway; keeping it to 17
// bits keeps the products in 32 (the Wii's processor has no 64-bit multiply).
static inline int32_t bin17(int32_t v) {
    return v > 65535 ? 65535 : v < -65535 ? -65535 : v;
}

// One side's sum: acc[i] += (bin[i] x g) >> 15 for the bins in `bins` (bit b: bin b) with their
// weights.
static void mix_side(const WlXboxApu *apu, uint32_t bins, const uint16_t *weight, int32_t *acc) {
    for (uint32_t b = 0; bins != 0; b++, bins >>= 1) {
        if ((bins & 1u) == 0) {
            continue;
        }
        const int32_t *bin = apu->mix[b], g = weight[b];
        if (g == 32768) { // (s x 2^15) >> 15 is s
            for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i += 4u) {
                acc[i] += bin17(bin[i]);
                acc[i + 1u] += bin17(bin[i + 1u]);
                acc[i + 2u] += bin17(bin[i + 2u]);
                acc[i + 3u] += bin17(bin[i + 3u]);
            }
            continue;
        }
        for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i += 4u) {
            acc[i] += (bin17(bin[i]) * g) >> 15;
            acc[i + 1u] += (bin17(bin[i + 1u]) * g) >> 15;
            acc[i + 2u] += (bin17(bin[i + 2u]) * g) >> 15;
            acc[i + 3u] += (bin17(bin[i + 3u]) * g) >> 15;
        }
    }
}

// Each output side (16 bits, clipped and counted) of its sum.
static void out_side(WlXboxApu *apu, const int32_t *acc, int16_t *out) {
    for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++) {
        int32_t v = acc[i];
        if ((uint32_t)(v + 32768) > 65535u) {
            apu->stats.clipped++;
            v = v > 0 ? 32767 : -32768;
        }
        out[2u * i] = (int16_t)v;
    }
}

static void mix_out(WlXboxApu *apu, int16_t *out) {
    // the bins mixed this frame with a weight on a side (the others add nothing to it)
    int32_t l[WL_XBOX_APU_FRAME], r[WL_XBOX_APU_FRAME];
    for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++) {
        l[i] = r[i] = 0;
    }
    mix_side(apu, apu->out_left & apu->mix_used, apu->cfg.mix_left, l);
    mix_side(apu, apu->out_right & apu->mix_used, apu->cfg.mix_right, r);
    out_side(apu, l, out);
    out_side(apu, r, out + 1);
}

uint32_t wl_xbox_apu_run(WlXboxApu *apu, uint32_t frames, int16_t *out) {
    uint32_t done = 0;
    apu->trap_pending = false;
    for (; done < frames; done++) {
        gp_command(apu);
        if (!wl_xbox_apu_running(apu)) {
            if (done != 0 && apu->trap_pending) {
                break; // trapped in this run: the guest answers before the next frame
            }
            wl_apu_clear_mix(apu);
            apu->stats.frames_silent++;
        } else {
            wl_apu_vp_frame(apu);
            wl_apu_set_reg(apu, WL_APU_XGSCNT, wl_apu_reg(apu, WL_APU_XGSCNT) + WL_XBOX_APU_FRAME);
        }
        apu->stats.frames++;
        int16_t *frame_out = out != NULL ? out + done * WL_XBOX_APU_FRAME * 2u : NULL;
        if (frame_out != NULL) {
            mix_out(apu, frame_out);
        }
        if (apu->cfg.on_frame != NULL) {
            apu->cfg.on_frame(apu->cfg.on_frame_user, &apu->mix[0][0], frame_out);
        }
        if (apu->trap_pending) {
            done++;
            break;
        }
    }
    return done;
}

const int32_t *wl_xbox_apu_mixbins(const WlXboxApu *apu) {
    return &apu->mix[0][0];
}

void wl_xbox_apu_stats(const WlXboxApu *apu, WlXboxApuStats *out) {
    *out = apu->stats;
}
