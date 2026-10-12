// SPDX-License-Identifier: MIT
// A Wii Remote reports about every 5 ms, six or seven times in a 30 Hz frame. Reading one snapshot
// a frame can miss a press that came and went inside it, and carries the pointer's jitter of that
// one report. This folds all of a frame's reports into the one WlRawRemote the frame reads: a
// button seen pressed in any report is pressed for the frame, the pointer is the average of the
// reports that had one, the acceleration keeps its peak. It holds no libogc call and no byte-order
// assumption, so host tests and the big-endian build run it; wii/input_wii.c feeds it.
#pragma once

#include "wl/platform/platform.h"

// What one report carries that is folded (input_wii.c fills it from libogc's data for the report).
typedef struct {
    uint32_t buttons; // as WlRawRemote.buttons
    bool ir_valid;
    int16_t ir_x, ir_y; // as WlRawRemote.ir_x, ir_y
    int16_t accel[3];   // as WlRawRemote.accel
} WlRemoteReport;

typedef struct {
    uint32_t count;   // reports added
    uint32_t buttons; // every button of any report
    int32_t ir_sum_x, ir_sum_y;
    uint32_t ir_count; // reports with a pointer
    int16_t accel[3];  // the last report's
    int16_t peak[3];   // the report of greatest magnitude
    uint32_t peak_magnitude_squared;
} WlRemoteReports;

// Starts a frame's reports.
void wl_remote_reports_begin(WlRemoteReports *r);
// Folds one report (the oldest first).
void wl_remote_reports_add(WlRemoteReports *r, const WlRemoteReport *report);
// Writes what the reports add up to into `out`: buttons, ir_valid / ir_x / ir_y (the mean of the
// reports that had a pointer, none if none had one), accel (the last report's), accel_peak and
// reports (saturating at 255). With no report added it writes nothing: `out` keeps the caller's
// snapshot.
void wl_remote_reports_finish(const WlRemoteReports *r, WlRawRemote *out);
