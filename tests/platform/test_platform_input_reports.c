// SPDX-License-Identifier: MIT
// wl/platform/input_reports.c: a frame's Wii Remote reports folded into one: every button seen,
// the pointer's mean over the reports that had one, the acceleration's peak.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/input_reports.h"

static WlRemoteReport report(uint32_t buttons, int ir_x, int ir_y, int ax, int ay, int az) {
    WlRemoteReport r;
    memset(&r, 0, sizeof r);
    r.buttons = buttons;
    r.ir_valid = ir_x >= 0;
    r.ir_x = (int16_t)ir_x;
    r.ir_y = (int16_t)ir_y;
    r.accel[0] = (int16_t)ax;
    r.accel[1] = (int16_t)ay;
    r.accel[2] = (int16_t)az;
    return r;
}

TEST no_report_writes_nothing(void) {
    WlRemoteReports r;
    wl_remote_reports_begin(&r);
    WlRawRemote out;
    memset(&out, 0x5a, sizeof out);
    WlRawRemote before = out;
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(0, memcmp(&before, &out, sizeof out));
    PASS();
}

TEST a_button_in_any_report_is_pressed(void) {
    WlRemoteReports r;
    wl_remote_reports_begin(&r);
    WlRemoteReport a = report(0, 10, 10, 0, 0, 100);
    WlRemoteReport b = report(WL_WM_A | WL_NC_Z, 10, 10, 0, 0, 100);
    WlRemoteReport c = report(WL_WM_B, 10, 10, 0, 0, 100);
    wl_remote_reports_add(&r, &a);
    wl_remote_reports_add(&r, &b);
    wl_remote_reports_add(&r, &c);
    WlRawRemote out;
    memset(&out, 0, sizeof out);
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(WL_WM_A | WL_WM_B | WL_NC_Z, out.buttons);
    ASSERT_EQ(3, out.reports);
    // and a new frame begins with none
    wl_remote_reports_begin(&r);
    wl_remote_reports_add(&r, &a);
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(0u, out.buttons);
    ASSERT_EQ(1, out.reports);
    PASS();
}

TEST the_pointer_is_the_rounded_mean_of_the_reports_with_one(void) {
    WlRemoteReports r;
    wl_remote_reports_begin(&r);
    WlRemoteReport list[] = {report(0, 100, 50, 0, 0, 0), report(0, -1, -1, 0, 0, 0), report(0, 101, 52, 0, 0, 0)};
    for (int i = 0; i < 3; i++) {
        wl_remote_reports_add(&r, &list[i]);
    }
    WlRawRemote out;
    memset(&out, 0, sizeof out);
    wl_remote_reports_finish(&r, &out);
    ASSERT(out.ir_valid);
    ASSERT_EQ(101, out.ir_x); // 100.5 up
    ASSERT_EQ(51, out.ir_y);
    // positions off the screen average as they are, and a half rounds away from zero
    wl_remote_reports_begin(&r);
    WlRemoteReport neg[] = {report(0, 0, 0, 0, 0, 0), report(0, 0, 0, 0, 0, 0)};
    neg[0].ir_x = -41;
    neg[1].ir_x = -40;
    neg[0].ir_y = -700;
    neg[1].ir_y = 700;
    wl_remote_reports_add(&r, &neg[0]);
    wl_remote_reports_add(&r, &neg[1]);
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(-41, out.ir_x);
    ASSERT_EQ(0, out.ir_y);
    PASS();
}

TEST no_pointer_in_the_frame_is_none(void) {
    WlRemoteReports r;
    wl_remote_reports_begin(&r);
    WlRemoteReport a = report(0, -1, -1, 0, 0, 0);
    wl_remote_reports_add(&r, &a);
    wl_remote_reports_add(&r, &a);
    WlRawRemote out;
    memset(&out, 0, sizeof out);
    out.ir_valid = true; // (the snapshot's)
    out.ir_x = 77;
    wl_remote_reports_finish(&r, &out);
    ASSERT(!out.ir_valid);
    ASSERT_EQ(0, out.ir_x);
    PASS();
}

