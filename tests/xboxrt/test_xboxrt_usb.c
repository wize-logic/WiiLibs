// SPDX-License-Identifier: MIT
// The USB host controller model and its gamepads (wl/xboxrt/usb.h, input.h) driven the way a
// title's XAPI drives them: registers through the device layer, endpoint and transfer descriptors
// and the HCCA built in guest memory (little-endian, physical addresses), the done queue read
// back. The gamepad on Xbox port 1 arrives on root hub port 3 once the controller runs, is reset,
// enumerated (descriptors, address, configuration, XID), reports its state on the interrupt
// endpoint, takes rumble on its OUT endpoint, and leaves when its port is disconnected. Host and
// ppc-be.
#include <string.h>

#include "greatest.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/pad.h"
#include "wl/xboxrt/usb.h"

#define HC_CONTROL 0x04u
#define HC_COMMAND_STATUS 0x08u
#define HC_INTERRUPT_STATUS 0x0Cu
#define HC_INTERRUPT_ENABLE 0x10u
#define HC_INTERRUPT_DISABLE 0x14u
#define HC_HCCA 0x18u
#define HC_CONTROL_HEAD_ED 0x20u
#define HC_DONE_HEAD 0x30u
#define HC_FM_INTERVAL 0x34u
#define HC_RH_DESCRIPTOR_A 0x48u
#define HC_RH_PORT_STATUS 0x54u

#define DP_SETUP 0u
#define DP_OUT 1u
#define DP_IN 2u

static bool g_up;
static uint32_t g_base; // physical: HCCA, descriptors, buffers

static uint32_t hc_rd(uint32_t off) {
    uint64_t v = 0;
    wl_xb_mmio_read(WL_XB_USB0_BASE + off, 4, &v);
    return (uint32_t)v;
}

static void hc_wr(uint32_t off, uint32_t v) {
    wl_xb_mmio_write(WL_XB_USB0_BASE + off, v, 4);
}

static void pw32(uint32_t phys, uint32_t v) {
    wl_xb_wr32(WL_XB_WINDOW | phys, v);
}

static uint32_t pr32(uint32_t phys) {
    return wl_xb_rd32(WL_XB_WINDOW | phys);
}

#define HCCA (g_base)
#define ED_CTRL (g_base + 0x100u)
#define ED_INT (g_base + 0x120u)
#define ED_OUT (g_base + 0x140u)
#define TD(n) (g_base + 0x400u + (n) * 16u)
#define BUF_SETUP (g_base + 0x1000u)
#define BUF_DATA (g_base + 0x1100u)

static void up(void) {
    if (g_up) {
        return;
    }
    WlXbMemConfig mc;
    wl_xb_mem_config_default(&mc);
    if (wl_xb_mem_init(&mc) != WL_OK || wl_xb_fault_install() != WL_OK) {
        return;
    }
    if (wl_xb_phys_alloc(0x10000u, 0, 0xFFFFFFFFu, 0x1000u, &g_base) != WL_OK) {
        return;
    }
    wl_xb_input_reset(false);
    if (wl_xb_usb_register(NULL) != WL_OK) {
        return;
    }
    g_up = true;
}

static void td(uint32_t at, uint32_t dp, uint32_t toggle, bool rounding, uint32_t buf, uint32_t len, uint32_t next) {
    pw32(at, (dp << 19) | (toggle << 24) | (rounding ? 1u << 18 : 0u) | (0xFu << 28));
    pw32(at + 4u, len != 0 ? buf : 0u);
    pw32(at + 8u, next);
    pw32(at + 12u, len != 0 ? buf + len - 1u : 0u);
}

static uint32_t td_cc(uint32_t at) {
    return pr32(at) >> 28;
}

