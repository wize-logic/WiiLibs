// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026 sp00nz and contributors
// The MCPX's OHCI host controllers with the Xbox's gamepads behind the first (usb.h). From
// xboxrecomp's src/usb/ohci.c: the register semantics (HCR self-clearing, the root hub ports'
// set/clear writes with their change bits cleared first, the done head retired on the WDH
// acknowledgement), the list walking (one done queue per pass over every list, never written
// while the driver still owns the last one, short IN packets as DATAUNDERRUN with the endpoint
// halted) and the plug-in after the driver runs with the root hub's interrupt enabled. Replaced:
// the trap (wl/xboxrt's device faults), the controller and interrupt threads (the vertical
// blank's frame tick and a level-triggered line the kernel delivers), guest memory (physical
// addresses through the window, little-endian accessors).
#include <stdio.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/usb.h"

#define TAG "xbusb"

// ---- OHCI 1.0a operational registers -----------------------------------------------------------
#define HC_REVISION 0x00u
#define HC_CONTROL 0x04u
#define HC_COMMAND_STATUS 0x08u
#define HC_INTERRUPT_STATUS 0x0Cu
#define HC_INTERRUPT_ENABLE 0x10u
#define HC_INTERRUPT_DISABLE 0x14u
#define HC_HCCA 0x18u
#define HC_PERIOD_CURRENT_ED 0x1Cu
#define HC_CONTROL_HEAD_ED 0x20u
#define HC_CONTROL_CURRENT_ED 0x24u
#define HC_BULK_HEAD_ED 0x28u
#define HC_BULK_CURRENT_ED 0x2Cu
#define HC_DONE_HEAD 0x30u
#define HC_FM_INTERVAL 0x34u
#define HC_FM_REMAINING 0x38u
#define HC_FM_NUMBER 0x3Cu
#define HC_PERIODIC_START 0x40u
#define HC_LS_THRESHOLD 0x44u
#define HC_RH_DESCRIPTOR_A 0x48u
#define HC_RH_DESCRIPTOR_B 0x4Cu
#define HC_RH_STATUS 0x50u
#define HC_RH_PORT_STATUS 0x54u // four of them: a driver reads every one whatever the descriptor says
#define HC_REG_END 0x64u

#define CTL_CLE 0x10u  // control list enable
#define CTL_BLE 0x20u  // bulk list enable
#define CTL_PLE 0x04u  // periodic list enable
#define CTL_HCFS 0xC0u // functional state
#define HCFS_OPERATIONAL 0x80u

#define CS_HCR 0x01u
#define CS_CLF 0x02u
#define CS_BLF 0x04u

#define INTR_WDH 0x02u
#define INTR_SF 0x04u
#define INTR_RHSC 0x40u
#define INTR_SOURCES 0x7Fu
#define INTR_MIE 0x80000000u

#define PORT_CCS 0x00000001u
#define PORT_PES 0x00000002u
#define PORT_PPS 0x00000100u
#define PORT_CSC 0x00010000u
#define PORT_PESC 0x00020000u
#define PORT_PRSC 0x00100000u
// writes: set/clear requests by bit position
#define PORT_W_CLEAR_ENABLE 0x00000001u
#define PORT_W_SET_ENABLE 0x00000002u
#define PORT_W_SET_RESET 0x00000010u
#define PORT_W_SET_POWER 0x00000100u
#define PORT_W_CLEAR_POWER 0x00000200u

#define HCCA_FRAME_NUMBER 0x80u
#define HCCA_DONE_HEAD 0x84u

// endpoint and transfer descriptors (OHCI 1.0a section 4)
#define ED_SKIP (1u << 14)
#define ED_HALT 0x1u
#define ED_CARRY 0x2u
#define PTR_MASK 0xFFFFFFF0u
#define TD_ROUNDING (1u << 18)
#define DP_SETUP 0u
#define DP_OUT 1u
#define DP_IN 2u
#define CC_NOERROR 0u
#define CC_STALL 4u
#define CC_NOT_RESPONDING 5u
#define CC_DATAUNDERRUN 9u
#define CC_NAK 0xFFu // not a code: the device has nothing yet, the descriptor stays queued

