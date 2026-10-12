// SPDX-License-Identifier: MIT
// wl/platform/input_gesture.c: a Nunchuk's thrust found in synthetic accelerometer traces (units of
// 1/100 g, sampled every few milliseconds and handed over in frames of 33 ms as the backend does):
// a thrust, the shake of a walk, a turn of the wrist, a tilt held, a pull back, two thrusts close
// together and far apart, the clipping at 2 g, and the same thrust at other report rates.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/input_gesture.h"

typedef void (*Signal)(double t_ms, int16_t a[3]);

typedef struct {
    int fires;
    double first_ms;
    double last_ms;
} Result;

// small, repeatable noise of a resting hand's sensor: about +-3
static int16_t noise(uint32_t i, int axis) {
    uint32_t h = (i * 2654435761u) ^ ((uint32_t)axis * 40503u + 12345u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (int16_t)((int)(h % 7u) - 3);
}

// Runs the signal for `duration_ms`, a reading every `period_ms`, a frame every `frame_ms`.
static Result run_with(WlThrust *t, Signal signal, double duration_ms, double period_ms, double frame_ms) {
    Result r = {0, -1.0, -1.0};
    int16_t frame[64][3];
    unsigned n = 0;
    double next_frame = frame_ms;
    uint32_t index = 0;
    for (double ms = period_ms; ms <= duration_ms; ms += period_ms, index++) {
        int16_t a[3];
        signal(ms, a);
        for (int ax = 0; ax < 3; ax++) {
            a[ax] = (int16_t)(a[ax] + noise(index, ax));
        }
        if (n < 64) {
            memcpy(frame[n++], a, sizeof a);
        }
        if (ms + period_ms / 2.0 >= next_frame) {
            uint64_t now_us = 5000000u + (uint64_t)(ms * 1000.0);
            if (wl_thrust_feed(t, (const int16_t(*)[3])frame, n, n, now_us)) {
                r.fires++;
                if (r.first_ms < 0) {
                    r.first_ms = ms;
                }
                r.last_ms = ms;
            }
            n = 0;
            next_frame += frame_ms;
        }
    }
    return r;
}

static Result run(Signal signal, double duration_ms) {
    WlThrust t;
    wl_thrust_init(&t);
    return run_with(&t, signal, duration_ms, 10.0, 33.3);
}

// ---- the signals; the Nunchuk held level: x 0, forward (y) 0, z 1 g

// a push forward: at `at` ms the forward axis rises over `rise_ms` by `peak`, holds for 30 ms, then
// the hand stops and it falls to -`peak`/1.3 and back to rest
static double push(double t, double at, double rise_ms, double peak) {
    double u = t - at;
    if (u < 0) {
        return 0;
    }
    if (u < rise_ms) {
        return peak * u / rise_ms;
    }
    u -= rise_ms;
    if (u < 30) {
        return peak;
    }
    u -= 30;
    if (u < 70) {
        return peak - (peak + peak / 1.3) * u / 70.0;
    }
    u -= 70;
    if (u < 80) {
        return -peak / 1.3 * (1.0 - u / 80.0);
    }
    return 0;
}

static void thrust_at_500(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)push(t, 500, 60, 130);
    a[2] = 100;
}

static void thrust_at_500_from_a_tilt(double t, int16_t a[3]) {
    // held pointing up a little: forward axis at 0.5 g to begin with
    a[0] = 0;
    a[1] = (int16_t)(50 + push(t, 500, 60, 130));
    a[2] = 87;
}

static void clipped_thrust(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)(80 + push(t, 500, 60, 150));
    if (a[1] > 200) {
        a[1] = 200; // the accelerometer clips near 2 g
    }
    a[2] = 60;
}

static void gentler_thrust(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)push(t, 500, 70, 110); // ~1.1 g
    a[2] = 100;
}

static void weak_push(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)push(t, 500, 60, 60); // 0.6 g: a bump, not a thrust
    a[2] = 100;
}

static void slow_push(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)push(t, 500, 400, 130); // 1.3 g but over 0.4 s: a lean
    a[2] = 100;
}

static void pull_back(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)-push(t, 500, 60, 130);
    a[2] = 100;
}

static void two_thrusts_300_ms_apart(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)(push(t, 500, 60, 130) + push(t, 800, 60, 130));
    a[2] = 100;
}

static void two_thrusts_a_second_apart(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)(push(t, 500, 60, 130) + push(t, 1500, 60, 130));
    a[2] = 100;
}