// One control transfer on address `fa`: SETUP, an IN data stage of `in_len` (or OUT of `out`),
// the status stage; returns the done queue's head (0: nothing done).
static uint32_t control(uint8_t fa, const uint8_t setup[8], uint32_t in_len, bool rounding, const uint8_t *out,
                        uint32_t out_len) {
    for (uint32_t i = 0; i < 8u; i++) {
        wl_xb_wr8(WL_XB_WINDOW | (BUF_SETUP + i), setup[i]);
    }
    for (uint32_t i = 0; i < out_len; i++) {
        wl_xb_wr8(WL_XB_WINDOW | (BUF_DATA + i), out[i]);
    }
    uint32_t n = 0;
    td(TD(n), DP_SETUP, 2, false, BUF_SETUP, 8, TD(n + 1u));
    n++;
    if (in_len != 0) {
        td(TD(n), DP_IN, 3, rounding, BUF_DATA, in_len, TD(n + 1u));
        n++;
        td(TD(n), DP_OUT, 3, false, 0, 0, TD(n + 1u)); // status
        n++;
    } else {
        if (out_len != 0) {
            td(TD(n), DP_OUT, 3, false, BUF_DATA, out_len, TD(n + 1u));
            n++;
        }
        td(TD(n), DP_IN, 3, false, 0, 0, TD(n + 1u)); // status
        n++;
    }
    td(TD(n), 0, 0, false, 0, 0, 0); // the tail, never moved
    pw32(ED_CTRL, (uint32_t)fa | (8u << 16));
    pw32(ED_CTRL + 4u, TD(n));
    pw32(ED_CTRL + 8u, TD(0));
    pw32(ED_CTRL + 12u, 0);
    pw32(HCCA + 0x84u, 0);
    hc_wr(HC_CONTROL_HEAD_ED, ED_CTRL);
    hc_wr(HC_COMMAND_STATUS, 0x2u); // ControlListFilled: walked at once
    return pr32(HCCA + 0x84u);
}

static void ack_wdh(void) {
    hc_wr(HC_INTERRUPT_STATUS, 0x2u);
}

TEST bring_up_and_plug_in(void) {
    up();
    ASSERT(g_up);
    ASSERT_EQ_FMT(0x10u, hc_rd(0x00), "%x");                // OHCI 1.0
    ASSERT_EQ_FMT(4u, hc_rd(HC_RH_DESCRIPTOR_A) & 0xFFu, "%u"); // four ports
    hc_wr(HC_COMMAND_STATUS, 1u);                            // reset
    ASSERT_EQ_FMT(0u, hc_rd(HC_COMMAND_STATUS) & 1u, "%u");  // done at once
    hc_wr(HC_HCCA, HCCA);
    hc_wr(HC_FM_INTERVAL, 0x27782EDFu);
    // not running yet: nothing is plugged in however long it waits
    for (int i = 0; i < 40; i++) {
        wl_xb_usb_frame();
    }
    ASSERT_EQ_FMT(0u, hc_rd(HC_RH_PORT_STATUS + 2u * 4u) & 1u, "%u");
    hc_wr(HC_INTERRUPT_ENABLE, 0x80000000u | 0x40u | 0x02u); // MIE, RHSC, WDH
    hc_wr(HC_CONTROL, 0x80u | 0x10u | 0x04u);                 // operational, control and periodic lists
    for (int i = 0; i < 40 && (hc_rd(HC_RH_PORT_STATUS + 2u * 4u) & 1u) == 0; i++) {
        wl_xb_usb_frame();
    }
    uint32_t ps = hc_rd(HC_RH_PORT_STATUS + 2u * 4u); // Xbox port 1 is root hub port 3
    ASSERT_EQ_FMT(0x00010101u, ps & 0x00010103u, "%08x");      // connected, changed, powered, not enabled
    ASSERT_EQ_FMT(0u, hc_rd(HC_RH_PORT_STATUS) & 1u, "%u");    // port 1 (Xbox port 3) is empty
    ASSERT(hc_rd(HC_INTERRUPT_STATUS) & 0x40u);                // root hub status change
    ASSERT(wl_xb_irq_lines() & (1u << WL_XB_IRQ_USB0));        // the line is up
    hc_wr(HC_INTERRUPT_STATUS, 0x40u);
    // reset the port and acknowledge the connect change in one word (DDS9's 0x01100010 pattern)
    hc_wr(HC_RH_PORT_STATUS + 2u * 4u, 0x00010010u);
    ps = hc_rd(HC_RH_PORT_STATUS + 2u * 4u);
    ASSERT_EQ_FMT(0x00100103u, ps & 0x00130103u, "%08x"); // enabled, reset changed, connect change cleared
    hc_wr(HC_RH_PORT_STATUS + 2u * 4u, 0x00100000u);
    hc_wr(HC_INTERRUPT_STATUS, 0x40u);
    ASSERT_EQ(0u, wl_xb_irq_lines() & (1u << WL_XB_IRQ_USB0));
    PASS();
}

