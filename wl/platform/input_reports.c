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
    out->buttons = r->buttons;
    out->ir_valid = r->ir_count != 0;
    out->ir_x = out->ir_valid ? mean(r->ir_sum_x, r->ir_count) : 0;
    out->ir_y = out->ir_valid ? mean(r->ir_sum_y, r->ir_count) : 0;
    memcpy(out->accel, r->accel, sizeof out->accel);
    memcpy(out->accel_peak, r->peak, sizeof out->accel_peak);
    out->reports = (uint8_t)(r->count > 255u ? 255u : r->count);
}