#define MAX_EDS 64u   // a list longer than this is taken for a loop
#define MAX_TDS 64u
#define WALK_SEEN 64u // interrupt endpoints met in one periodic pass

typedef struct {
    int index;
    uint32_t base, line, ndp;
    uint32_t reg[HC_REG_END / 4u];
    uint64_t start_us; // when it went operational: the frame clock's origin
    bool running_logged, periodic_logged;
} Hc;

typedef struct {
    bool attached;            // on its root port (CCS)
    uint32_t attached_frames; // frame ticks since it was plugged in
    bool configured_logged;
    bool setup_pending;
    WlXbUsbSetup setup;
    int answer_len;           // the IN data stage, computed at its first IN (-1: not yet)
    uint32_t answer_sent;
    uint8_t answer[64];
    uint8_t data[64];         // the OUT data stage, collected
    uint32_t data_len;
} Pipe;

static Hc g_hc[2];
static Pipe g_pipe[WL_XB_USB_PORTS];
static WlXbUsbConfig g_cfg;
static WlXbUsbStats g_stats;
static uint32_t g_settle;   // frame ticks since the last plug-in or configuration
static bool g_registered;

void wl_xb_usb_config_default(WlXbUsbConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    // the front sockets 1, 2, 3, 4 are the root hub's ports 3, 4, 1, 2
    cfg->root_port[0] = 2;
    cfg->root_port[1] = 3;
    cfg->root_port[2] = 0;
    cfg->root_port[3] = 1;
    cfg->settle_frames = 30;
}

void wl_xb_usb_stats(WlXbUsbStats *out) {
    *out = g_stats;
}

void wl_xb_usb_pad_info(uint32_t port, uint8_t *address, uint8_t *configuration, bool *attached) {
    bool on = port < WL_XB_USB_PORTS && g_pipe[port].attached;
    *address = on ? wl_xb_xid_address(port) : 0;
    *configuration = on ? wl_xb_xid_configuration(port) : 0;
    *attached = on;
}

// ---- guest memory as the bus sees it ---------------------------------------------------------

// A bus address (physical, as the driver took it from MmGetPhysicalAddress) as the guest VA of its
// window alias; a window VA or a mapped user VA a driver passed unconverted is taken too.
static bool bus_va(uint32_t addr, uint32_t bytes, uint32_t *va) {
    uint32_t ram = wl_xb_ram_bytes();
    if (addr >= WL_XB_WINDOW && addr - WL_XB_WINDOW < ram) {
        addr -= WL_XB_WINDOW;
    } else if (addr >= ram) {
        uint32_t phys = 0;
        if (!wl_xb_va_to_phys(addr, &phys)) {
            return false;
        }
        addr = phys;
    }
    if (addr == 0 || (uint64_t)addr + bytes > ram) {
        return false;
    }
    *va = WL_XB_WINDOW | addr;
    return true;
}

static uint32_t bus_rd32(uint32_t addr) {
    uint32_t va;
    return bus_va(addr, 4, &va) ? wl_xb_rd32(va) : 0;
}

static void bus_wr32(uint32_t addr, uint32_t value) {
    uint32_t va;
    if (bus_va(addr, 4, &va)) {
        wl_xb_wr32(va, value);
    }
}

// A transfer's buffer is [CBP, BE] and may cross into BE's page at a 4 KB boundary.
static uint32_t buf_len(uint32_t cbp, uint32_t be) {
    if (cbp == 0) {
        return 0;
    }
    if (((cbp ^ be) & ~0xFFFu) == 0) {
        return be >= cbp ? be - cbp + 1u : 0;
    }
    return (0x1000u - (cbp & 0xFFFu)) + (be & 0xFFFu) + 1u;
}

static uint32_t buf_addr(uint32_t cbp, uint32_t be, uint32_t offset) {
    uint32_t first = 0x1000u - (cbp & 0xFFFu);
    if (((cbp ^ be) & ~0xFFFu) == 0 || offset < first) {
        return cbp + offset;
    }
    return (be & ~0xFFFu) + (offset - first);
}

