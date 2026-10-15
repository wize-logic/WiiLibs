// SPDX-License-Identifier: MIT
// The MCPX AC'97 controller model (ac97.h). Registers as Intel's AC'97 2.3 / ICH bus master
// interface lays them out: the primary codec's registers at 0x000..0x07F, a bus master every 0x10
// from 0x100 (PCM in, PCM out at 0x110, mic) and the MCPX's S/PDIF out at 0x170, the global
// control at 0x12C, the global status at 0x130 and the codec access semaphore at 0x134.
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/xboxapu/ac97.h"

#define BM_BASE 0x100u
#define BM_SPDIF 0x170u
#define GLOB_CNT 0x12Cu
#define GLOB_STA 0x130u
#define CAS 0x134u

#define SR_DCH 0x01u   // halted
#define SR_CELV 0x02u  // the current buffer is the last valid one
#define SR_LVBCI 0x04u // the last valid buffer completed
#define SR_BCIS 0x08u  // a buffer with IOC completed
#define SR_FIFOE 0x10u
#define CR_RPBM 0x01u // run
#define CR_RR 0x02u   // reset the registers
#define CR_LVBIE 0x04u
#define CR_FEIE 0x08u
#define CR_IOCE 0x10u
#define BD_IOC 0x80000000u

#define GLOB_STA_PCR 0x00000100u // primary codec ready
#define GLOB_STA_SPDIF_INT 0x00000010u
#define GLOB_STA_PI_INT 0x00000020u
#define GLOB_STA_PO_INT 0x00000040u
#define GLOB_STA_MC_INT 0x00000080u

typedef struct {
    uint32_t bdbar;
    uint8_t civ, lvi, piv, cr;
    uint16_t sr, picb;
    bool loaded;    // the current descriptor is taken (picb counts it down)
    uint32_t flags; // its control bits
} BusMaster;

// 0 PCM in, 1 PCM out, 2 mic, 3 S/PDIF out
#define BM_COUNT 4u

struct WlXboxAc97 {
    WlXboxAc97Config cfg;
    uint16_t codec[64];
    uint32_t glob_cnt, glob_sta;
    BusMaster bm[BM_COUNT];
    WlXboxAc97Stats stats;
};

static BusMaster *bus_master(WlXboxAc97 *ac, uint32_t off, uint32_t *reg) {
    uint32_t i;
    if (off >= BM_BASE && off < BM_BASE + 0x2Cu) {
        i = (off - BM_BASE) >> 4;
    } else if (off >= BM_SPDIF && off < BM_SPDIF + 0x0Cu) {
        i = 3;
    } else {
        return NULL;
    }
    *reg = off & 0xFu;
    return &ac->bm[i];
}

static void bm_reset(BusMaster *b) {
    const uint32_t bdbar = b->bdbar;
    memset(b, 0, sizeof *b);
    b->bdbar = bdbar;
    b->sr = SR_DCH;
}

WlXboxAc97 *wl_xbox_ac97_create(const WlXboxAc97Config *cfg) {
    WlXboxAc97 *ac = calloc(1, sizeof *ac);
    if (ac == NULL) {
        return NULL;
    }
    if (cfg != NULL) {
        ac->cfg = *cfg;
    }
    for (uint32_t i = 0; i < BM_COUNT; i++) {
        bm_reset(&ac->bm[i]);
    }
    ac->codec[0x02 / 2] = 0x8000; // master volume: muted
    ac->codec[0x18 / 2] = 0x8808; // PCM out volume: muted, 0 dB
    ac->codec[0x26 / 2] = 0x000F; // ADC, DAC, analog mixer, reference ready
    ac->codec[0x7C / 2] = 0x8384; // a vendor id
    ac->codec[0x7E / 2] = 0x7600;
    return ac;
}

void wl_xbox_ac97_destroy(WlXboxAc97 *ac) {
    free(ac);
}

