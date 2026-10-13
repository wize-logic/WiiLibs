// SPDX-License-Identifier: MIT
// The Xbox gamepad state, its reports and the Wii's controllers mapped onto it (pad.h). The
// mappings and the rumble's duty cycle come from OpenCE-Wii's port/wii/src/xinput_wii_map.c (CC0).
#include <string.h>

#include "wl/platform/input_shape.h"
#include "wl/xboxrt/pad.h"

bool wl_xbox_pad_equal(const WlXboxPad *a, const WlXboxPad *b) {
    if (a->buttons != b->buttons) {
        return false;
    }
    for (unsigned i = 0; i < WL_XBOX_PAD_ANALOG_COUNT; i++) {
        if (a->analog[i] != b->analog[i]) {
            return false;
        }
    }
    for (unsigned i = 0; i < 4u; i++) {
        if (a->thumb[i] != b->thumb[i]) {
            return false;
        }
    }
    return true;
}

void wl_xbox_pad_merge(WlXboxPad *base, const WlXboxPad *over) {
    base->buttons |= over->buttons;
    for (unsigned i = 0; i < WL_XBOX_PAD_ANALOG_COUNT; i++) {
        if (over->analog[i] > base->analog[i]) {
            base->analog[i] = over->analog[i];
        }
    }
    for (unsigned i = 0; i < 4u; i++) {
        if (over->thumb[i] != 0) {
            base->thumb[i] = over->thumb[i];
        }
    }
}

uint32_t wl_xbox_pad_report(const WlXboxPad *pad, uint8_t out[WL_XBOX_PAD_REPORT_BYTES]) {
    out[0] = 0;
    out[1] = (uint8_t)WL_XBOX_PAD_REPORT_BYTES;
    out[2] = (uint8_t)(pad->buttons & 0xFFu);
    out[3] = 0;
    for (unsigned i = 0; i < WL_XBOX_PAD_ANALOG_COUNT; i++) {
        out[4 + i] = pad->analog[i];
    }
    for (unsigned i = 0; i < 4u; i++) {
        uint16_t v = (uint16_t)pad->thumb[i];
        out[12 + 2 * i] = (uint8_t)(v & 0xFFu);
        out[13 + 2 * i] = (uint8_t)(v >> 8);
    }
    return WL_XBOX_PAD_REPORT_BYTES;
}

bool wl_xbox_pad_parse(const uint8_t *report, uint32_t bytes, WlXboxPad *out) {
    if (bytes < WL_XBOX_PAD_REPORT_BYTES || report[1] != WL_XBOX_PAD_REPORT_BYTES) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->buttons = report[2];
    for (unsigned i = 0; i < WL_XBOX_PAD_ANALOG_COUNT; i++) {
        out->analog[i] = report[4 + i];
    }
    for (unsigned i = 0; i < 4u; i++) {
        out->thumb[i] = (int16_t)(uint16_t)(report[12 + 2 * i] | (report[13 + 2 * i] << 8));
    }
    return true;
}

bool wl_xbox_rumble_parse(const uint8_t *report, uint32_t bytes, uint16_t *left, uint16_t *right) {
    if (bytes < WL_XBOX_PAD_RUMBLE_BYTES || report[1] != WL_XBOX_PAD_RUMBLE_BYTES) {
        return false;
    }
    *left = (uint16_t)(report[2] | (report[3] << 8));
    *right = (uint16_t)(report[4] | (report[5] << 8));
    return true;
}

// ---- The Wii's controllers -------------------------------------------------------------------

static void analog(WlXboxPad *pad, bool held, unsigned index) {
    if (held) {
        pad->analog[index] = 0xFF;
    }
}

static void digital(WlXboxPad *pad, bool held, uint16_t flag) {
    if (held) {
        pad->buttons |= flag;
    }
}

// a trigger: its analogue reading, the whole travel when its click says so (the Classic Pro has
// only the click)
static void trigger(WlXboxPad *pad, bool held, unsigned index, uint8_t reading) {
    pad->analog[index] = held ? 0xFF : reading;
}

