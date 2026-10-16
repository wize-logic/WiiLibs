// SPDX-License-Identifier: MIT
#include <math.h>

#include "wl/platform/input_shape.h"

#define IR_HALF_W 320.0f
#define IR_HALF_H 240.0f

// The sticks are shaped in integers: the Wii reads the GameCube pads in the retrace interrupt, and
// libogc saves no floating-point registers for an interrupt, so a float there overwrote the ones
// the interrupted code was using (Halo's matrices on the Wii, sixty times a second).

// n / d rounded to the nearest, a half away from zero (lroundf's way); d > 0
static long round_div(long n, long d) {
    return n >= 0 ? (2 * n + d) / (2 * d) : -((-2 * n + d) / (2 * d));
}

int8_t wl_input_stick(int value, int full) {
    if (full <= 0) {
        return 0;
    }
    long scaled = round_div((long)value * 127, full);
    return (int8_t)(scaled < -127 ? -127 : scaled > 127 ? 127 : scaled);
}

// 127 * v / sqrt(squares) rounded as round_div does, with no square root: the largest q with
// (2q - 1)^2 * squares <= (254 * |v|)^2, that is q - 1/2 <= 127 |v| / sqrt(squares)
static int8_t on_circle(int v, int64_t squares) {
    int64_t a = v < 0 ? -(int64_t)v : v;
    int64_t limit = (254 * a) * (254 * a);
    int q = 0;
    for (int bit = 64; bit != 0; bit >>= 1) {
        int64_t odd = 2 * (int64_t)(q + bit) - 1;
        if (odd * odd * squares <= limit) {
            q += bit;
        }
    }
    q = q > 127 ? 127 : q;
    return (int8_t)(v < 0 ? -q : q);
}

void wl_input_stick_pair(int x, int y, int full, int8_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    if (full <= 0) {
        return;
    }
    int64_t squares = (int64_t)x * x + (int64_t)y * y;
    if (squares <= (int64_t)full * full) {
        out[0] = (int8_t)round_div((long)x * 127, full);
        out[1] = (int8_t)round_div((long)y * 127, full);
        return;
    }
    out[0] = on_circle(x, squares);
    out[1] = on_circle(y, squares);
}

uint8_t wl_input_trigger(unsigned value, unsigned first, unsigned full) {
    if (full <= first || value <= first) {
        return 0;
    }
    if (value >= full) {
        return 255;
    }
    return (uint8_t)(((value - first) * 255u + (full - first) / 2u) / (full - first));
}

int16_t wl_input_axis16(int8_t value) {
    long scaled = (long)value * 32767 / 127;
    return (int16_t)(scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled);
}

static float clampf(float v) {
    return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

bool wl_pointer_update_steady(WlPointer *p, const WlRawRemote *remote, uint64_t now_us, uint32_t hold_us,
                              float radius) {
    if (!remote->connected) {
        p->tracking = false;
        return false;
    }
    if (remote->ir_valid) {
        float x = clampf(((float)remote->ir_x - IR_HALF_W) / IR_HALF_W);
        float y = clampf(((float)remote->ir_y - IR_HALF_H) / IR_HALF_H);
        float dx = x - p->x;
        float dy = y - p->y;
        // (a reading at an edge is taken: the turn is fastest there, and a held position just inside
        // it would never reach it)
        bool at_edge = (fabsf(x) >= 1.0f && x != p->x) || (fabsf(y) >= 1.0f && y != p->y);
        if (!p->tracking || radius <= 0.0f || at_edge || dx * dx + dy * dy > radius * radius) {
            p->x = x;
            p->y = y;
        }
        p->seen_us = now_us;
        p->tracking = true;
    } else if (p->tracking && now_us - p->seen_us > hold_us) {
        p->tracking = false;
    }
    return p->tracking;
}

bool wl_pointer_update(WlPointer *p, const WlRawRemote *remote, uint64_t now_us, uint32_t hold_us) {
    return wl_pointer_update_steady(p, remote, now_us, hold_us, 0.0f);
}

float wl_pointer_turn(float offset, float dead_zone) {
    float magnitude = fabsf(offset);
    if (dead_zone < 0.0f) {
        dead_zone = 0.0f;
    }
    if (dead_zone >= 1.0f || magnitude <= dead_zone) {
        return 0.0f;
    }
    if (magnitude > 1.0f) {
        magnitude = 1.0f;
    }
    magnitude = (magnitude - dead_zone) / (1.0f - dead_zone);
    return (offset < 0.0f ? -1.0f : 1.0f) * magnitude * magnitude;
}
