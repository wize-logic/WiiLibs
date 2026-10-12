// SPDX-License-Identifier: MIT
// wl/platform/wii/input_wii.c compiled against stand-ins for libogc's WPAD, PAD and keyboard
// (tests/platform/input_stub): what the console backend reports for a Wii Remote with a Nunchuk or
// a Classic Controller, a GameCube pad, and a Remote that drops out for a moment.
#define _POSIX_C_SOURCE 199309L // (clock_gettime, for the cost check)
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "greatest.h"
#include "input_stub.h"
#include "wl/platform/platform.h"

static void reset(void) {
    memset(&g_input_stub, 0, sizeof g_input_stub);
    g_input_stub.motor_mode = 1; // (the console's rumble setting: on)
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

TEST rumble_off_in_the_console_settings_is_none(void) {
    reset();
    g_input_stub.motor_mode = 0;
    ASSERT(!wl_platform_rumble_enabled());
    wl_platform_rumble(0, true);
    wl_platform_pad_rumble(0, true);
    ASSERT_EQ(0, g_input_stub.rumble[0]);
    ASSERT_EQ(PAD_MOTOR_STOP, g_input_stub.motor[0]);
    // an unreadable setting allows it, as does "on"
    g_input_stub.motor_mode = -1;
    ASSERT(wl_platform_rumble_enabled());
    g_input_stub.motor_mode = 1;
    ASSERT(wl_platform_rumble_enabled());
    wl_platform_rumble(0, true);
    ASSERT_EQ(1, g_input_stub.rumble[0]);
    wl_platform_rumble(0, false);
    PASS();
}

TEST the_consoles_sound_setting_is_mono_stereo_or_surround(void) {
    reset();
    g_input_stub.sound_mode = 0;
    ASSERT_EQ(WL_SOUND_MONO, wl_platform_sound_mode());
    g_input_stub.sound_mode = 1;
    ASSERT_EQ(WL_SOUND_STEREO, wl_platform_sound_mode());
    g_input_stub.sound_mode = 2;
    ASSERT_EQ(WL_SOUND_SURROUND, wl_platform_sound_mode());
    // read at each call; a setting that cannot be read, or is not one of these, is stereo
    g_input_stub.sound_mode = 0;
    ASSERT_EQ(WL_SOUND_MONO, wl_platform_sound_mode());
    g_input_stub.sound_mode = -1;
    ASSERT_EQ(WL_SOUND_STEREO, wl_platform_sound_mode());
    g_input_stub.sound_mode = 7;
    ASSERT_EQ(WL_SOUND_STEREO, wl_platform_sound_mode());
    PASS();
}

TEST only_a_change_is_sent_to_a_motor(void) {
    reset();
    g_input_stub.motor_mode = 1;
    wl_platform_rumble(2, true);
    wl_platform_rumble(2, true);
    wl_platform_rumble(2, true);
    ASSERT_EQ(1u, g_input_stub.rumble_calls);
    wl_platform_rumble(2, false);
    wl_platform_rumble(2, false);
    ASSERT_EQ(2u, g_input_stub.rumble_calls);
    wl_platform_pad_rumble(3, true);
    wl_platform_pad_rumble(3, true);
    ASSERT_EQ(1u, g_input_stub.motor_calls);
    wl_platform_pad_rumble(3, false);
    PASS();
}

TEST a_remote_that_comes_back_is_told_again(void) {
    reset();
    g_input_stub.motor_mode = 1;
    remote(1, WPAD_EXP_NONE, 0);
    (void)poll();
    wl_platform_rumble(1, true);
    ASSERT_EQ(1, g_input_stub.rumble[1]);
    // it goes, and returns (its motor is off then): the next request is sent although the state
    // was "on" before
    g_input_stub.probe[1] = WPAD_ERR_NO_CONTROLLER;
    (void)poll();
    remote(1, WPAD_EXP_NONE, 0);
    (void)poll();
    g_input_stub.rumble[1] = 0;
    wl_platform_rumble(1, true);
    ASSERT_EQ(1, g_input_stub.rumble[1]);
    wl_platform_rumble(1, false);
    PASS();
}

TEST stopping_every_motor_stops_every_motor(void) {
    reset();
    g_input_stub.motor_mode = 1;
    (void)poll(); // (the input is up)
    wl_platform_rumble(0, true);
    wl_platform_rumble(3, true);
    wl_platform_pad_rumble(1, true);
    wl_platform_input_stop_rumble();
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(0, g_input_stub.rumble[i]);
        ASSERT_EQ(PAD_MOTOR_STOP, g_input_stub.motor[i]);
    }
    // and a motor asked for again afterwards is sent (the state was forgotten with the stop)
    wl_platform_rumble(0, true);
    ASSERT_EQ(1, g_input_stub.rumble[0]);
    wl_platform_input_shutdown();
    ASSERT_EQ(0, g_input_stub.rumble[0]);
    PASS();
}