TEST enumerate(void) {
    ASSERT(g_up);
    // GET_DESCRIPTOR(DEVICE) for 8 bytes at address 0
    static const uint8_t get_dev8[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00};
    uint32_t done = control(0, get_dev8, 8, false, NULL, 0);
    ASSERT_EQ_FMT(TD(2), done, "%08x"); // newest first: the status stage, then the data, then the setup
    ASSERT_EQ_FMT(TD(1), pr32(TD(2) + 8u), "%08x");
    ASSERT_EQ_FMT(TD(0), pr32(TD(1) + 8u), "%08x");
    ASSERT_EQ(0u, td_cc(TD(0)));
    ASSERT_EQ(0u, td_cc(TD(1)));
    ASSERT_EQ_FMT(0u, pr32(TD(1) + 4u), "%08x"); // CBP 0: all of it moved
    ASSERT_EQ(18, wl_xb_rd8(WL_XB_WINDOW | BUF_DATA));
    ASSERT_EQ(1, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 1u)));
    ASSERT_EQ(8, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 7u))); // bMaxPacketSize0
    ASSERT_EQ_FMT(TD(3), pr32(ED_CTRL + 8u) & ~0xFu, "%08x"); // the head reached the tail
    ASSERT(hc_rd(HC_INTERRUPT_STATUS) & 0x2u);                 // WDH
    ASSERT_EQ_FMT(TD(2), hc_rd(HC_DONE_HEAD), "%08x");
    // a second fill while the driver owns the done queue waits for its acknowledgement
    uint32_t again = control(0, get_dev8, 8, false, NULL, 0);
    ASSERT_EQ_FMT(0u, again, "%08x");
    ack_wdh();
    ASSERT_EQ_FMT(TD(2), pr32(HCCA + 0x84u), "%08x"); // walked at the acknowledgement
    ack_wdh();
    ASSERT_EQ_FMT(0u, hc_rd(HC_DONE_HEAD), "%08x");

    // the whole device descriptor into a 64-byte buffer without rounding: a short packet is a
    // data underrun, and the endpoint halts
    static const uint8_t get_dev64[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00};
    control(0, get_dev64, 64, false, NULL, 0);
    ASSERT_EQ(9u, td_cc(TD(1)));
    ASSERT(pr32(ED_CTRL + 8u) & 1u);
    ASSERT_EQ_FMT(BUF_DATA + 18u, pr32(TD(1) + 4u), "%08x"); // CBP after what moved
    ack_wdh();
    // with rounding it is fine
    control(0, get_dev64, 64, true, NULL, 0);
    ASSERT_EQ(0u, td_cc(TD(1)));
    ASSERT_EQ(0x5E, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 8u))); // Microsoft
    ASSERT_EQ(0x89, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 10u))); // Controller S
    ack_wdh();

    // SET_ADDRESS 5: takes effect at the status stage
    static const uint8_t set_addr[8] = {0x00, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00};
    control(0, set_addr, 0, false, NULL, 0);
    ASSERT_EQ(0u, td_cc(TD(1)));
    ack_wdh();
    uint8_t address, configuration;
    bool attached;
    wl_xb_usb_pad_info(0, &address, &configuration, &attached);
    ASSERT(attached);
    ASSERT_EQ(5, address);
    // nothing answers at address 0 any more
    control(0, get_dev8, 8, false, NULL, 0);
    ASSERT_EQ(5u, td_cc(TD(0))); // device not responding
    ack_wdh();
    // the configuration descriptor, SET_CONFIGURATION 1
    static const uint8_t get_cfg[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x20, 0x00};
    control(5, get_cfg, 32, true, NULL, 0);
    ASSERT_EQ(0x58, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 9u + 5u))); // the interface's class
    ASSERT_EQ(0x81, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 18u + 2u))); // endpoint 1 IN
    ack_wdh();
    static const uint8_t set_cfg[8] = {0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    control(5, set_cfg, 0, false, NULL, 0);
    ack_wdh();
    wl_xb_usb_pad_info(0, &address, &configuration, &attached);
    ASSERT_EQ(1, configuration);
    // XID descriptor (vendor, interface)
    static const uint8_t get_xid[8] = {0xC1, 0x06, 0x00, 0x42, 0x00, 0x00, 0x10, 0x00};
    control(5, get_xid, 16, true, NULL, 0);
    ASSERT_EQ(0u, td_cc(TD(1)));
    ASSERT_EQ(0x10, wl_xb_rd8(WL_XB_WINDOW | BUF_DATA));
    ASSERT_EQ(0x42, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 1u)));
    ASSERT_EQ(20, wl_xb_rd8(WL_XB_WINDOW | (BUF_DATA + 6u)));
    ack_wdh();
    // a string descriptor is stalled
    static const uint8_t get_str[8] = {0x80, 0x06, 0x01, 0x03, 0x09, 0x04, 0xFF, 0x00};
    control(5, get_str, 64, true, NULL, 0);
    ASSERT_EQ(4u, td_cc(TD(1)));
    ack_wdh();
    PASS();
}

