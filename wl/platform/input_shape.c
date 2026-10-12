// SPDX-License-Identifier: MIT
#include <math.h>

#include "wl/platform/input_shape.h"

#define IR_HALF_W 320.0f
#define IR_HALF_H 240.0f

int8_t wl_input_stick(int value, int full) {
    if (full <= 0) {
        return 0;
    }
    long scaled = lroundf((float)value * 127.0f / (float)full);
    return (int8_t)(scaled < -127 ? -127 : scaled > 127 ? 127 : scaled);
}

void wl_input_stick_pair(int x, int y, int full, int8_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    if (full <= 0) {
        return;
    }
    float fx = (float)x / (float)full;
    float fy = (float)y / (float)full;
    float length = sqrtf(fx * fx + fy * fy);
    if (length > 1.0f) {
        fx /= length;
        fy /= length;
    }
    out[0] = (int8_t)lroundf(fx * 127.0f);
    out[1] = (int8_t)lroundf(fy * 127.0f);
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