static bool buf_read(uint32_t cbp, uint32_t be, uint8_t *dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t va;
        if (!bus_va(buf_addr(cbp, be, i), 1, &va)) {
            return false;
        }
        dst[i] = wl_xb_rd8(va);
    }
    return true;
}

static bool buf_write(uint32_t cbp, uint32_t be, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t va;
        if (!bus_va(buf_addr(cbp, be, i), 1, &va)) {
            return false;
        }
        wl_xb_wr8(va, src[i]);
    }
    return true;
}

// ---- the root hub and its gamepads -----------------------------------------------------------

static uint32_t *port_reg(Hc *hc, uint32_t root_port) {
    return &hc->reg[(HC_RH_PORT_STATUS + root_port * 4u) / 4u];
}

static void hc_raise(Hc *hc, uint32_t source) {
    hc->reg[HC_INTERRUPT_STATUS / 4u] |= source;
    if (source & hc->reg[HC_INTERRUPT_ENABLE / 4u]) {
        g_xb_attention = 1; // the line goes up: the next safe point delivers it
    }
}

static bool operational(const Hc *hc) {
    return (hc->reg[HC_CONTROL / 4u] & CTL_HCFS) == HCFS_OPERATIONAL;
}

// The Xbox port of the gamepad a descriptor's function address names: its address, or 0 for the
// one on an enabled port that has none yet (ports are reset and addressed one at a time).
static int pad_for(Hc *hc, uint32_t ed0) {
    if (hc->index != 0) {
        return -1;
    }
    uint32_t fa = ed0 & 0x7Fu;
    for (uint32_t p = 0; p < WL_XB_USB_PORTS; p++) {
        if (!g_pipe[p].attached || (*port_reg(hc, g_cfg.root_port[p]) & PORT_PES) == 0) {
            continue;
        }
        if (wl_xb_xid_address(p) == fa) {
            return (int)p;
        }
    }
    return -1;
}

static void plug(Hc *hc, uint32_t p, bool in) {
    uint32_t *ps = port_reg(hc, g_cfg.root_port[p]);
    Pipe *pipe = &g_pipe[p];
    memset(pipe, 0, sizeof *pipe);
    pipe->answer_len = -1;
    wl_xb_xid_reset(p);
    if (in) {
        pipe->attached = true;
        *ps |= PORT_CCS | PORT_CSC;
        g_stats.plugged++;
    } else {
        *ps &= ~(PORT_CCS | PORT_PES);
        *ps |= PORT_CSC | PORT_PESC;
        g_stats.unplugged++;
    }
    g_settle = 0;
    hc_raise(hc, INTR_RHSC);
    WL_LOG_I(TAG, "gamepad %s port %u (root hub port %u)", in ? "plugged into" : "unplugged from", (unsigned)p + 1u,
             (unsigned)g_cfg.root_port[p] + 1u);
}

// Plug in or out what input.h says, once the driver listens (operational, the root hub's change
// interrupt enabled), one gamepad at a time.
static void hotplug(Hc *hc) {
    if (hc->index != 0) {
        return;
    }
    bool listening = operational(hc) && (hc->reg[HC_INTERRUPT_ENABLE / 4u] & INTR_RHSC) != 0;
    bool busy = false;
    for (uint32_t p = 0; p < WL_XB_USB_PORTS; p++) {
        Pipe *pipe = &g_pipe[p];
        if (pipe->attached) {
            pipe->attached_frames++;
            if (wl_xb_xid_configuration(p) == 0 && pipe->attached_frames < 600u) {
                busy = true; // still being enumerated (given 10 s at most)
            } else if (wl_xb_xid_configuration(p) != 0 && !pipe->configured_logged) {
                pipe->configured_logged = true;
                g_stats.configured++;
                g_settle = 0;
                WL_LOG_I(TAG, "gamepad on port %u configured (address %u)", (unsigned)p + 1u,
                         (unsigned)wl_xb_xid_address(p));
            }
        }
    }
    g_settle++;
    for (uint32_t p = 0; p < WL_XB_USB_PORTS; p++) {
        bool want = wl_xb_input_connected(p);
        if (g_pipe[p].attached && !want) {
            plug(hc, p, false);
        } else if (!g_pipe[p].attached && want && listening && !busy && g_settle >= g_cfg.settle_frames) {
            plug(hc, p, true);
            busy = true;
        }
    }
}

