// SPDX-License-Identifier: MIT
#include <string.h>

#include "wl/platform/input_reports.h"

void wl_remote_reports_begin(WlRemoteReports *r) {
    memset(r, 0, sizeof *r);
}

void wl_remote_reports_add(WlRemoteReports *r, const WlRemoteReport *report) {
    // (element by element: this runs for every report, and a call to memcpy for six bytes costs more)
    int32_t x = report->accel[0];
    int32_t y = report->accel[1];
    int32_t z = report->accel[2];
    uint32_t magnitude = (uint32_t)(x * x) + (uint32_t)(y * y) + (uint32_t)(z * z);
    if (r->count == 0 || magnitude > r->peak_magnitude_squared) {
        r->peak_magnitude_squared = magnitude;
        r->peak[0] = report->accel[0];
        r->peak[1] = report->accel[1];
        r->peak[2] = report->accel[2];
    }
    if (report->nunchuk_valid) {
        int16_t *slot = r->nunchuk[r->nunchuk_count % WL_NUNCHUK_SAMPLES];
        slot[0] = report->nunchuk[0];
        slot[1] = report->nunchuk[1];
        slot[2] = report->nunchuk[2];
        r->nunchuk_count++;
    }
    r->accel[0] = report->accel[0];
    r->accel[1] = report->accel[1];
    r->accel[2] = report->accel[2];
    r->buttons |= report->buttons;
    if (report->ir_valid) {
        r->ir_sum_x += report->ir_x;
        r->ir_sum_y += report->ir_y;
        r->ir_count++;
    }
    r->count++;
}

// sum / n rounded to the nearest, halves away from zero
static int16_t mean(int32_t sum, uint32_t n) {
    int32_t half = (int32_t)(n / 2u);
    return (int16_t)((sum + (sum < 0 ? -half : half)) / (int32_t)n);
}

void wl_remote_reports_finish(const WlRemoteReports *r, WlRawRemote *out) {
    if (r->count == 0) {
        return;
    }
    uint32_t kept = r->nunchuk_count < WL_NUNCHUK_SAMPLES ? r->nunchuk_count : WL_NUNCHUK_SAMPLES;
    // (the ring from its oldest entry: the one the next would replace, or the first)
    uint32_t first = r->nunchuk_count - kept;
    for (uint32_t i = 0; i < kept; i++) {
        memcpy(out->nunchuk_accel[i], r->nunchuk[(first + i) % WL_NUNCHUK_SAMPLES], sizeof out->nunchuk_accel[i]);
    }
    out->nunchuk_samples = (uint8_t)kept;
    out->nunchuk_reports = (uint8_t)(r->nunchuk_count > 255u ? 255u : r->nunchuk_count);
    out->buttons = r->buttons;
    out->ir_valid = r->ir_count != 0;
    out->ir_x = out->ir_valid ? mean(r->ir_sum_x, r->ir_count) : 0;
    out->ir_y = out->ir_valid ? mean(r->ir_sum_y, r->ir_count) : 0;
    memcpy(out->accel, r->accel, sizeof out->accel);
    memcpy(out->accel_peak, r->peak, sizeof out->accel_peak);
    out->reports = (uint8_t)(r->count > 255u ? 255u : r->count);
}

void wl_pad_tracker_init(WlPadTracker *t) {
    memset(t, 0, sizeof *t);
}

void wl_pad_tracker_sample(WlPadTracker *t, WlPadSampleKind kind, const WlRawPad *pad) {
    t->samples++;
    if (kind == WL_PAD_SAMPLE_OK) {
        t->was = true;
        t->errors = 0;
        t->last = *pad;
        t->last.connected = true;
        t->latched |= pad->buttons;
    } else if (kind == WL_PAD_SAMPLE_ABSENT) {
        t->was = false;
        t->errors = 0;
        t->latched = 0;
    } else if (t->was) {
        // (a pad that never answered, or was found absent, stays absent through errors)
        if (t->errors < WL_PAD_ERROR_GRACE_SAMPLES) {
            t->errors++;
        }
        if (t->errors >= WL_PAD_ERROR_GRACE_SAMPLES) {
            t->was = false;
            t->latched = 0;
        }
    }
}

void wl_pad_tracker_take(WlPadTracker *t, WlRawPad *out, uint32_t *samples) {
    if (t->was) {
        *out = t->last;
        out->buttons |= t->latched;
    } else {
        memset(out, 0, sizeof *out);
    }
    if (samples != NULL) {
        *samples = t->samples;
    }
    t->latched = 0;
    t->samples = 0;
}
