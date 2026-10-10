// SPDX-License-Identifier: MIT
// wl/platform/input_shape.c: sticks over their real travel, trigger ranges, 16-bit axes and the
// Remote's pointer (position, a short loss held, the turn ramp).
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/input_shape.h"

TEST stick_scales_over_the_travel_and_clamps(void) {
    ASSERT_EQ(0, wl_input_stick(0, WL_PAD_MAIN_STICK_FULL));
    ASSERT_EQ(127, wl_input_stick(WL_PAD_MAIN_STICK_FULL, WL_PAD_MAIN_STICK_FULL));
    ASSERT_EQ(-127, wl_input_stick(-WL_PAD_MAIN_STICK_FULL, WL_PAD_MAIN_STICK_FULL));
    ASSERT_EQ(127, wl_input_stick(120, WL_PAD_MAIN_STICK_FULL)); // past the travel
    ASSERT_EQ(-127, wl_input_stick(-128, WL_PAD_MAIN_STICK_FULL));
    ASSERT_EQ(63, wl_input_stick(43, 87)); // half of the travel is half
    ASSERT_EQ(0, wl_input_stick(50, 0));   // no division by zero
    PASS();
}

TEST stick_pair_is_a_circle(void) {
    int8_t out[2];
    wl_input_stick_pair(0, 0, 87, out);
    ASSERT_EQ(0, out[0]);
    ASSERT_EQ(0, out[1]);
    wl_input_stick_pair(87, 0, 87, out);
    ASSERT_EQ(127, out[0]);
    ASSERT_EQ(0, out[1]);
    wl_input_stick_pair(0, -87, 87, out);
    ASSERT_EQ(0, out[0]);
    ASSERT_EQ(-127, out[1]);
    // the corner of the square (a gate never reaches it) comes out at the circle: 127 long
    wl_input_stick_pair(87, 87, 87, out);
    ASSERT_EQ(out[0], out[1]);
    float length = sqrtf((float)out[0] * (float)out[0] + (float)out[1] * (float)out[1]);
    ASSERT(fabsf(length - 127.0f) < 1.5f);
    // a diagonal inside the circle is kept as it is
    wl_input_stick_pair(30, -40, 87, out);
    ASSERT(out[0] > 0 && out[1] < 0);
    ASSERT_EQ(wl_input_stick(30, 87), out[0]);
    ASSERT_EQ(wl_input_stick(-40, 87), out[1]);
    PASS();
}

TEST triggers_run_from_first_to_full(void) {
    // the GameCube's: 0 below 30, 255 from 180
    ASSERT_EQ(0, wl_input_trigger(0, WL_PAD_TRIGGER_FIRST, WL_PAD_TRIGGER_FULL));
    ASSERT_EQ(0, wl_input_trigger(30, WL_PAD_TRIGGER_FIRST, WL_PAD_TRIGGER_FULL));
    ASSERT_EQ(255, wl_input_trigger(180, WL_PAD_TRIGGER_FIRST, WL_PAD_TRIGGER_FULL));
    ASSERT_EQ(255, wl_input_trigger(255, WL_PAD_TRIGGER_FIRST, WL_PAD_TRIGGER_FULL));
    ASSERT_EQ(128, wl_input_trigger(105, WL_PAD_TRIGGER_FIRST, WL_PAD_TRIGGER_FULL));
    // the Classic Controller's five bits
    ASSERT_EQ(0, wl_input_trigger(0, 0, WL_CLASSIC_TRIGGER_FULL));
    ASSERT_EQ(255, wl_input_trigger(31, 0, WL_CLASSIC_TRIGGER_FULL));
    ASSERT_EQ(8, wl_input_trigger(1, 0, WL_CLASSIC_TRIGGER_FULL));
    // monotonic over the whole range
    unsigned last = 0;
    for (unsigned v = 0; v <= 31; v++) {
        unsigned t = wl_input_trigger(v, 0, WL_CLASSIC_TRIGGER_FULL);
        ASSERT(t >= last);
        last = t;
    }
    ASSERT_EQ(0, wl_input_trigger(10, 20, 20)); // an empty range
    PASS();
}

TEST axis16_reaches_the_extremes(void) {
    ASSERT_EQ(0, wl_input_axis16(0));
    ASSERT_EQ(32767, wl_input_axis16(127));
    ASSERT_EQ(-32767, wl_input_axis16(-127));
    ASSERT_EQ(-32768, wl_input_axis16(-128));
    ASSERT_EQ(258, wl_input_axis16(1));
    PASS();
}

static WlRawRemote remote_at(int x, int y, bool valid) {
    WlRawRemote r;
    memset(&r, 0, sizeof r);
    r.connected = true;
    r.ir_valid = valid;
    r.ir_x = (int16_t)x;
    r.ir_y = (int16_t)y;
    return r;
}