// ---- transfers -------------------------------------------------------------------------------

// Moves one transfer descriptor; returns its condition code (CC_NAK: left queued).
static uint32_t do_td(Hc *hc, uint32_t ed0, uint32_t td, uint32_t *packets) {
    uint32_t info = bus_rd32(td);
    uint32_t cbp = bus_rd32(td + 4u);
    uint32_t be = bus_rd32(td + 12u);
    uint32_t dir = (ed0 >> 11) & 3u;
    uint32_t dp = dir == 1u ? DP_OUT : dir == 2u ? DP_IN : (info >> 19) & 3u;
    uint32_t len = buf_len(cbp, be);
    uint32_t ep = (ed0 >> 7) & 0xFu;
    uint32_t mps = (ed0 >> 16) & 0x7FFu;
    uint32_t moved = 0;
    int p = pad_for(hc, ed0);
    *packets = 1;
    if (p < 0) {
        return CC_NOT_RESPONDING;
    }
    Pipe *pipe = &g_pipe[p];
    if (g_cfg.trace) {
        WL_LOG_I(TAG, "OHCI%d TD %08x pad %d ep %u dp %u len %u", hc->index, (unsigned)td, p + 1, (unsigned)ep,
                 (unsigned)dp, (unsigned)len);
    }
    if (dp == DP_SETUP) {
        uint8_t b[8];
        if (len < 8u || !buf_read(cbp, be, b, 8)) {
            return CC_STALL;
        }
        pipe->setup.request_type = b[0];
        pipe->setup.request = b[1];
        pipe->setup.value = (uint16_t)(b[2] | (b[3] << 8));
        pipe->setup.index = (uint16_t)(b[4] | (b[5] << 8));
        pipe->setup.length = (uint16_t)(b[6] | (b[7] << 8));
        pipe->setup_pending = true;
        pipe->answer_len = -1;
        pipe->answer_sent = 0;
        pipe->data_len = 0;
        g_stats.setups++;
        if (g_cfg.trace) {
            WL_LOG_I(TAG, "OHCI%d SETUP pad %d %02x %02x value %04x index %04x length %u", hc->index, p + 1,
                     (unsigned)b[0], (unsigned)b[1], (unsigned)pipe->setup.value, (unsigned)pipe->setup.index,
                     (unsigned)pipe->setup.length);
        }
        moved = 8;
    } else if (dp == DP_IN) {
        if (ep == 0) {
            if (!pipe->setup_pending) {
                return CC_NOERROR; // a stray status stage: nothing to say
            }
            bool to_device = (pipe->setup.request_type & 0x80u) == 0;
            if (pipe->answer_len < 0) {
                pipe->answer_len = wl_xb_xid_control((uint32_t)p, &pipe->setup, pipe->data, pipe->data_len, pipe->answer,
                                                     sizeof pipe->answer);
                pipe->answer_sent = 0;
                if (pipe->answer_len < 0) {
                    // worth saying: a stall halts the endpoint, and a driver that sees one usually
                    // gives the device up
                    g_stats.stalls++;
                    WL_LOG_W(TAG, "pad %d stalls request %02x %02x value %04x index %04x length %u", p + 1,
                             (unsigned)pipe->setup.request_type, (unsigned)pipe->setup.request,
                             (unsigned)pipe->setup.value, (unsigned)pipe->setup.index, (unsigned)pipe->setup.length);
                    pipe->setup_pending = false;
                    return CC_STALL;
                }
            }
            uint32_t left = (uint32_t)pipe->answer_len - pipe->answer_sent;
            uint32_t n = left < len ? left : len;
            if (n > 0 && !buf_write(cbp, be, pipe->answer + pipe->answer_sent, n)) {
                return CC_STALL;
            }
            pipe->answer_sent += n;
            moved = n;
            if (to_device || pipe->setup.length == 0) {
                pipe->setup_pending = false; // that was the status stage
            }
        } else {
            uint8_t b[64];
            uint32_t cap = len < sizeof b ? len : (uint32_t)sizeof b;
            int n = wl_xb_xid_in((uint32_t)p, (uint8_t)ep, b, cap);
            if (n == 0) {
                return CC_NAK;
            }
            if (n < 0) {
                g_stats.stalls++;
                return CC_STALL;
            }
            if (!buf_write(cbp, be, b, (uint32_t)n)) {
                return CC_STALL;
            }
            moved = (uint32_t)n;
            g_stats.reports++;
        }
    } else { // OUT
        uint8_t b[64];
        uint32_t n = len < sizeof b ? len : (uint32_t)sizeof b;
        if (n > 0 && !buf_read(cbp, be, b, n)) {
            return CC_STALL;
        }
        if (ep == 0) {
            bool to_device = (pipe->setup.request_type & 0x80u) == 0;
            if (pipe->setup_pending && to_device && pipe->answer_len < 0) {
                uint32_t room = (uint32_t)sizeof pipe->data - pipe->data_len;
                uint32_t k = n < room ? n : room;
                memcpy(pipe->data + pipe->data_len, b, k); // the data stage, for the request at its status stage
                pipe->data_len += k;
            } else {
                pipe->setup_pending = false; // the status stage of a request that sent data in
            }
        } else {
            wl_xb_xid_out((uint32_t)p, (uint8_t)ep, b, n);
            g_stats.outs++;
        }
        moved = len;
    }
    if (mps != 0 && moved > mps) {
        *packets = (moved + mps - 1u) / mps;
    }
    // CBP is 0 when all of it moved, and else the first byte that did not
    bus_wr32(td + 4u, moved >= len ? 0u : buf_addr(cbp, be, moved));
    if (dp == DP_IN && moved < len && (info & TD_ROUNDING) == 0) {
        return CC_DATAUNDERRUN; // a short packet without bufferRounding (xboxrecomp: X-Men Legends)
    }
    return CC_NOERROR;
}

