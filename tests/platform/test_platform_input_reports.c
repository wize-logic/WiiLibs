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

SUITE(input_reports_suite) {
    RUN_TEST(no_report_writes_nothing);
    RUN_TEST(a_button_in_any_report_is_pressed);
    RUN_TEST(the_pointer_is_the_rounded_mean_of_the_reports_with_one);
    RUN_TEST(no_pointer_in_the_frame_is_none);
    RUN_TEST(the_mean_does_not_overflow);
    RUN_TEST(the_acceleration_is_the_last_and_the_peak_by_magnitude);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(input_reports_suite);
    GREATEST_MAIN_END();
}
