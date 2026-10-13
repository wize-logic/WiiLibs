// SPDX-License-Identifier: MIT
// The Xbox gamepad state (wl/xboxrt/pad.h): its report both ways, the rumble report, merging,
// the Wii controllers' mappings (GameCube, Classic, Remote with Nunchuk and the pointer as the
// right stick), the rumble's duty cycle; and the ports (input.h): the host's controllers through
// the host stub's input (wl_host_input_set), what the harness lays over them, connections. Host
// and ppc-be.
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/input_host.h"
#include "wl/platform/input_shape.h"
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/pad.h"

TEST report_layout(void) {
    WlXboxPad p;
    memset(&p, 0, sizeof p);
    p.buttons = WL_XBOX_PAD_UP | WL_XBOX_PAD_RTHUMB;
    p.analog[WL_XBOX_PAD_B] = 0x80;
    p.analog[WL_XBOX_PAD_RT] = 0xFF;
    p.thumb[WL_XBOX_PAD_LX] = 0x1234;
    p.thumb[WL_XBOX_PAD_LY] = -2;
    p.thumb[WL_XBOX_PAD_RX] = -32768;
    p.thumb[WL_XBOX_PAD_RY] = 32767;
    uint8_t r[20];
    ASSERT_EQ(20u, wl_xbox_pad_report(&p, r));
    static const uint8_t want[20] = {0, 20, 0x81, 0, 0, 0x80, 0, 0, 0, 0, 0, 0xFF,
                                     0x34, 0x12, 0xFE, 0xFF, 0x00, 0x80, 0xFF, 0x7F};
    ASSERT_MEM_EQ(want, r, 20);
    WlXboxPad q;
    ASSERT(wl_xbox_pad_parse(r, 20, &q));
    ASSERT(wl_xbox_pad_equal(&p, &q));
    r[1] = 19;
    ASSERT_FALSE(wl_xbox_pad_parse(r, 20, &q));
    uint16_t left = 1, right = 1;
    static const uint8_t rumble[6] = {0, 6, 0xFF, 0xFF, 0x01, 0x80};
    ASSERT(wl_xbox_rumble_parse(rumble, 6, &left, &right));
    ASSERT_EQ(0xFFFFu, left);
    ASSERT_EQ(0x8001u, right);
    ASSERT_FALSE(wl_xbox_rumble_parse(rumble, 5, &left, &right));
    PASS();
}

TEST merge(void) {
    WlXboxPad a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.buttons = WL_XBOX_PAD_START;
    a.analog[WL_XBOX_PAD_A] = 100;
    a.thumb[WL_XBOX_PAD_LX] = 500;
    a.thumb[WL_XBOX_PAD_LY] = 600;
    b.buttons = WL_XBOX_PAD_BACK;
    b.analog[WL_XBOX_PAD_A] = 50;
    b.analog[WL_XBOX_PAD_X] = 255;
    b.thumb[WL_XBOX_PAD_LY] = -700;
    wl_xbox_pad_merge(&a, &b);
    ASSERT_EQ(WL_XBOX_PAD_START | WL_XBOX_PAD_BACK, a.buttons);
    ASSERT_EQ(100, a.analog[WL_XBOX_PAD_A]);
    ASSERT_EQ(255, a.analog[WL_XBOX_PAD_X]);
    ASSERT_EQ(500, a.thumb[WL_XBOX_PAD_LX]);  // 0 does not win
    ASSERT_EQ(-700, a.thumb[WL_XBOX_PAD_LY]); // a non-zero axis does
    PASS();
}

TEST gamecube(void) {
    WlRawPad gc;
    memset(&gc, 0, sizeof gc);
    gc.connected = true;
    gc.buttons = WL_PAD_A | WL_PAD_Z | WL_PAD_UP | WL_PAD_START | WL_PAD_R;
    gc.stick[0][0] = 127;
    gc.stick[1][1] = -127;
    gc.trigger[0] = 40;
    WlXboxPad p;
    wl_xbox_pad_from_gamecube(&gc, false, &p);
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_A]);
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_BLACK]);
    ASSERT_EQ(40, p.analog[WL_XBOX_PAD_LT]);
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_RT]); // the click: full travel
    ASSERT_EQ(WL_XBOX_PAD_UP | WL_XBOX_PAD_RTHUMB | WL_XBOX_PAD_START, p.buttons);
    ASSERT_EQ(32767, p.thumb[WL_XBOX_PAD_LX]);
    ASSERT_EQ(-32767, p.thumb[WL_XBOX_PAD_RY]);
    wl_xbox_pad_from_gamecube(&gc, true, &p); // in the menus the D-pad is only the D-pad
    ASSERT_EQ(WL_XBOX_PAD_UP | WL_XBOX_PAD_START, p.buttons);
    PASS();
}

TEST classic(void) {
    WlRawRemote r;
    memset(&r, 0, sizeof r);
    r.connected = true;
    r.extension = WL_EXT_CLASSIC;
    r.buttons = WL_CC_B | WL_CC_X | WL_CC_ZR | WL_CC_DOWN | WL_CC_MINUS;
    r.stick[1][0] = -127;
    r.trigger[1] = 77;
    WlXboxPad p;
    wl_xbox_pad_from_classic(&r, false, &p);
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_A]); // b is A by position
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_Y]); // x is Y
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_WHITE]);
    ASSERT_EQ(77, p.analog[WL_XBOX_PAD_RT]);
    ASSERT_EQ(WL_XBOX_PAD_DOWN | WL_XBOX_PAD_LTHUMB | WL_XBOX_PAD_BACK, p.buttons);
    ASSERT_EQ(-32767, p.thumb[WL_XBOX_PAD_RX]);
    PASS();
}