typedef struct {
    uint32_t done;          // this pass's done queue, newest first
    uint32_t seen[WALK_SEEN];
    uint32_t seen_count;
} Pass;

// Walks one list of endpoint descriptors. `one_each`: the periodic list, where an interrupt
// endpoint moves one packet a tick, however many frame slots it is in.
static void walk(Hc *hc, uint32_t ed, Pass *pass, bool one_each) {
    for (uint32_t guard = 0; ed != 0 && guard < MAX_EDS; guard++) {
        uint32_t ed0 = bus_rd32(ed);
        uint32_t tail = bus_rd32(ed + 4u) & PTR_MASK;
        uint32_t head = bus_rd32(ed + 8u);
        uint32_t next = bus_rd32(ed + 12u) & PTR_MASK;
        uint32_t va;
        if (!bus_va(ed, 16, &va)) {
            return;
        }
        if (one_each) {
            bool seen = false;
            for (uint32_t i = 0; i < pass->seen_count && !seen; i++) {
                seen = pass->seen[i] == ed;
            }
            if (seen) {
                ed = next;
                continue;
            }
            if (pass->seen_count < WALK_SEEN) {
                pass->seen[pass->seen_count++] = ed;
            }
        }
        if ((ed0 & ED_SKIP) == 0 && (head & ED_HALT) == 0) {
            for (uint32_t t = 0; (head & PTR_MASK) != tail && t < MAX_TDS; t++) {
                uint32_t td = head & PTR_MASK;
                uint32_t td_next = bus_rd32(td + 8u) & PTR_MASK;
                uint32_t info = bus_rd32(td);
                uint32_t packets = 1;
                uint32_t cc = do_td(hc, ed0, td, &packets);
                if (cc == CC_NAK) {
                    break;
                }
                // the data toggle: from the TD when its T says so, else the ED's carry
                uint32_t toggle = (info & (1u << 25)) ? (info >> 24) & 1u : (head & ED_CARRY) ? 1u : 0u;
                if (cc == CC_NOERROR) {
                    toggle ^= packets & 1u;
                }
                head = (head & ~ED_CARRY) | (toggle ? ED_CARRY : 0u);
                bus_wr32(td, (info & 0x03FFFFFFu) | (cc << 28)); // the code, the error count 0
                bus_wr32(td + 8u, pass->done);
                pass->done = td;
                head = td_next | (head & (ED_HALT | ED_CARRY));
                if (cc != CC_NOERROR) {
                    head |= ED_HALT;
                    break;
                }
                if (one_each) {
                    break;
                }
            }
            bus_wr32(ed + 8u, head);
        }
        ed = next;
    }
}