// ---- every report of the frame (WPAD_ReadPending, input_reports.h) ----------------------------

// Queues one report on `chan`: the channel's data as set up, with these buttons, a pointer
// (ir_x < 0: none) and a g-force.
static void report(int chan, u32 buttons, float ir_x, float ir_y, float gx, float gy, float gz) {
    WPADData *r = &g_input_stub.queue[chan][g_input_stub.queue_len[chan]++];
    *r = g_input_stub.data[chan];
    r->btns_h = buttons;
    r->ir.valid = ir_x >= 0.0f;
    r->ir.raw_valid = r->ir.valid;
    r->ir.x = r->ir.ax = ir_x;
    r->ir.y = r->ir.ay = ir_y;
    r->gforce.x = gx;
    r->gforce.y = gy;
    r->gforce.z = gz;
}

TEST a_press_that_came_and_went_inside_the_frame_is_pressed_for_the_frame(void) {
    reset();
    remote(0, WPAD_EXP_NONE, 0);
    report(0, 0, 300, 200, 0, 0, 1);
    report(0, WPAD_BUTTON_A, 300, 200, 0, 0, 1);
    report(0, WPAD_BUTTON_A | WPAD_BUTTON_B, 300, 200, 0, 0, 1);
    report(0, 0, 300, 200, 0, 0, 1); // let go before the frame's last report
    report(0, 0, 300, 200, 0, 0, 1);
    WlRawInput in = poll();
    ASSERT(in.remotes[0].buttons & WL_WM_A);
    ASSERT(in.remotes[0].buttons & WL_WM_B);
    ASSERT_EQ(5, in.remotes[0].reports);
    // and it is released in the next frame, which has reports of nothing pressed
    report(0, 0, 300, 200, 0, 0, 1);
    report(0, 0, 300, 200, 0, 0, 1);
    in = poll();
    ASSERT_EQ(0u, in.remotes[0].buttons);
    ASSERT_EQ(2, in.remotes[0].reports);
    PASS();
}

TEST a_held_button_is_held_in_every_frame_and_a_late_release_is_seen_late(void) {
    reset();
    remote(0, WPAD_EXP_NUNCHUK, 0);
    for (int frame = 0; frame < 3; frame++) {
        for (int i = 0; i < 6; i++) {
            report(0, WPAD_BUTTON_B | WPAD_NUNCHUK_BUTTON_Z, 300, 200, 0, 0, 1);
        }
        WlRawInput in = poll();
        ASSERT(in.remotes[0].buttons & WL_WM_B);
        ASSERT(in.remotes[0].buttons & WL_NC_Z); // (the extension's half too)
    }
    // released mid-frame: still down for that frame, up in the next
    report(0, WPAD_BUTTON_B, 300, 200, 0, 0, 1);
    report(0, 0, 300, 200, 0, 0, 1);
    ASSERT(poll().remotes[0].buttons & WL_WM_B);
    report(0, 0, 300, 200, 0, 0, 1);
    ASSERT_EQ(0u, poll().remotes[0].buttons);
    PASS();
}

TEST a_frame_without_reports_repeats_the_last_snapshot(void) {
    reset();
    remote(0, WPAD_EXP_NONE, WPAD_BUTTON_A);
    g_input_stub.data[0].ir.valid = 1;
    g_input_stub.data[0].ir.x = 320.0f;
    g_input_stub.data[0].ir.y = 240.0f;
    WlRawInput in = poll(); // nothing queued
    ASSERT_EQ(0, in.remotes[0].reports);
    ASSERT(in.remotes[0].buttons & WL_WM_A);
    ASSERT(in.remotes[0].ir_valid);
    ASSERT_EQ(320, in.remotes[0].ir_x);
    PASS();
}