TEST the_mean_does_not_overflow(void) {
    WlRemoteReports r;
    wl_remote_reports_begin(&r);
    WlRemoteReport a = report(0, 32767, 32767, 0, 0, 0);
    for (int i = 0; i < 255; i++) {
        wl_remote_reports_add(&r, &a);
    }
    WlRawRemote out;
    memset(&out, 0, sizeof out);
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(32767, out.ir_x);
    ASSERT_EQ(255, out.reports);
    wl_remote_reports_add(&r, &a);
    wl_remote_reports_add(&r, &a);
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(255, out.reports); // saturates
    PASS();
}

TEST the_acceleration_is_the_last_and_the_peak_by_magnitude(void) {
    WlRemoteReports r;
    wl_remote_reports_begin(&r);
    WlRemoteReport list[] = {report(0, 1, 1, 0, 0, 100), report(0, 1, 1, -300, 50, 20), report(0, 1, 1, 100, 100, 100),
                             report(0, 1, 1, 0, 0, 90)};
    for (int i = 0; i < 4; i++) {
        wl_remote_reports_add(&r, &list[i]);
    }
    WlRawRemote out;
    memset(&out, 0, sizeof out);
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(-300, out.accel_peak[0]); // the whole report of greatest magnitude, not per axis
    ASSERT_EQ(50, out.accel_peak[1]);
    ASSERT_EQ(20, out.accel_peak[2]);
    ASSERT_EQ(0, out.accel[0]);
    ASSERT_EQ(90, out.accel[2]);
    // the extremes of 16 bits do not overflow the magnitude
    wl_remote_reports_begin(&r);
    WlRemoteReport big = report(0, 1, 1, -32768, 32767, -32768);
    WlRemoteReport small = report(0, 1, 1, 1, 1, 1);
    wl_remote_reports_add(&r, &small);
    wl_remote_reports_add(&r, &big);
    wl_remote_reports_add(&r, &small);
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(-32768, out.accel_peak[0]);
    PASS();
}

TEST the_nunchuks_readings_keep_the_last_sixteen_oldest_first(void) {
    WlRemoteReports r;
    wl_remote_reports_begin(&r);
    WlRawRemote out;
    memset(&out, 0, sizeof out);
    // 20 reports, 5 of them without a Nunchuk (a report that has none is not counted)
    for (int i = 0; i < 25; i++) {
        WlRemoteReport a = report(0, 1, 1, 0, 0, 0);
        a.nunchuk_valid = i % 5 != 4;
        a.nunchuk[1] = (int16_t)i;
        wl_remote_reports_add(&r, &a);
    }
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(25, out.reports);
    ASSERT_EQ(20, out.nunchuk_reports);
    ASSERT_EQ(16, out.nunchuk_samples);
    // the 20 readings were 0..3, 5..8, 10..13, 15..18, 20..23: the last 16 start at 6
    ASSERT_EQ(5, out.nunchuk_accel[0][1]);
    ASSERT_EQ(23, out.nunchuk_accel[15][1]);
    for (int i = 1; i < 16; i++) {
        ASSERT(out.nunchuk_accel[i][1] > out.nunchuk_accel[i - 1][1]);
    }
    // and fewer than sixteen are all kept, in order
    wl_remote_reports_begin(&r);
    for (int i = 0; i < 3; i++) {
        WlRemoteReport a = report(0, 1, 1, 0, 0, 0);
        a.nunchuk_valid = true;
        a.nunchuk[0] = (int16_t)(10 + i);
        wl_remote_reports_add(&r, &a);
    }
    wl_remote_reports_finish(&r, &out);
    ASSERT_EQ(3, out.nunchuk_samples);
    ASSERT_EQ(10, out.nunchuk_accel[0][0]);
    ASSERT_EQ(12, out.nunchuk_accel[2][0]);
    PASS();
}

static WlRawPad pad_with(uint16_t buttons, int stick_x) {
    WlRawPad p;
    memset(&p, 0, sizeof p);
    p.connected = true;
    p.buttons = buttons;
    p.stick[0][0] = (int8_t)stick_x;
    return p;
}