TEST report_and_rumble(void) {
    ASSERT(g_up);
    // the interrupt endpoint in every slot of the HCCA's table, one TD queued
    pw32(ED_INT, 5u | (1u << 7) | (2u << 11) | (32u << 16));
    td(TD(10), DP_IN, 0, true, BUF_DATA + 0x100u, 32, TD(11));
    td(TD(11), 0, 0, false, 0, 0, 0);
    pw32(ED_INT + 4u, TD(11));
    pw32(ED_INT + 8u, TD(10));
    pw32(ED_INT + 12u, 0);
    for (uint32_t slot = 0; slot < 32u; slot++) {
        pw32(HCCA + slot * 4u, ED_INT);
    }
    WlXboxPad pad;
    memset(&pad, 0, sizeof pad);
    pad.analog[WL_XBOX_PAD_A] = 255;
    pad.buttons = WL_XBOX_PAD_START;
    pad.thumb[WL_XBOX_PAD_LX] = -1000;
    pad.thumb[WL_XBOX_PAD_RY] = 32767;
    wl_xb_input_set_script(0, &pad);
    uint32_t before = wl_xb_input_reports(0);
    pw32(HCCA + 0x84u, 0);
    wl_xb_usb_frame();
    ASSERT_EQ_FMT(TD(10), pr32(HCCA + 0x84u), "%08x"); // one report though it is in 32 slots
    ASSERT_EQ(before + 1u, wl_xb_input_reports(0));
    ASSERT_EQ(0u, td_cc(TD(10)));
    uint8_t rep[20];
    wl_xb_copy_out(rep, WL_XB_WINDOW | (BUF_DATA + 0x100u), 20);
    ASSERT_EQ(0, rep[0]);
    ASSERT_EQ(20, rep[1]);
    ASSERT_EQ(WL_XBOX_PAD_START, rep[2]);
    ASSERT_EQ(255, rep[4]);
    ASSERT_EQ(0x18, rep[12]); // -1000 = 0xFC18, little-endian
    ASSERT_EQ(0xFC, rep[13]);
    ASSERT_EQ(0xFF, rep[18]);
    ASSERT_EQ(0x7F, rep[19]);
    WlXboxPad back;
    ASSERT(wl_xbox_pad_parse(rep, 20, &back));
    ASSERT(wl_xbox_pad_equal(&back, &pad));
    ASSERT(pr32(ED_INT + 8u) & 2u); // the toggle carried
    ack_wdh();
    // nothing queued: nothing done
    pw32(HCCA + 0x84u, 0);
    wl_xb_usb_frame();
    ASSERT_EQ_FMT(0u, pr32(HCCA + 0x84u), "%08x");

    // rumble on endpoint 2
    for (uint32_t slot = 0; slot < 32u; slot++) {
        pw32(HCCA + slot * 4u, 0);
    }
    static const uint8_t rumble[6] = {0x00, 0x06, 0x34, 0x12, 0x78, 0x56};
    for (uint32_t i = 0; i < 6u; i++) {
        wl_xb_wr8(WL_XB_WINDOW | (BUF_DATA + 0x200u + i), rumble[i]);
    }
    pw32(ED_OUT, 5u | (2u << 7) | (1u << 11) | (32u << 16));
    td(TD(12), DP_OUT, 0, false, BUF_DATA + 0x200u, 6, TD(13));
    td(TD(13), 0, 0, false, 0, 0, 0);
    pw32(ED_OUT + 4u, TD(13));
    pw32(ED_OUT + 8u, TD(12));
    pw32(ED_OUT + 12u, 0);
    pw32(HCCA, ED_OUT);
    wl_xb_usb_frame();
    uint16_t left, right;
    wl_xb_input_last_rumble(0, &left, &right);
    ASSERT_EQ_FMT(0x1234u, (unsigned)left, "%x");
    ASSERT_EQ_FMT(0x5678u, (unsigned)right, "%x");
    ack_wdh();
    // and as SET_REPORT on the control pipe
    static const uint8_t set_report[8] = {0x21, 0x09, 0x00, 0x02, 0x00, 0x00, 0x06, 0x00};
    static const uint8_t stop[6] = {0x00, 0x06, 0x00, 0x00, 0x00, 0x00};
    control(5, set_report, 0, false, stop, 6);
    wl_xb_input_last_rumble(0, &left, &right);
    ASSERT_EQ(0u, left);
    ASSERT_EQ(0u, right);
    ack_wdh();
    wl_xb_input_set_script(0, NULL);
    PASS();
}

