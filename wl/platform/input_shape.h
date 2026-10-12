// SPDX-License-Identifier: MIT
// Shaping of the raw input words (platform.h) into the ranges an application wants. It holds no
// libogc call and no byte-order assumption, so every backend uses it and host tests and the
// big-endian build run it. Mapping to actions stays with the layer above.
#pragma once

#include "wl/platform/platform.h"

// ---- Full-scale readings of the devices ------------------------------------------------------
// GameCube pad. libogc's PAD_Clamp (the SDK's PADClamp) takes a stick as full at 72 (main) and
// 59 (C) after removing a dead zone of 15 from each axis, and a trigger as 0 at 30 and full at
// 180. A raw reading is therefore full at max + 15 (stick), and the trigger's range starts at 30.
// The dead zone is left out here: an application has its own.
#define WL_PAD_MAIN_STICK_FULL 87
#define WL_PAD_C_STICK_FULL 74
#define WL_PAD_TRIGGER_FIRST 30
#define WL_PAD_TRIGGER_FULL 180
// The Classic Controller's analogue triggers are 5 bits (wiiuse's ls_raw / rs_raw).
#define WL_CLASSIC_TRIGGER_FULL 31

// A stick reading `value` of `full` (full deflection) as -127..127, clamped.
int8_t wl_input_stick(int value, int full);
// A stick's two axes the same way, limited as a circle: a diagonal beyond full keeps its
// direction and comes out at full, not at a corner (the pads' gates are octagons).
void wl_input_stick_pair(int x, int y, int full, int8_t out[2]);
// A trigger reading as 0..255: 0 up to `first`, 255 from `full`.
uint8_t wl_input_trigger(unsigned value, unsigned first, unsigned full);
// A -127..127 axis as the 16-bit signed axis of most gamepad APIs (-127 and 127 are the extremes).
int16_t wl_input_axis16(int8_t value);

// ---- The Wii Remote's pointer ----------------------------------------------------------------
// The pointer's position across the screen, -1..1 each way (x right, y down) from the IR
// position (platform.h: fb space, 640 by 480). The sensor bar is lost now and then, at the
// screen's edge and when the Remote is turned: a loss shorter than `hold_us` keeps the last
// position, a longer one ends the tracking.
typedef struct {
    bool tracking;
    float x, y;
    uint64_t seen_us; // when the position was last read
} WlPointer;

// One poll's reading. Returns whether the pointer is tracked (p->x, p->y valid). A Remote that is
// not connected resets p.
bool wl_pointer_update(WlPointer *p, const WlRawRemote *remote, uint64_t now_us, uint32_t hold_us);

// The same with a play radius, which takes a hand's tremor out of the pointer: while the reading is
// within `radius` (0..1 of the half screen, as the pointer's own units; 0 for none) of the held
// position p->x, p->y, the pointer holds still; a reading beyond it, or one at the screen's edge, moves it
// to the reading at once, so a deliberate move has no lag. A pointer found again after a loss
// starts at its reading.
bool wl_pointer_update_steady(WlPointer *p, const WlRawRemote *remote, uint64_t now_us, uint32_t hold_us,
                              float radius);

// A turn rate from the pointer's offset on one axis: 0..1 and the sign of `offset`. Nothing
// within `dead_zone` (0..1 of the half screen); beyond it a ramp that starts gently (squared) and
// reaches 1 at the screen's edge, so that a small step out of the box turns slowly.
float wl_pointer_turn(float offset, float dead_zone);