TEST a_press_inside_a_frame_is_pressed_for_the_look_and_released_after(void) {
    WlPadTracker t;
    wl_pad_tracker_init(&t);
    WlRawPad up = pad_with(0, 0), down = pad_with(WL_PAD_A | WL_PAD_Z, 40);
    WlRawPad out;
    uint32_t samples = 99;
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &up);
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &down); // one field of 60 Hz
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &up);
    wl_pad_tracker_take(&t, &out, &samples);
    ASSERT(out.connected);
    ASSERT_EQ(WL_PAD_A | WL_PAD_Z, out.buttons);
    ASSERT_EQ(0, out.stick[0][0]); // the latest sample's stick
    ASSERT_EQ(3u, samples);
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &up);
    wl_pad_tracker_take(&t, &out, &samples);
    ASSERT_EQ(0u, out.buttons);
    ASSERT_EQ(1u, samples);
    // a look with no sample in between sees the last state, held buttons included
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &down);
    wl_pad_tracker_take(&t, &out, &samples);
    wl_pad_tracker_take(&t, &out, &samples);
    ASSERT_EQ(WL_PAD_A | WL_PAD_Z, out.buttons);
    ASSERT_EQ(0u, samples);
    PASS();
}

TEST a_failed_sample_keeps_the_state_and_adds_no_press(void) {
    WlPadTracker t;
    wl_pad_tracker_init(&t);
    WlRawPad held = pad_with(WL_PAD_L, 5), out;
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &held);
    wl_pad_tracker_take(&t, &out, NULL);
    for (unsigned i = 0; i + 1 < WL_PAD_ERROR_GRACE_SAMPLES; i++) {
        wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_ERROR, NULL);
    }
    wl_pad_tracker_take(&t, &out, NULL);
    ASSERT(out.connected);
    ASSERT_EQ(WL_PAD_L, out.buttons);
    ASSERT_EQ(5, out.stick[0][0]);
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_ERROR, NULL);
    wl_pad_tracker_take(&t, &out, NULL);
    ASSERT(!out.connected);
    ASSERT_EQ(0u, out.buttons);
    // gone stays gone through more errors, and comes back with a good sample
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_ERROR, NULL);
    wl_pad_tracker_take(&t, &out, NULL);
    ASSERT(!out.connected);
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &held);
    wl_pad_tracker_take(&t, &out, NULL);
    ASSERT(out.connected);
    // an error count starts again after a good sample
    for (unsigned i = 0; i + 1 < WL_PAD_ERROR_GRACE_SAMPLES; i++) {
        wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_ERROR, NULL);
    }
    wl_pad_tracker_take(&t, &out, NULL);
    ASSERT(out.connected);
    PASS();
}

TEST an_absent_pad_is_gone_at_once_and_a_pad_never_seen_stays_absent(void) {
    WlPadTracker t;
    wl_pad_tracker_init(&t);
    WlRawPad down = pad_with(WL_PAD_B, 0), out;
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_ERROR, NULL);
    wl_pad_tracker_take(&t, &out, NULL);
    ASSERT(!out.connected);
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_OK, &down);
    wl_pad_tracker_sample(&t, WL_PAD_SAMPLE_ABSENT, NULL); // (pulled out)
    wl_pad_tracker_take(&t, &out, NULL);
    ASSERT(!out.connected);
    ASSERT_EQ(0u, out.buttons); // and its press is not remembered
    PASS();
}

SUITE(input_reports_suite) {
    RUN_TEST(no_report_writes_nothing);
    RUN_TEST(a_button_in_any_report_is_pressed);
    RUN_TEST(the_pointer_is_the_rounded_mean_of_the_reports_with_one);
    RUN_TEST(no_pointer_in_the_frame_is_none);
    RUN_TEST(the_mean_does_not_overflow);
    RUN_TEST(the_acceleration_is_the_last_and_the_peak_by_magnitude);
    RUN_TEST(the_nunchuks_readings_keep_the_last_sixteen_oldest_first);
    RUN_TEST(a_press_inside_a_frame_is_pressed_for_the_look_and_released_after);
    RUN_TEST(a_failed_sample_keeps_the_state_and_adds_no_press);
    RUN_TEST(an_absent_pad_is_gone_at_once_and_a_pad_never_seen_stays_absent);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(input_reports_suite);
    GREATEST_MAIN_END();
}