TEST pointer_is_the_ir_position_across_the_screen(void) {
    WlPointer p;
    memset(&p, 0, sizeof p);
    WlRawRemote r = remote_at(320, 240, true);
    ASSERT(wl_pointer_update(&p, &r, 1000, 100000));
    ASSERT_IN_RANGE(0.0f, p.x, 0.0001f);
    ASSERT_IN_RANGE(0.0f, p.y, 0.0001f);
    r = remote_at(640, 0, true); // the top right corner: right and up (y is down)
    ASSERT(wl_pointer_update(&p, &r, 2000, 100000));
    ASSERT_IN_RANGE(1.0f, p.x, 0.0001f);
    ASSERT_IN_RANGE(-1.0f, p.y, 0.0001f);
    r = remote_at(-300, 900, true); // beyond the screen: clamped
    ASSERT(wl_pointer_update(&p, &r, 3000, 100000));
    ASSERT_IN_RANGE(-1.0f, p.x, 0.0001f);
    ASSERT_IN_RANGE(1.0f, p.y, 0.0001f);
    PASS();
}

TEST pointer_loss_is_held_briefly_then_ends(void) {
    WlPointer p;
    memset(&p, 0, sizeof p);
    WlRawRemote r = remote_at(480, 240, true);
    ASSERT(wl_pointer_update(&p, &r, 1000000, 150000));
    r = remote_at(0, 0, false); // the sensor bar lost
    ASSERT(wl_pointer_update(&p, &r, 1100000, 150000)); // 100 ms: held
    ASSERT_IN_RANGE(0.5f, p.x, 0.0001f);
    ASSERT(!wl_pointer_update(&p, &r, 1200000, 150000)); // 200 ms: gone
    ASSERT(!p.tracking);
    ASSERT(!wl_pointer_update(&p, &r, 1300000, 150000)); // and stays gone
    r = remote_at(100, 100, true); // seen again
    ASSERT(wl_pointer_update(&p, &r, 1400000, 150000));
    ASSERT(p.x < 0.0f);
    PASS();
}

TEST pointer_never_tracks_before_it_is_seen_or_without_a_remote(void) {
    WlPointer p;
    memset(&p, 0, sizeof p);
    WlRawRemote r = remote_at(0, 0, false);
    ASSERT(!wl_pointer_update(&p, &r, 10, 150000));
    r = remote_at(320, 240, true);
    ASSERT(wl_pointer_update(&p, &r, 20, 150000));
    r.connected = false; // the Remote left
    ASSERT(!wl_pointer_update(&p, &r, 30, 150000));
    r.connected = true;
    r.ir_valid = false;
    ASSERT(!wl_pointer_update(&p, &r, 40, 150000)); // not held across a disconnect
    PASS();
}

TEST pointer_turn_has_a_dead_zone_and_a_ramp(void) {
    ASSERT_IN_RANGE(0.0f, wl_pointer_turn(0.0f, 0.35f), 0.0f);
    ASSERT_IN_RANGE(0.0f, wl_pointer_turn(0.35f, 0.35f), 0.0f);
    ASSERT_IN_RANGE(0.0f, wl_pointer_turn(-0.2f, 0.35f), 0.0f);
    ASSERT_IN_RANGE(1.0f, wl_pointer_turn(1.0f, 0.35f), 0.0001f);
    ASSERT_IN_RANGE(-1.0f, wl_pointer_turn(-1.0f, 0.35f), 0.0001f);
    ASSERT_IN_RANGE(1.0f, wl_pointer_turn(3.0f, 0.35f), 0.0001f); // clamped
    // halfway between the box and the edge: a quarter of the speed (squared)
    ASSERT_IN_RANGE(0.25f, wl_pointer_turn(0.675f, 0.35f), 0.0001f);
    // a symmetrical, rising ramp
    float last = 0.0f;
    for (int i = 0; i <= 20; i++) {
        float offset = (float)i / 20.0f;
        float t = wl_pointer_turn(offset, 0.35f);
        ASSERT(t >= last);
        ASSERT_IN_RANGE(t, -wl_pointer_turn(-offset, 0.35f), 0.0f);
        last = t;
    }
    ASSERT_IN_RANGE(0.0f, wl_pointer_turn(0.9f, 1.0f), 0.0f); // a box as large as the screen
    ASSERT_IN_RANGE(1.0f, wl_pointer_turn(1.0f, -1.0f), 0.0001f); // no box
    PASS();
}

SUITE(input_shape_suite) {
    RUN_TEST(stick_scales_over_the_travel_and_clamps);
    RUN_TEST(stick_pair_is_a_circle);
    RUN_TEST(triggers_run_from_first_to_full);
    RUN_TEST(axis16_reaches_the_extremes);
    RUN_TEST(pointer_is_the_ir_position_across_the_screen);
    RUN_TEST(pointer_loss_is_held_briefly_then_ends);
    RUN_TEST(pointer_never_tracks_before_it_is_seen_or_without_a_remote);
    RUN_TEST(pointer_turn_has_a_dead_zone_and_a_ramp);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(input_shape_suite);
    GREATEST_MAIN_END();
}
