// SPDX-License-Identifier: MIT
// wl/platform/wii/input_wii.c compiled against stand-ins for libogc's WPAD, PAD and keyboard
// (tests/platform/input_stub): what the console backend reports for a Wii Remote with a Nunchuk or
// a Classic Controller, a GameCube pad, and a Remote that drops out for a moment.
#include <string.h>

#include "greatest.h"
#include "input_stub.h"
#include "wl/platform/platform.h"

static void reset(void) {
    memset(&g_input_stub, 0, sizeof g_input_stub);
    for (int i = 0; i < 4; i++) {
        g_input_stub.probe[i] = WPAD_ERR_NO_CONTROLLER;
        g_input_stub.pad[i].err = PAD_ERR_NO_CONTROLLER;
    }
}

static WlRawInput poll(void) {
    WlRawInput in;
    wl_platform_input_poll(&in);
    return in;
}

static void remote(int chan, u32 type, u32 buttons) {
    g_input_stub.probe[chan] = WPAD_ERR_NONE;
    g_input_stub.probe_type[chan] = type;
    g_input_stub.data[chan].err = WPAD_ERR_NONE;
    g_input_stub.data[chan].btns_h = buttons;
    g_input_stub.data[chan].battery_level = 200;
}

TEST classic_controller_sticks_and_triggers(void) {
    reset();
    remote(0, WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_ZL | WPAD_BUTTON_HOME);
    struct classic_ctrl_t *c = &g_input_stub.data[0].exp.classic;
    c->ljs.ang = 90.0f; // right
    c->ljs.mag = 1.0f;
    c->rjs.ang = 0.0f; // up
    c->rjs.mag = 0.5f;
    c->ls_raw = 31;
    c->rs_raw = 16;
    WlRawInput in = poll();
    ASSERT(in.remotes[0].connected);
    ASSERT_EQ(WL_EXT_CLASSIC, in.remotes[0].extension);
    ASSERT(in.remotes[0].buttons & WL_CC_A);
    ASSERT(in.remotes[0].buttons & WL_CC_ZL);
    ASSERT(in.remotes[0].buttons & WL_WM_HOME);
    ASSERT_EQ(127, in.remotes[0].stick[0][0]);
    ASSERT_EQ(0, in.remotes[0].stick[0][1]);
    ASSERT_EQ(0, in.remotes[0].stick[1][0]);
    ASSERT_EQ(64, in.remotes[0].stick[1][1]);
    ASSERT_EQ(255, in.remotes[0].trigger[0]); // five bits scaled to a byte
    ASSERT_EQ(132, in.remotes[0].trigger[1]);
    ASSERT(!in.remotes[1].connected);
    PASS();
}

TEST classic_pro_triggers_are_the_digital_bits(void) {
    reset();
    remote(1, WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_FULL_L);
    WlRawInput in = poll();
    ASSERT(in.remotes[1].connected);
    ASSERT(in.remotes[1].buttons & WL_CC_L);
    ASSERT(!(in.remotes[1].buttons & WL_CC_R));
    ASSERT_EQ(0, in.remotes[1].trigger[0]);
    PASS();
}

TEST nunchuk_stick_and_buttons(void) {
    reset();
    remote(0, WPAD_EXP_NUNCHUK, WPAD_BUTTON_B | WPAD_NUNCHUK_BUTTON_Z | WPAD_NUNCHUK_BUTTON_C);
    g_input_stub.data[0].exp.nunchuk.js.ang = 180.0f; // down
    g_input_stub.data[0].exp.nunchuk.js.mag = 1.0f;
    WlRawInput in = poll();
    ASSERT_EQ(WL_EXT_NUNCHUK, in.remotes[0].extension);
    ASSERT(in.remotes[0].buttons & WL_WM_B);
    ASSERT(in.remotes[0].buttons & WL_NC_Z);
    ASSERT(in.remotes[0].buttons & WL_NC_C);
    ASSERT_IN_RANGE(0, in.remotes[0].stick[0][0], 1);
    ASSERT_EQ(-127, in.remotes[0].stick[0][1]); // y is up
    PASS();
}

TEST pointer_is_the_bounded_position_else_the_unbounded(void) {
    reset();
    remote(0, WPAD_EXP_NONE, 0);
    g_input_stub.data[0].ir.valid = 1;
    g_input_stub.data[0].ir.x = 600.0f;
    g_input_stub.data[0].ir.y = 100.0f;
    WlRawInput in = poll();
    ASSERT(in.remotes[0].ir_valid);
    ASSERT_EQ(600, in.remotes[0].ir_x);
    ASSERT_EQ(100, in.remotes[0].ir_y);
    g_input_stub.data[0].ir.valid = 0;
    g_input_stub.data[0].ir.raw_valid = 1;
    g_input_stub.data[0].ir.ax = 700.0f; // off the right edge, the bar still seen
    g_input_stub.data[0].ir.ay = -40.0f;
    in = poll();
    ASSERT(in.remotes[0].ir_valid);
    ASSERT_EQ(700, in.remotes[0].ir_x);
    ASSERT_EQ(-40, in.remotes[0].ir_y);
    g_input_stub.data[0].ir.raw_valid = 0;
    in = poll();
    ASSERT(!in.remotes[0].ir_valid);
    PASS();
}