static void publish(Hc *hc, Pass *pass) {
    if (pass->done == 0) {
        return;
    }
    uint32_t hcca = hc->reg[HC_HCCA / 4u];
    if (hcca != 0) {
        bus_wr32(hcca + HCCA_DONE_HEAD, pass->done);
    }
    hc->reg[HC_DONE_HEAD / 4u] = pass->done;
    g_stats.done_queues++;
    hc_raise(hc, INTR_WDH);
    pass->done = 0;
}

// The control and bulk lists (and with `periodic` the periodic one), unless the driver still owns
// the last done queue.
static void run_lists(Hc *hc, bool periodic) {
    if (!operational(hc) || (hc->reg[HC_INTERRUPT_STATUS / 4u] & INTR_WDH) != 0) {
        return;
    }
    Pass pass;
    memset(&pass, 0, sizeof pass);
    uint32_t control = hc->reg[HC_CONTROL / 4u];
    uint32_t hcca = hc->reg[HC_HCCA / 4u];
    if (periodic && (control & CTL_PLE) != 0 && hcca != 0) {
        for (uint32_t slot = 0; slot < 32u; slot++) {
            uint32_t ed = bus_rd32(hcca + slot * 4u) & PTR_MASK;
            if (ed != 0 && !hc->periodic_logged) {
                hc->periodic_logged = true;
                WL_LOG_I(TAG, "OHCI%d: periodic list in use (slot %u: ED %08x info %08x)", hc->index, (unsigned)slot,
                         (unsigned)ed, (unsigned)bus_rd32(ed));
            }
            walk(hc, ed, &pass, true);
        }
    }
    if ((control & CTL_BLE) != 0) {
        walk(hc, hc->reg[HC_BULK_HEAD_ED / 4u] & PTR_MASK, &pass, false);
        hc->reg[HC_COMMAND_STATUS / 4u] &= ~CS_BLF;
    }
    if ((control & CTL_CLE) != 0) {
        walk(hc, hc->reg[HC_CONTROL_HEAD_ED / 4u] & PTR_MASK, &pass, false);
        hc->reg[HC_COMMAND_STATUS / 4u] &= ~CS_CLF;
    }
    publish(hc, &pass);
}

// ---- registers -------------------------------------------------------------------------------

static uint32_t frame_number(const Hc *hc) {
    if (hc->start_us == 0) {
        return 0;
    }
    return (uint32_t)((wl_xb_time_us() - hc->start_us) / 1000u) & 0xFFFFu;
}

static void hc_reset(Hc *hc) {
    hc->reg[HC_CONTROL / 4u] &= ~CTL_HCFS; // UsbReset
    hc->reg[HC_INTERRUPT_STATUS / 4u] = 0;
    hc->reg[HC_INTERRUPT_ENABLE / 4u] = 0;
    hc->reg[HC_COMMAND_STATUS / 4u] = 0;
    hc->reg[HC_DONE_HEAD / 4u] = 0;
    hc->reg[HC_FM_INTERVAL / 4u] = 0x27782EDFu;
    hc->reg[HC_PERIODIC_START / 4u] = 0x00003E67u;
    hc->reg[HC_LS_THRESHOLD / 4u] = 0x00000628u;
    hc->start_us = 0;
}