TEST remote_and_pointer(void) {
    WlRawRemote r;
    memset(&r, 0, sizeof r);
    r.connected = true;
    r.extension = WL_EXT_NUNCHUK;
    r.buttons = WL_WM_B | WL_NC_Z | WL_WM_UP | WL_NC_C;
    r.stick[0][1] = 127;
    WlPointer ptr = {true, 1.0f, -1.0f, 0}; // the top right corner
    WlXboxPad p;
    wl_xbox_pad_from_remote(&r, &ptr, WL_XBOX_POINTER_DEAD_ZONE, false, &p);
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_RT]);
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_LT]);
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_Y]); // the D-pad's up in play
    ASSERT_EQ(0, p.analog[WL_XBOX_PAD_B]);
    ASSERT_EQ(WL_XBOX_PAD_UP | WL_XBOX_PAD_LTHUMB, p.buttons);
    ASSERT_EQ(32767, p.thumb[WL_XBOX_PAD_LY]);
    ASSERT_EQ(32767, p.thumb[WL_XBOX_PAD_RX]); // pointing right: turn right at full rate
    ASSERT_EQ(32767, p.thumb[WL_XBOX_PAD_RY]); // pointing up: look up
    ptr.x = 0.2f;                              // within the dead zone: nothing
    ptr.y = 0.0f;
    wl_xbox_pad_from_remote(&r, &ptr, WL_XBOX_POINTER_DEAD_ZONE, false, &p);
    ASSERT_EQ(0, p.thumb[WL_XBOX_PAD_RX]);
    ptr.x = -0.675f; // half way out of it: a quarter rate (the squared ramp), to the left
    wl_xbox_pad_from_remote(&r, &ptr, WL_XBOX_POINTER_DEAD_ZONE, false, &p);
    ASSERT(p.thumb[WL_XBOX_PAD_RX] < -8000 && p.thumb[WL_XBOX_PAD_RX] > -8400);
    wl_xbox_pad_from_remote(&r, &ptr, WL_XBOX_POINTER_DEAD_ZONE, true, &p); // menus: B is back
    ASSERT_EQ(255, p.analog[WL_XBOX_PAD_B]);
    ASSERT_EQ(0, p.analog[WL_XBOX_PAD_Y]);
    ASSERT_EQ(0, p.thumb[WL_XBOX_PAD_RX]);
    PASS();
}

TEST rumble_duty(void) {
    WlXboxRumble d;
    memset(&d, 0, sizeof d);
    unsigned on = 0;
    uint64_t t = 1;
    for (int i = 0; i < 100; i++, t += 16683u) {
        on += wl_xbox_rumble_duty(&d, WL_XBOX_RUMBLE_FULL / 2u, t) ? 1u : 0u;
    }
    ASSERT(on >= 48 && on <= 52); // half strength: on one tick in two
    ASSERT_FALSE(wl_xbox_rumble_duty(&d, 0, t)); // 0 stops it at once
    ASSERT_FALSE(d.on);
    PASS();
}

TEST ports(void) {
    wl_xb_input_reset(true);
    WlRawInput raw;
    memset(&raw, 0, sizeof raw);
    raw.pads[2].connected = true;
    raw.pads[2].buttons = WL_PAD_B;
    wl_host_input_set(&raw);
    wl_xb_input_frame();
    ASSERT(wl_xb_input_connected(0)); // port 1 always has one
    ASSERT_FALSE(wl_xb_input_connected(1));
    ASSERT(wl_xb_input_connected(2)); // the host has a pad there
    WlXboxPad st;
    wl_xb_input_state(2, &st);
    ASSERT_EQ(255, st.analog[WL_XBOX_PAD_B]);
    // a port the harness drives is the harness's alone: the host's B is not merged in
    WlXboxPad s;
    memset(&s, 0, sizeof s);
    s.analog[WL_XBOX_PAD_A] = 255;
    wl_xb_input_set_script(2, &s);
    wl_xb_input_state(2, &st);
    ASSERT_EQ(255, st.analog[WL_XBOX_PAD_A]);
    ASSERT_EQ(0, st.analog[WL_XBOX_PAD_B]);
    wl_xb_input_set_script(2, NULL); // and the host's comes back once it lets go
    wl_xb_input_state(2, &st);
    ASSERT_EQ(255, st.analog[WL_XBOX_PAD_B]);
    wl_xb_input_set_mode(2, WL_XB_PORT_OFF);
    ASSERT_FALSE(wl_xb_input_connected(2));
    wl_xb_input_set_mode(3, WL_XB_PORT_ON);
    ASSERT(wl_xb_input_connected(3));
    // rumble goes to the host's pad of the same number
    wl_xb_input_set_mode(2, WL_XB_PORT_AUTO);
    wl_xb_input_rumble(2, 65535, 0);
    wl_xb_input_frame();
    ASSERT(wl_host_pad_rumble(2));
    wl_xb_input_rumble(2, 0, 0);
    wl_xb_input_frame();
    ASSERT_FALSE(wl_host_pad_rumble(2));
    memset(&raw, 0, sizeof raw);
    wl_host_input_set(&raw);
    wl_xb_input_reset(false);
    PASS();
}

SUITE(pad) {
    RUN_TEST(report_layout);
    RUN_TEST(merge);
    RUN_TEST(gamecube);
    RUN_TEST(classic);
    RUN_TEST(remote_and_pointer);
    RUN_TEST(rumble_duty);
    RUN_TEST(ports);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(pad);
    GREATEST_MAIN_END();
}