TEST the_pointer_is_the_mean_of_the_reports_that_had_one(void) {
    reset();
    remote(0, WPAD_EXP_NONE, 0);
    report(0, 0, 300, 200, 0, 0, 1);
    report(0, 0, -1, -1, 0, 0, 1); // the sensor bar lost for one report: not a zero
    report(0, 0, 304, 204, 0, 0, 1);
    report(0, 0, 308, 202, 0, 0, 1);
    WlRawInput in = poll();
    ASSERT(in.remotes[0].ir_valid);
    ASSERT_EQ(304, in.remotes[0].ir_x);
    ASSERT_EQ(202, in.remotes[0].ir_y);
    // a frame in which no report had a pointer has none, though the snapshot's flag was left set
    report(0, 0, -1, -1, 0, 0, 1);
    report(0, 0, -1, -1, 0, 0, 1);
    in = poll();
    ASSERT(!in.remotes[0].ir_valid);
    // a pointer off the screen (the unbounded position) averages too, and a half rounds away
    report(0, 0, 10, 10, 0, 0, 1);
    g_input_stub.queue[0][0].ir.valid = 0;
    g_input_stub.queue[0][0].ir.ax = -41.0f;
    g_input_stub.queue[0][0].ir.ay = 10.0f;
    report(0, 0, 10, 10, 0, 0, 1);
    g_input_stub.queue[0][1].ir.valid = 0;
    g_input_stub.queue[0][1].ir.ax = -40.0f;
    in = poll();
    ASSERT(in.remotes[0].ir_valid);
    ASSERT_EQ(-41, in.remotes[0].ir_x); // -40.5 to -41
    ASSERT_EQ(10, in.remotes[0].ir_y);
    PASS();
}

TEST the_acceleration_keeps_its_peak_and_its_last_value(void) {
    reset();
    remote(0, WPAD_EXP_NONE, 0);
    report(0, 0, 300, 200, 0.0f, 0.0f, 1.0f);
    report(0, 0, 300, 200, 2.5f, -1.0f, 0.5f); // the swing
    report(0, 0, 300, 200, 0.5f, 0.0f, 1.0f);
    WlRawInput in = poll();
    ASSERT_EQ(250, in.remotes[0].accel_peak[0]);
    ASSERT_EQ(-100, in.remotes[0].accel_peak[1]);
    ASSERT_EQ(50, in.remotes[0].accel_peak[2]);
    ASSERT_EQ(50, in.remotes[0].accel[0]); // the last
    ASSERT_EQ(100, in.remotes[0].accel[2]);
    PASS();
}

TEST a_report_with_an_error_is_not_read(void) {
    reset();
    remote(0, WPAD_EXP_NONE, 0);
    report(0, WPAD_BUTTON_A, 100, 100, 0, 0, 1);
    g_input_stub.queue[0][0].err = WPAD_ERR_TRANSFER;
    report(0, WPAD_BUTTON_B, 300, 200, 0, 0, 1);
    WlRawInput in = poll();
    ASSERT_EQ(1, in.remotes[0].reports);
    ASSERT(!(in.remotes[0].buttons & WL_WM_A));
    ASSERT(in.remotes[0].buttons & WL_WM_B);
    ASSERT_EQ(300, in.remotes[0].ir_x);
    PASS();
}

TEST the_reports_of_each_remote_stay_its_own(void) {
    reset();
    remote(0, WPAD_EXP_NONE, 0);
    remote(2, WPAD_EXP_NONE, 0);
    report(0, WPAD_BUTTON_A, 100, 100, 0, 0, 1);
    report(0, 0, 100, 100, 0, 0, 1);
    report(2, WPAD_BUTTON_1, 500, 400, 0, 0, 1);
    WlRawInput in = poll();
    ASSERT_EQ(WL_WM_A, in.remotes[0].buttons);
    ASSERT_EQ(WL_WM_1, in.remotes[2].buttons);
    ASSERT_EQ(100, in.remotes[0].ir_x);
    ASSERT_EQ(500, in.remotes[2].ir_x);
    ASSERT_EQ(2, in.remotes[0].reports);
    ASSERT_EQ(1, in.remotes[2].reports);
    PASS();
}

// Reading a frame's reports costs little: four Remotes with two dozen reports each (a long frame)
// are read in well under 0.1 ms on the host (the stand-in's copying and the test's queuing
// included, so a ceiling; the Wii's processor is slower, and tools/qemu_icount.py counts the
// instructions of the folding).
TEST reading_four_remotes_of_two_dozen_reports_is_cheap(void) {
    reset();
    for (int c = 0; c < 4; c++) {
        remote(c, WPAD_EXP_NUNCHUK, 0);
    }
    enum { FRAMES = 2000 };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < FRAMES; f++) {
        for (int c = 0; c < 4; c++) {
            for (int i = 0; i < 24; i++) {
                report(c, (u32)(i & 3), (float)(300 + i), 200.0f, 0.1f * (float)i, 0.0f, 1.0f);
            }
        }
        WlRawInput in = poll();
        ASSERT_EQ(24, in.remotes[3].reports);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3) / FRAMES;
    printf("(%.1f us a frame for 96 reports, queuing them included) ", us);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    ASSERT(us < 100.0);
#endif // (qemu's clock says nothing about the console's: tools/qemu_icount.py counts instructions)
    PASS();
}

// ---- a GameCube pad's momentary errors ----------------------------------------------------------