static bool bm_irq(const BusMaster *b) {
    return ((b->sr & SR_BCIS) && (b->cr & CR_IOCE)) || ((b->sr & SR_LVBCI) && (b->cr & CR_LVBIE)) ||
           ((b->sr & SR_FIFOE) && (b->cr & CR_FEIE));
}

static uint32_t glob_sta(const WlXboxAc97 *ac) {
    uint32_t v = ac->glob_sta & ~(GLOB_STA_PCR | GLOB_STA_SPDIF_INT | GLOB_STA_PI_INT |
                                  GLOB_STA_PO_INT | GLOB_STA_MC_INT);
    if (ac->cfg.codec_ready) {
        v |= GLOB_STA_PCR;
    }
    v |= bm_irq(&ac->bm[0]) ? GLOB_STA_PI_INT : 0u;
    v |= bm_irq(&ac->bm[1]) ? GLOB_STA_PO_INT : 0u;
    v |= bm_irq(&ac->bm[2]) ? GLOB_STA_MC_INT : 0u;
    v |= bm_irq(&ac->bm[3]) ? GLOB_STA_SPDIF_INT : 0u;
    return v;
}

bool wl_xbox_ac97_irq(const WlXboxAc97 *ac) {
    for (uint32_t i = 0; i < BM_COUNT; i++) {
        if (bm_irq(&ac->bm[i])) {
            return true;
        }
    }
    return false;
}

// Takes descriptor civ: its length (samples, 16-bit words) and control bits.
static void bm_load(WlXboxAc97 *ac, BusMaster *b) {
    const uint32_t at = (b->bdbar & ~7u) + (uint32_t)b->civ * 8u;
    const uint8_t *d = ac->cfg.phys_ptr != NULL ? ac->cfg.phys_ptr(ac->cfg.user, at, 8) : NULL;
    const uint32_t w1 = d != NULL ? wl_rd_le32(d + 4) : 0;
    b->picb = (uint16_t)w1;
    b->flags = w1 & 0xC0000000u;
    b->piv = (uint8_t)((b->civ + 1u) & 31u);
    b->loaded = true;
    if (b->civ == b->lvi) {
        b->sr |= SR_CELV;
    } else {
        b->sr &= (uint16_t)~SR_CELV;
    }
}

static uint8_t bm_read8(const BusMaster *b, uint32_t reg) {
    switch (reg) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3:
        return (uint8_t)(b->bdbar >> (8u * reg));
    case 0x4:
        return b->civ;
    case 0x5:
        return b->lvi;
    case 0x6:
        return (uint8_t)b->sr;
    case 0x7:
        return (uint8_t)(b->sr >> 8);
    case 0x8:
        return (uint8_t)b->picb;
    case 0x9:
        return (uint8_t)(b->picb >> 8);
    case 0xA:
        return b->piv;
    case 0xB:
        return b->cr;
    default:
        return 0;
    }
}

static void bm_write8(WlXboxAc97 *ac, BusMaster *b, uint32_t reg, uint8_t v) {
    switch (reg) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3: {
        const uint32_t shift = 8u * reg;
        b->bdbar = (b->bdbar & ~(0xFFu << shift)) | ((uint32_t)v << shift);
        break;
    }
    case 0x5:
        b->lvi = v & 31u;
        if ((b->cr & CR_RPBM) && (b->sr & SR_DCH) && b->loaded && b->lvi != b->civ) {
            // halted after the last valid buffer: a new last one lets it go on with the next (the
            // same one again leaves it halted, its buffer played)
            b->sr &= (uint16_t)~(SR_DCH | SR_CELV);
            b->civ = (uint8_t)((b->civ + 1u) & 31u);
            bm_load(ac, b);
        }
        break;
    case 0x6:
        b->sr &= (uint16_t)~(v & (SR_LVBCI | SR_BCIS | SR_FIFOE));
        break;
    case 0xB:
        if (v & CR_RR) {
            bm_reset(b);
            break;
        }
        if ((v & CR_RPBM) && !(b->cr & CR_RPBM)) {
            b->sr &= (uint16_t)~SR_DCH;
            if (!b->loaded) {
                bm_load(ac, b);
            }
        } else if (!(v & CR_RPBM)) {
            b->sr |= SR_DCH;
        }
        b->cr = v & (CR_RPBM | CR_LVBIE | CR_FEIE | CR_IOCE);
        break;
    default: // CIV, PICB, PIV are read-only
        break;
    }
}