static uint32_t hc_read(void *user, uint32_t off, unsigned size) {
    Hc *hc = user;
    uint32_t a = off & ~3u;
    uint32_t v = a < HC_REG_END ? hc->reg[a / 4u] : 0;
    g_stats.reads++;
    switch (a) {
    case HC_FM_NUMBER:
        v = frame_number(hc);
        break;
    case HC_FM_REMAINING: {
        uint32_t us = (uint32_t)(wl_xb_time_us() % 1000u);
        v = (0x2EDFu - us * 12u) & 0x3FFFu;
        break;
    }
    case HC_RH_STATUS:
        v = 0; // no over-current, nothing to switch
        break;
    default:
        break;
    }
    if (size < 4) {
        v >>= (off & 3u) * 8u;
        v &= size == 1 ? 0xFFu : 0xFFFFu;
    }
    if (g_cfg.trace) {
        WL_LOG_I(TAG, "OHCI%d read +0x%02x = %08x", hc->index, (unsigned)off, (unsigned)v);
    }
    return v;
}

static void hc_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    Hc *hc = user;
    uint32_t a = off & ~3u;
    g_stats.writes++;
    if (a >= HC_REG_END) {
        return;
    }
    if (size < 4) { // a narrow write: merged into the register (none of the ones below takes one)
        uint32_t shift = (off & 3u) * 8u;
        uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        value = (hc->reg[a / 4u] & ~mask) | ((value << shift) & mask);
    }
    if (g_cfg.trace) {
        WL_LOG_I(TAG, "OHCI%d write +0x%02x = %08x", hc->index, (unsigned)off, (unsigned)value);
    }
    uint32_t *r = &hc->reg[a / 4u];
    switch (a) {
    case HC_REVISION:
    case HC_DONE_HEAD:
    case HC_FM_NUMBER:
    case HC_FM_REMAINING:
    case HC_RH_STATUS:
        return; // read-only, or the controller's
    case HC_CONTROL: {
        bool was = operational(hc);
        *r = value;
        if (!was && operational(hc)) {
            hc->start_us = wl_xb_time_us();
            if (!hc->running_logged) {
                hc->running_logged = true;
                WL_LOG_I(TAG, "OHCI%d operational (HcControl %08x, HCCA %08x)", hc->index, (unsigned)value,
                         (unsigned)hc->reg[HC_HCCA / 4u]);
            }
        }
        run_lists(hc, false);
        return;
    }
    case HC_COMMAND_STATUS:
        // HCR clears itself (a driver polls for it): leaving it set is a hang
        if (value & CS_HCR) {
            hc_reset(hc);
        }
        *r |= value & (CS_CLF | CS_BLF);
        run_lists(hc, false);
        return;
    case HC_INTERRUPT_STATUS:
        // acknowledging WDH hands the done queue back: its head reads 0 from now on (a driver that
        // checks the head before it trusts the interrupt walks a stale one again otherwise)
        if ((value & INTR_WDH) && (*r & INTR_WDH)) {
            uint32_t hcca = hc->reg[HC_HCCA / 4u];
            if (hcca != 0) {
                bus_wr32(hcca + HCCA_DONE_HEAD, 0);
            }
            hc->reg[HC_DONE_HEAD / 4u] = 0;
            *r &= ~value;
            run_lists(hc, false);
            return;
        }
        *r &= ~value;
        return;
    case HC_INTERRUPT_ENABLE:
        hc->reg[HC_INTERRUPT_ENABLE / 4u] |= value;
        return;
    case HC_INTERRUPT_DISABLE:
        hc->reg[HC_INTERRUPT_ENABLE / 4u] &= ~value;
        return;
    case HC_RH_DESCRIPTOR_A:
        *r = (*r & 0xFFu) | (value & ~0xFFu); // the port count is the controller's
        return;
    default:
        break;
    }
    if (a >= HC_RH_PORT_STATUS) {
        uint32_t root = (a - HC_RH_PORT_STATUS) / 4u;
        uint32_t *ps = port_reg(hc, root);
        // the change bits first: they speak of the state before this write (a driver acknowledges
        // the last reset and starts the next in one word, 0x01100010; xboxrecomp's DDS9 loop)
        *ps &= ~(value & 0xFFFF0000u);
        if (value & PORT_W_CLEAR_ENABLE) {
            *ps &= ~PORT_PES;
        }
        if ((value & PORT_W_SET_ENABLE) && (*ps & PORT_CCS)) {
            *ps |= PORT_PES;
        }
        if (value & PORT_W_SET_POWER) {
            *ps |= PORT_PPS;
        }
        if ((value & PORT_W_CLEAR_POWER) && root >= hc->ndp) {
            *ps &= ~PORT_PPS;
        }
        if (value & PORT_W_SET_RESET) {
            // a reset completes at once: a device there comes back enabled at address 0
            for (uint32_t p = 0; p < WL_XB_USB_PORTS && hc->index == 0; p++) {
                if (g_cfg.root_port[p] == root && g_pipe[p].attached) {
                    wl_xb_xid_reset(p);
                    g_pipe[p].setup_pending = false;
                }
            }
            if (*ps & PORT_CCS) {
                *ps |= PORT_PES;
            }
            *ps |= PORT_PRSC;
            hc_raise(hc, INTR_RHSC);
        }
        return;
    }
    *r = value;
}