static void sticks(WlXboxPad *pad, const int8_t *left, const int8_t *right) {
    if (left != NULL) {
        pad->thumb[WL_XBOX_PAD_LX] = wl_input_axis16(left[0]);
        pad->thumb[WL_XBOX_PAD_LY] = wl_input_axis16(left[1]);
    }
    if (right != NULL) {
        pad->thumb[WL_XBOX_PAD_RX] = wl_input_axis16(right[0]);
        pad->thumb[WL_XBOX_PAD_RY] = wl_input_axis16(right[1]);
    }
}

void wl_xbox_pad_from_gamecube(const WlRawPad *gc, bool menu, WlXboxPad *pad) {
    uint16_t held = gc->buttons;
    memset(pad, 0, sizeof *pad);
    analog(pad, held & WL_PAD_A, WL_XBOX_PAD_A);
    analog(pad, held & WL_PAD_B, WL_XBOX_PAD_B);
    analog(pad, held & WL_PAD_X, WL_XBOX_PAD_X);
    analog(pad, held & WL_PAD_Y, WL_XBOX_PAD_Y);
    analog(pad, held & WL_PAD_Z, WL_XBOX_PAD_BLACK);
    trigger(pad, held & WL_PAD_L, WL_XBOX_PAD_LT, gc->trigger[0]);
    trigger(pad, held & WL_PAD_R, WL_XBOX_PAD_RT, gc->trigger[1]);
    digital(pad, held & WL_PAD_UP, WL_XBOX_PAD_UP);
    digital(pad, held & WL_PAD_DOWN, WL_XBOX_PAD_DOWN);
    digital(pad, held & WL_PAD_LEFT, WL_XBOX_PAD_LEFT);
    digital(pad, held & WL_PAD_RIGHT, WL_XBOX_PAD_RIGHT);
    if (!menu) {
        digital(pad, held & WL_PAD_UP, WL_XBOX_PAD_RTHUMB);
        digital(pad, held & WL_PAD_DOWN, WL_XBOX_PAD_LTHUMB);
        digital(pad, held & WL_PAD_LEFT, WL_XBOX_PAD_BACK);
        analog(pad, held & WL_PAD_RIGHT, WL_XBOX_PAD_WHITE);
    }
    digital(pad, held & WL_PAD_START, WL_XBOX_PAD_START);
    sticks(pad, gc->stick[0], gc->stick[1]);
}

void wl_xbox_pad_from_classic(const WlRawRemote *remote, bool menu, WlXboxPad *pad) {
    uint32_t held = remote->buttons;
    memset(pad, 0, sizeof *pad);
    analog(pad, held & WL_CC_B, WL_XBOX_PAD_A);
    analog(pad, held & WL_CC_A, WL_XBOX_PAD_B);
    analog(pad, held & WL_CC_Y, WL_XBOX_PAD_X);
    analog(pad, held & WL_CC_X, WL_XBOX_PAD_Y);
    analog(pad, held & WL_CC_ZL, WL_XBOX_PAD_BLACK);
    analog(pad, held & WL_CC_ZR, WL_XBOX_PAD_WHITE);
    trigger(pad, held & WL_CC_L, WL_XBOX_PAD_LT, remote->trigger[0]);
    trigger(pad, held & WL_CC_R, WL_XBOX_PAD_RT, remote->trigger[1]);
    digital(pad, held & WL_CC_UP, WL_XBOX_PAD_UP);
    digital(pad, held & WL_CC_DOWN, WL_XBOX_PAD_DOWN);
    digital(pad, held & WL_CC_LEFT, WL_XBOX_PAD_LEFT);
    digital(pad, held & WL_CC_RIGHT, WL_XBOX_PAD_RIGHT);
    if (!menu) {
        digital(pad, held & WL_CC_UP, WL_XBOX_PAD_RTHUMB);
        digital(pad, held & WL_CC_DOWN, WL_XBOX_PAD_LTHUMB);
    }
    digital(pad, held & WL_CC_PLUS, WL_XBOX_PAD_START);
    digital(pad, held & WL_CC_MINUS, WL_XBOX_PAD_BACK);
    sticks(pad, remote->stick[0], remote->stick[1]);
}