TEST gamecube_sticks_reach_full_over_their_travel(void) {
    reset();
    g_input_stub.pad[2].err = PAD_ERR_NONE;
    g_input_stub.pad[2].button = PAD_BUTTON_A | PAD_TRIGGER_Z;
    g_input_stub.pad[2].stickX = 87; // the gate's end
    g_input_stub.pad[2].stickY = -87;
    g_input_stub.pad[2].substickX = -74;
    g_input_stub.pad[2].substickY = 37;
    g_input_stub.pad[2].triggerL = 30;
    g_input_stub.pad[2].triggerR = 180;
    WlRawInput in = poll();
    ASSERT(in.pads[2].connected);
    ASSERT(!in.pads[0].connected);
    ASSERT(in.pads[2].buttons & WL_PAD_A);
    ASSERT(in.pads[2].buttons & WL_PAD_Z);
    // a corner of the square comes out on the circle, as a gate's diagonal does
    ASSERT_EQ(90, in.pads[2].stick[0][0]);
    ASSERT_EQ(-90, in.pads[2].stick[0][1]);
    ASSERT_EQ(-114, in.pads[2].stick[1][0]);
    ASSERT_EQ(57, in.pads[2].stick[1][1]);
    ASSERT_EQ(0, in.pads[2].trigger[0]);
    ASSERT_EQ(255, in.pads[2].trigger[1]);
    // a cardinal at the end of the travel is full
    g_input_stub.pad[2].stickX = 87;
    g_input_stub.pad[2].stickY = 0;
    g_input_stub.pad[2].triggerL = 105;
    in = poll();
    ASSERT_EQ(127, in.pads[2].stick[0][0]);
    ASSERT_EQ(0, in.pads[2].stick[0][1]);
    ASSERT_EQ(128, in.pads[2].trigger[0]);
    PASS();
}

TEST gamecube_pad_stays_through_a_momentary_error(void) {
    reset();
    g_input_stub.pad[0].err = PAD_ERR_NONE;
    ASSERT(poll().pads[0].connected);
    g_input_stub.pad[0].err = PAD_ERR_NOT_READY;
    ASSERT(poll().pads[0].connected);
    g_input_stub.pad[0].err = PAD_ERR_NO_CONTROLLER;
    ASSERT(!poll().pads[0].connected);
    PASS();
}

TEST remote_stays_through_an_extension_being_plugged_in(void) {
    reset();
    remote(0, WPAD_EXP_CLASSIC, 0);
    ASSERT_EQ(WL_EXT_CLASSIC, poll().remotes[0].extension);
    g_input_stub.probe[0] = WPAD_ERR_NOT_READY;
    for (int i = 0; i < 120; i++) {
        WlRawInput in = poll();
        ASSERT(in.remotes[0].connected);
        ASSERT_EQ(WL_EXT_CLASSIC, in.remotes[0].extension);
        ASSERT_EQ(0u, in.remotes[0].buttons);
    }
    ASSERT(!poll().remotes[0].connected); // not ready for too long: gone
    // and it is a new connection once data returns
    unsigned calls = g_input_stub.data_format_calls;
    remote(0, WPAD_EXP_NUNCHUK, 0);
    WlRawInput in = poll();
    ASSERT(in.remotes[0].connected);
    ASSERT_EQ(WL_EXT_NUNCHUK, in.remotes[0].extension);
    ASSERT(g_input_stub.data_format_calls > calls);
    PASS();
}

TEST remote_that_is_gone_is_gone_at_once(void) {
    reset();
    remote(0, WPAD_EXP_NONE, 0);
    ASSERT(poll().remotes[0].connected);
    g_input_stub.probe[0] = WPAD_ERR_NO_CONTROLLER;
    ASSERT(!poll().remotes[0].connected);
    // a Remote never seen is not held when it reads NOT_READY (still pairing)
    g_input_stub.probe[3] = WPAD_ERR_NOT_READY;
    ASSERT(!poll().remotes[3].connected);
    PASS();
}

TEST rumble_reaches_the_devices(void) {
    reset();
    wl_platform_rumble(1, true);
    ASSERT_EQ(1, g_input_stub.rumble[1]);
    wl_platform_rumble(1, false);
    ASSERT_EQ(0, g_input_stub.rumble[1]);
    wl_platform_pad_rumble(2, true);
    ASSERT_EQ(PAD_MOTOR_RUMBLE, g_input_stub.motor[2]);
    wl_platform_pad_rumble(2, false);
    ASSERT_EQ(PAD_MOTOR_STOP, g_input_stub.motor[2]);
    PASS();
}

SUITE(input_wii_suite) {
    RUN_TEST(classic_controller_sticks_and_triggers);
    RUN_TEST(classic_pro_triggers_are_the_digital_bits);
    RUN_TEST(nunchuk_stick_and_buttons);
    RUN_TEST(pointer_is_the_bounded_position_else_the_unbounded);
    RUN_TEST(gamecube_sticks_reach_full_over_their_travel);
    RUN_TEST(gamecube_pad_stays_through_a_momentary_error);
    RUN_TEST(remote_stays_through_an_extension_being_plugged_in);
    RUN_TEST(remote_that_is_gone_is_gone_at_once);
    RUN_TEST(rumble_reaches_the_devices);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(input_wii_suite);
    GREATEST_MAIN_END();
}
