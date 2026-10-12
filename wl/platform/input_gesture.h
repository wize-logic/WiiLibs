// SPDX-License-Identifier: MIT
// A forward thrust of a Wii Nunchuk, found in the readings of its accelerometer. The Nunchuk's
// accelerometer clips near 2 g and a hand at rest or walking already reads up to 1 g of gravity
// and a few tenths more, so the thrust is told by a change, not by a size: a rise of the forward
// axis of about a gram within a tenth of a second, from a short rest, with the sideways axes
// quiet. A walk's shake (no rest, no such rise), a turn of the wrist (the sideways axes move with
// it), a tilt that is held (no change) and the Nunchuk's own noise do not fire it. One thrust fires
// once: for a while after it fires nothing does. Plain C on plain numbers (no libogc, no byte
// order), driven by the readings of a frame (WlRawRemote.nunchuk_accel), so that host tests and the
// big-endian build run it.
#pragma once

#include "wl/platform/platform.h"

// The units are WlRawRemote.accel's (1 g ~ 100). Defaults from wl_thrust_init.
typedef struct {
    uint32_t rise;       // the forward axis must rise by this much within rise_us (100: 1 g)
    uint32_t rise_us;    // ... in this time (130 ms)
    uint32_t rest_us;    // preceded by this long a rest (150 ms) ...
    uint32_t rest_range; // ... in which no axis moves by more than this (35: a third of a g)
    uint32_t side_range; // and the other two axes move by no more than this during the rise (60)
    uint32_t holdoff_us; // a thrust fires once: nothing fires for this long after it (600 ms)
    int forward_axis;    // which axis is forward: 0, 1 or 2 (1: the Nunchuk's long axis)
    int forward_sign;    // 1 when a push forward reads as a rise of that axis, -1 when as a fall
} WlThrustParams;

#define WL_THRUST_HISTORY 96u

typedef struct {
    WlThrustParams params;
    uint64_t at_us[WL_THRUST_HISTORY]; // the readings kept, in a ring, oldest first from `first`
    int16_t value[WL_THRUST_HISTORY][3];
    uint32_t first, count;
    uint64_t last_feed_us;
    uint64_t fired_us;
    bool fired;
} WlThrust;

// Sets the defaults and forgets everything.
void wl_thrust_init(WlThrust *t);
// Forgets the readings and the last thrust (the Nunchuk was unplugged, a long gap): the next thrust
// needs a rest again before it.
void wl_thrust_reset(WlThrust *t);
// Takes a frame's readings, the last of them read just before `now_us` (the clock of the caller's
// own, in microseconds). `n` are given and `reports` is how many the frame had in all (WlRawRemote's
// nunchuk_samples and nunchuk_reports); their times are spread evenly over the time since the last
// feed. Returns true when a thrust is found in them.
bool wl_thrust_feed(WlThrust *t, const int16_t (*samples)[3], unsigned n, unsigned reports, uint64_t now_us);