static int16_t pointer_axis(float offset, float dead_zone) {
    float turn = wl_pointer_turn(offset, dead_zone);
    return (int16_t)(turn * 32767.0f);
}

void wl_xbox_pad_from_remote(const WlRawRemote *remote, const WlPointer *pointer, float dead_zone, bool menu,
                             WlXboxPad *pad) {
    uint32_t held = remote->buttons;
    memset(pad, 0, sizeof *pad);
    analog(pad, held & WL_WM_A, WL_XBOX_PAD_A);
    analog(pad, held & WL_WM_1, WL_XBOX_PAD_B);
    analog(pad, held & WL_NC_Z, WL_XBOX_PAD_LT);
    analog(pad, held & WL_WM_B, WL_XBOX_PAD_RT);
    digital(pad, held & WL_NC_C, WL_XBOX_PAD_LTHUMB);
    digital(pad, held & WL_WM_2, WL_XBOX_PAD_RTHUMB);
    digital(pad, held & WL_WM_PLUS, WL_XBOX_PAD_START);
    digital(pad, held & WL_WM_MINUS, WL_XBOX_PAD_BACK);
    digital(pad, held & WL_WM_UP, WL_XBOX_PAD_UP);
    digital(pad, held & WL_WM_DOWN, WL_XBOX_PAD_DOWN);
    digital(pad, held & WL_WM_LEFT, WL_XBOX_PAD_LEFT);
    digital(pad, held & WL_WM_RIGHT, WL_XBOX_PAD_RIGHT);
    if (menu) {
        analog(pad, held & WL_WM_B, WL_XBOX_PAD_B);
    } else {
        analog(pad, held & WL_WM_UP, WL_XBOX_PAD_Y);
        analog(pad, held & WL_WM_DOWN, WL_XBOX_PAD_X);
        analog(pad, held & WL_WM_LEFT, WL_XBOX_PAD_BLACK);
        analog(pad, held & WL_WM_RIGHT, WL_XBOX_PAD_WHITE);
    }
    if (remote->extension == WL_EXT_NUNCHUK) {
        sticks(pad, remote->stick[0], NULL);
    }
    if (pointer != NULL && pointer->tracking && !menu) {
        pad->thumb[WL_XBOX_PAD_RX] = pointer_axis(pointer->x, dead_zone);
        pad->thumb[WL_XBOX_PAD_RY] = pointer_axis(-pointer->y, dead_zone); // pointer up: look up
    }
}

// ---- Rumble -----------------------------------------------------------------------------------

bool wl_xbox_rumble_duty(WlXboxRumble *r, uint32_t strength, uint64_t now_us) {
    if (strength == 0) {
        r->carry = 0;
        if (r->on) {
            r->on = false;
            r->switched_us = now_us;
        }
        return false;
    }
    if (strength > WL_XBOX_RUMBLE_FULL) {
        strength = WL_XBOX_RUMBLE_FULL;
    }
    r->carry += (int32_t)strength;
    bool wanted = r->carry >= (int32_t)(WL_XBOX_RUMBLE_FULL / 2u + 1u);
    if (wanted != r->on && (r->switched_us == 0 || now_us - r->switched_us >= WL_XBOX_RUMBLE_SWITCH_US)) {
        r->on = wanted;
        r->switched_us = now_us;
    }
    // what the motor really did this tick comes off the carry: a switch that had to wait is made
    // up for by the ticks after it
    if (r->on) {
        r->carry -= (int32_t)WL_XBOX_RUMBLE_FULL;
    }
    return r->on;
}