static bool hc_irq(void *user) {
    const Hc *hc = user;
    uint32_t enable = hc->reg[HC_INTERRUPT_ENABLE / 4u];
    return (enable & INTR_MIE) != 0 && (hc->reg[HC_INTERRUPT_STATUS / 4u] & enable & INTR_SOURCES) != 0;
}

// ---- the frame tick --------------------------------------------------------------------------

void wl_xb_usb_frame(void) {
    if (!g_registered) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        Hc *hc = &g_hc[i];
        hotplug(hc);
        if (!operational(hc)) {
            continue;
        }
        uint32_t hcca = hc->reg[HC_HCCA / 4u];
        if (hcca != 0) {
            bus_wr32(hcca + HCCA_FRAME_NUMBER, frame_number(hc)); // the pad above it is zero
        }
        hc_raise(hc, INTR_SF);
        run_lists(hc, true);
    }
}

static void usb_vblank(void *user) {
    (void)user;
    wl_xb_usb_frame();
}

WlResult wl_xb_usb_register(const WlXbUsbConfig *cfg) {
    if (cfg != NULL) {
        g_cfg = *cfg;
    } else {
        wl_xb_usb_config_default(&g_cfg);
    }
    for (uint32_t p = 0; p < WL_XB_USB_PORTS; p++) {
        g_cfg.root_port[p] &= 3u;
    }
    memset(g_hc, 0, sizeof g_hc);
    memset(g_pipe, 0, sizeof g_pipe);
    memset(&g_stats, 0, sizeof g_stats);
    g_settle = 0;
    static const char *names[2] = {"usb0", "usb1"};
    for (int i = 0; i < 2; i++) {
        Hc *hc = &g_hc[i];
        hc->index = i;
        hc->base = i == 0 ? WL_XB_USB0_BASE : WL_XB_USB1_BASE;
        hc->line = i == 0 ? WL_XB_IRQ_USB0 : WL_XB_IRQ_USB1;
        hc->ndp = i == 0 ? 4u : 2u;
        hc->reg[HC_REVISION / 4u] = 0x10u; // OHCI 1.0
        hc_reset(hc);
        hc->reg[HC_RH_DESCRIPTOR_A / 4u] = hc->ndp | (1u << 9); // no power switching: always powered
        for (uint32_t p = 0; p < 4u; p++) {
            *port_reg(hc, p) = p < hc->ndp ? PORT_PPS : 0u;
        }
        WlXbDevice dev = {names[i], hc->base, WL_XB_USB_SIZE, hc_read, hc_write, hc};
        WlResult r = wl_xb_mmio_register(&dev);
        if (r != WL_OK) {
            return r;
        }
        wl_xb_irq_set_level(hc->line, hc_irq, hc);
    }
    for (uint32_t p = 0; p < WL_XB_USB_PORTS; p++) {
        g_pipe[p].answer_len = -1;
    }
    g_registered = true;
    return wl_xb_service_add_vblank_hook(usb_vblank, NULL);
}