TEST unplug_and_plug_again(void) {
    ASSERT(g_up);
    pw32(HCCA, 0);
    wl_xb_input_set_mode(0, WL_XB_PORT_OFF);
    ASSERT(!wl_xb_input_connected(0));
    wl_xb_usb_frame();
    uint32_t ps = hc_rd(HC_RH_PORT_STATUS + 2u * 4u);
    ASSERT_EQ_FMT(0x00030000u, ps & 0x00030003u, "%08x"); // gone and disabled, both changes set
    ASSERT(hc_rd(HC_INTERRUPT_STATUS) & 0x40u);
    hc_wr(HC_RH_PORT_STATUS + 2u * 4u, 0x00030000u);
    hc_wr(HC_INTERRUPT_STATUS, 0x40u);
    // port 2 gets a controller as well as port 1 coming back: one at a time
    wl_xb_input_set_mode(0, WL_XB_PORT_AUTO);
    wl_xb_input_set_mode(1, WL_XB_PORT_ON);
    for (int i = 0; i < 40; i++) {
        wl_xb_usb_frame();
    }
    ASSERT_EQ(1u, hc_rd(HC_RH_PORT_STATUS + 2u * 4u) & 1u);
    ASSERT_EQ(0u, hc_rd(HC_RH_PORT_STATUS + 3u * 4u) & 1u); // waits for the first to be configured
    WlXbUsbStats st;
    wl_xb_usb_stats(&st);
    ASSERT_EQ(1u, st.unplugged);
    ASSERT(st.plugged >= 2u);
    ASSERT(st.reports >= 1u);
    PASS();
}

SUITE(usb) {
    RUN_TEST(bring_up_and_plug_in);
    RUN_TEST(enumerate);
    RUN_TEST(report_and_rumble);
    RUN_TEST(unplug_and_plug_again);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(usb);
    GREATEST_MAIN_END();
}