uint32_t wl_xbox_ac97_read(WlXboxAc97 *ac, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t off = offset + i;
        uint32_t reg;
        uint8_t byte = 0;
        BusMaster *b = bus_master(ac, off, &reg);
        if (b != NULL) {
            byte = bm_read8(b, reg);
        } else if (off < 0x80u) {
            byte = (uint8_t)(ac->codec[off >> 1] >> (8u * (off & 1u)));
        } else if (off >= GLOB_CNT && off < GLOB_CNT + 4u) {
            byte = (uint8_t)(ac->glob_cnt >> (8u * (off - GLOB_CNT)));
        } else if (off >= GLOB_STA && off < GLOB_STA + 4u) {
            byte = (uint8_t)(glob_sta(ac) >> (8u * (off - GLOB_STA)));
        }
        // CAS (0x134) reads 0: no codec access is ever in progress
        v |= (uint32_t)byte << (8u * i);
    }
    return v;
}

void wl_xbox_ac97_write(WlXboxAc97 *ac, uint32_t offset, uint32_t value, unsigned size) {
    if (offset < 0x80u) {
        for (unsigned i = 0; i < size; i++) {
            const uint32_t off = offset + i;
            const uint32_t shift = 8u * (off & 1u);
            uint16_t *r = &ac->codec[off >> 1];
            *r = (uint16_t)((*r & ~(0xFFu << shift)) | (((value >> (8u * i)) & 0xFFu) << shift));
        }
        ac->codec[0x26 / 2] |= 0x000F; // the codec stays ready whatever is powered down
        return;
    }
    if (offset == GLOB_STA) {
        ac->glob_sta &= ~(value & 0x1u); // GPI status change: write one to clear
        return;
    }
    if (offset == GLOB_CNT) {
        ac->glob_cnt = value;
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        uint32_t reg;
        BusMaster *b = bus_master(ac, offset + i, &reg);
        if (b != NULL) {
            bm_write8(ac, b, reg, (uint8_t)(value >> (8u * i)));
        }
    }
}

void wl_xbox_ac97_advance(WlXboxAc97 *ac, uint32_t frames) {
    const bool before = wl_xbox_ac97_irq(ac);
    for (uint32_t i = 1; i < BM_COUNT; i += 2) { // the outputs: PCM out and S/PDIF out
        BusMaster *b = &ac->bm[i];
        if (!(b->cr & CR_RPBM) || (b->sr & SR_DCH)) {
            continue;
        }
        uint32_t words = frames * 2u; // stereo 16-bit: two words a frame
        for (uint32_t guard = 0; words > 0 && guard < 64u; guard++) {
            if (!b->loaded) {
                bm_load(ac, b);
            }
            const uint32_t take = words < b->picb ? words : b->picb;
            b->picb = (uint16_t)(b->picb - take);
            words -= take;
            if (b->picb != 0) {
                break;
            }
            ac->stats.buffers_done[i == 1 ? 0 : 1]++;
            if (b->flags & BD_IOC) {
                b->sr |= SR_BCIS;
            }
            if (b->civ == b->lvi) {
                b->sr |= SR_DCH | SR_CELV | SR_LVBCI;
                break;
            }
            b->civ = (uint8_t)((b->civ + 1u) & 31u);
            b->loaded = false;
        }
    }
    if (!before && wl_xbox_ac97_irq(ac)) {
        ac->stats.interrupts++;
    }
}

void wl_xbox_ac97_stats(const WlXboxAc97 *ac, WlXboxAc97Stats *out) {
    *out = ac->stats;
}