static void walking(double t, int16_t a[3]) {
    double w = 2.0 * 3.14159265358979 * t / 1000.0;
    a[0] = (int16_t)(25.0 * sin(w));
    a[1] = (int16_t)(45.0 * sin(2.0 * w + 0.7)); // a stride a step, twice a second
    a[2] = (int16_t)(100.0 + 35.0 * sin(4.0 * w));
}

static void hurried_walk(double t, int16_t a[3]) {
    double w = 2.0 * 3.14159265358979 * 3.0 * t / 1000.0; // three strides a second, a swing of the arm
    a[0] = (int16_t)(40.0 * sin(w));
    a[1] = (int16_t)(60.0 * sin(2.0 * w));
    a[2] = (int16_t)(100.0 + 50.0 * sin(w));
}

// a turn of the wrist: the Nunchuk rolls through a quarter turn (gravity moves from z to x)
static void roll(double t, int16_t a[3]) {
    double u = t < 500 ? 0.0 : t > 700 ? 1.0 : (t - 500) / 200.0;
    double angle = u * 3.14159265358979 / 2.0;
    a[0] = (int16_t)(100.0 * sin(angle));
    a[1] = 0;
    a[2] = (int16_t)(100.0 * cos(angle));
}

// the Nunchuk pitched up, forward axis from level to straight up, quickly: gravity moves from z to the forward axis
static void quick_pitch(double t, int16_t a[3]) {
    double u = t < 500 ? 0.0 : t > 600 ? 1.0 : (t - 500) / 100.0;
    double angle = u * 3.14159265358979 / 2.0;
    a[0] = 0;
    a[1] = (int16_t)(100.0 * sin(angle));
    a[2] = (int16_t)(100.0 * cos(angle));
}

static void held_tilt(double t, int16_t a[3]) {
    (void)t;
    a[0] = 20;
    a[1] = 70;
    a[2] = 70;
}

// a lunge with a wind-up: the hand goes back before it goes forward, with no rest between
static void windup_without_rest(double t, int16_t a[3]) {
    a[0] = 0;
    a[1] = (int16_t)(push(t, 20, 50, 130) + push(t, 120, 50, 130));
    a[2] = 100;
}

static void thrust_with_sideways_sway(double t, int16_t a[3]) {
    a[0] = (int16_t)(push(t, 500, 60, 40)); // a little sideways with it
    a[1] = (int16_t)push(t, 500, 60, 130);
    a[2] = (int16_t)(100 - push(t, 500, 60, 30));
}

TEST a_thrust_from_rest_fires_once_while_it_rises(void) {
    Result r = run(thrust_at_500, 1500);
    ASSERT_EQ(1, r.fires);
    ASSERT(r.first_ms >= 500.0 + 30.0);        // not before it has risen by a gram
    ASSERT(r.first_ms <= 500.0 + 60.0 + 40.0); // and a frame or two of it
    PASS();
}

TEST a_thrust_from_a_tilted_hand_and_a_clipped_one_fire(void) {
    ASSERT_EQ(1, run(thrust_at_500_from_a_tilt, 1500).fires);
    ASSERT_EQ(1, run(clipped_thrust, 1500).fires);
    ASSERT_EQ(1, run(gentler_thrust, 1500).fires);
    ASSERT_EQ(1, run(thrust_with_sideways_sway, 1500).fires);
    PASS();
}

TEST the_same_thrust_fires_at_other_report_rates_and_frame_lengths(void) {
    const double periods[] = {5.0, 10.0, 20.0};
    const double frames[] = {16.7, 33.3, 66.7};
    for (int p = 0; p < 3; p++) {
        for (int f = 0; f < 3; f++) {
            WlThrust t;
            wl_thrust_init(&t);
            Result r = run_with(&t, thrust_at_500, 1500, periods[p], frames[f]);
            ASSERT_EQ(1, r.fires);
        }
    }
    PASS();
}

TEST a_small_bump_a_slow_lean_and_a_pull_back_do_not_fire(void) {
    ASSERT_EQ(0, run(weak_push, 1500).fires);
    ASSERT_EQ(0, run(slow_push, 1500).fires);
    ASSERT_EQ(0, run(pull_back, 1500).fires);
    PASS();
}

TEST a_walks_shake_never_fires(void) {
    ASSERT_EQ(0, run(walking, 30000).fires);
    ASSERT_EQ(0, run(hurried_walk, 30000).fires);
    PASS();
}