TEST a_pad_that_fails_to_transfer_keeps_its_last_good_state(void) {
    reset();
    g_input_stub.pad[1].err = PAD_ERR_NONE;
    g_input_stub.pad[1].button = PAD_BUTTON_A;
    g_input_stub.pad[1].stickX = 87;
    g_input_stub.pad[1].triggerR = 180;
    WlRawInput in = poll();
    ASSERT_EQ(255, in.pads[1].trigger[1]);
    // libogc hands back the failed pad with nothing in it
    memset(&g_input_stub.pad[1], 0, sizeof g_input_stub.pad[1]);
    g_input_stub.pad[1].err = PAD_ERR_TRANSFER;
    for (int i = 0; i < 5; i++) {
        in = poll();
        ASSERT(in.pads[1].connected);
        ASSERT(in.pads[1].buttons & WL_PAD_A);
        ASSERT_EQ(127, in.pads[1].stick[0][0]);
        ASSERT_EQ(255, in.pads[1].trigger[1]); // a held trigger stays held
    }
    // a good read ends it: the new state is the pad's
    g_input_stub.pad[1].err = PAD_ERR_NONE;
    g_input_stub.pad[1].button = 0;
    in = poll();
    ASSERT_EQ(0u, in.pads[1].buttons);
    ASSERT_EQ(0, in.pads[1].trigger[1]);
    PASS();
}

TEST a_pad_that_keeps_failing_is_gone_and_returns_with_a_good_read(void) {
    reset();
    g_input_stub.pad[0].err = PAD_ERR_NONE;
    g_input_stub.pad[0].button = PAD_BUTTON_B;
    ASSERT(poll().pads[0].connected);
    g_input_stub.pad[0].err = PAD_ERR_TRANSFER;
    int held = 0;
    while (poll().pads[0].connected) {
        ASSERT(++held < 100);
    }
    ASSERT(held >= 5 && held <= 20); // several, not one
    // gone stays gone, with nothing pressed, while it still fails
    WlRawInput in = poll();
    ASSERT(!in.pads[0].connected);
    ASSERT_EQ(0u, in.pads[0].buttons);
    // and a failure count starts again after a good read
    g_input_stub.pad[0].err = PAD_ERR_NONE;
    g_input_stub.pad[0].button = PAD_BUTTON_X;
    ASSERT(poll().pads[0].connected);
    g_input_stub.pad[0].err = PAD_ERR_TRANSFER;
    for (int i = 0; i < held; i++) {
        in = poll();
        ASSERT(in.pads[0].connected);
        ASSERT(in.pads[0].buttons & WL_PAD_X);
    }
    PASS();
}

TEST a_pad_never_seen_is_not_held_by_an_error(void) {
    reset();
    g_input_stub.pad[3].err = PAD_ERR_TRANSFER;
    ASSERT(!poll().pads[3].connected);
    // and an unplugged one is gone at once, its last state forgotten
    g_input_stub.pad[3].err = PAD_ERR_NONE;
    g_input_stub.pad[3].button = PAD_BUTTON_A;
    ASSERT(poll().pads[3].connected);
    g_input_stub.pad[3].err = PAD_ERR_NO_CONTROLLER;
    ASSERT(!poll().pads[3].connected);
    g_input_stub.pad[3].err = PAD_ERR_TRANSFER;
    ASSERT(!poll().pads[3].connected);
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
    RUN_TEST(rumble_off_in_the_console_settings_is_none);
    RUN_TEST(the_consoles_sound_setting_is_mono_stereo_or_surround);
    RUN_TEST(only_a_change_is_sent_to_a_motor);
    RUN_TEST(a_remote_that_comes_back_is_told_again);
    RUN_TEST(stopping_every_motor_stops_every_motor);
    RUN_TEST(a_press_that_came_and_went_inside_the_frame_is_pressed_for_the_frame);
    RUN_TEST(a_held_button_is_held_in_every_frame_and_a_late_release_is_seen_late);
    RUN_TEST(a_frame_without_reports_repeats_the_last_snapshot);
    RUN_TEST(the_pointer_is_the_mean_of_the_reports_that_had_one);
    RUN_TEST(the_acceleration_keeps_its_peak_and_its_last_value);
    RUN_TEST(a_report_with_an_error_is_not_read);
    RUN_TEST(the_reports_of_each_remote_stay_its_own);
    RUN_TEST(reading_four_remotes_of_two_dozen_reports_is_cheap);
    RUN_TEST(a_pad_that_fails_to_transfer_keeps_its_last_good_state);
    RUN_TEST(a_pad_that_keeps_failing_is_gone_and_returns_with_a_good_read);
    RUN_TEST(a_pad_never_seen_is_not_held_by_an_error);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(input_wii_suite);
    GREATEST_MAIN_END();
}