TEST a_turn_of_the_wrist_and_a_quick_tilt_do_not_fire(void) {
    ASSERT_EQ(0, run(roll, 1500).fires);
    ASSERT_EQ(0, run(quick_pitch, 1500).fires);
    PASS();
}

TEST a_tilt_held_does_not_fire_and_neither_does_stillness(void) {
    ASSERT_EQ(0, run(held_tilt, 20000).fires);
    PASS();
}

TEST a_thrust_with_no_rest_before_it_does_not_fire(void) {
    ASSERT_EQ(0, run(windup_without_rest, 1200).fires);
    PASS();
}

TEST one_thrust_is_one_fire_and_two_apart_are_two(void) {
    // the second push 300 ms after the first comes inside the hold-off, and before a rest
    ASSERT_EQ(1, run(two_thrusts_300_ms_apart, 2500).fires);
    Result r = run(two_thrusts_a_second_apart, 3000);
    ASSERT_EQ(2, r.fires);
    ASSERT(r.last_ms - r.first_ms > 900.0);
    PASS();
}

TEST a_thrust_just_after_the_nunchuk_appears_has_no_rest_to_stand_on(void) {
    WlThrust t;
    wl_thrust_init(&t);
    // the readings begin 40 ms before the push
    int16_t frame[8][3];
    unsigned n = 0;
    bool fired = false;
    uint64_t now = 9000000u;
    for (int i = 0; i < 60; i++) {
        double ms = i * 10.0;
        frame[n][0] = 0;
        frame[n][1] = (int16_t)push(ms, 40, 50, 140);
        frame[n][2] = 100;
        n++;
        if (n == 3) {
            now += 30000;
            fired = wl_thrust_feed(&t, (const int16_t(*)[3])frame, n, n, now) || fired;
            n = 0;
        }
    }
    ASSERT(!fired);
    // forgetting what it knew (the Nunchuk was unplugged) needs a rest again
    wl_thrust_reset(&t);
    ASSERT_EQ(0u, t.count);
    PASS();
}

TEST the_forward_axis_and_its_sign_are_the_callers_to_set(void) {
    WlThrust t;
    wl_thrust_init(&t);
    t.params.forward_sign = -1;
    // the same push, reading as a fall of the forward axis, is a thrust now; a rise is not
    Result r = run_with(&t, pull_back, 1500, 10.0, 33.3);
    ASSERT_EQ(1, r.fires);
    wl_thrust_init(&t);
    t.params.forward_sign = -1;
    ASSERT_EQ(0, run_with(&t, thrust_at_500, 1500, 10.0, 33.3).fires);
    PASS();
}

TEST a_frame_with_no_readings_and_a_busy_history_are_safe(void) {
    WlThrust t;
    wl_thrust_init(&t);
    ASSERT(!wl_thrust_feed(&t, NULL, 0, 0, 1000000u));
    // far more readings than the history holds
    int16_t big[WL_NUNCHUK_SAMPLES][3];
    memset(big, 0, sizeof big);
    for (int i = 0; i < 40; i++) {
        (void)wl_thrust_feed(&t, (const int16_t(*)[3])big, WL_NUNCHUK_SAMPLES, 200, 2000000u + (uint64_t)i * 33000u);
    }
    ASSERT(t.count <= WL_THRUST_HISTORY);
    PASS();
}

SUITE(input_gesture_suite) {
    RUN_TEST(a_thrust_from_rest_fires_once_while_it_rises);
    RUN_TEST(a_thrust_from_a_tilted_hand_and_a_clipped_one_fire);
    RUN_TEST(the_same_thrust_fires_at_other_report_rates_and_frame_lengths);
    RUN_TEST(a_small_bump_a_slow_lean_and_a_pull_back_do_not_fire);
    RUN_TEST(a_walks_shake_never_fires);
    RUN_TEST(a_turn_of_the_wrist_and_a_quick_tilt_do_not_fire);
    RUN_TEST(a_tilt_held_does_not_fire_and_neither_does_stillness);
    RUN_TEST(a_thrust_with_no_rest_before_it_does_not_fire);
    RUN_TEST(one_thrust_is_one_fire_and_two_apart_are_two);
    RUN_TEST(a_thrust_just_after_the_nunchuk_appears_has_no_rest_to_stand_on);
    RUN_TEST(the_forward_axis_and_its_sign_are_the_callers_to_set);
    RUN_TEST(a_frame_with_no_readings_and_a_busy_history_are_safe);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(input_gesture_suite);
    GREATEST_MAIN_END();
}
