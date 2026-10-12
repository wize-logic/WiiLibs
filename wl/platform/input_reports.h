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
    bool nunchuk_valid; // the report carries a Nunchuk's acceleration ...
    int16_t nunchuk[3]; // ... in the same units
} WlRemoteReport;

typedef struct {
    uint32_t count;   // reports added
    uint32_t buttons; // every button of any report
    int32_t ir_sum_x, ir_sum_y;
    uint32_t ir_count; // reports with a pointer
    int16_t accel[3];  // the last report's
    int16_t peak[3];   // the report of greatest magnitude
    uint32_t peak_magnitude_squared;
    uint32_t nunchuk_count;                       // reports that carried the Nunchuk's acceleration
    int16_t nunchuk[WL_NUNCHUK_SAMPLES][3];       // the last WL_NUNCHUK_SAMPLES of them, in a ring
} WlRemoteReports;

// Starts a frame's reports.
void wl_remote_reports_begin(WlRemoteReports *r);
// Folds one report (the oldest first).
void wl_remote_reports_add(WlRemoteReports *r, const WlRemoteReport *report);
// Writes what the reports add up to into `out`: buttons, ir_valid / ir_x / ir_y (the mean of the
// reports that had a pointer, none if none had one), accel (the last report's), accel_peak and
// reports (saturating at 255), and the Nunchuk's readings (nunchuk_accel, oldest first, the last
// WL_NUNCHUK_SAMPLES; nunchuk_reports, all there were, saturating at 255). With no report added it writes nothing: `out` keeps the caller's
// snapshot.
void wl_remote_reports_finish(const WlRemoteReports *r, WlRawRemote *out);

// ---------------------------------------------------------------------------------------------
// GameCube controllers, read at every video retrace
// ---------------------------------------------------------------------------------------------
// A pad is read 50 or 60 times a second, from the retrace, and the program takes what has come in
// since its last look: a button pressed in any sample is pressed for that look, so a tap shorter
// than a frame is not lost; everything else is the latest sample's. A sample that failed for a
// moment (a transfer error, a read that came too soon) keeps the last good state, and a pad that
// fails for WL_PAD_ERROR_GRACE_SAMPLES in a row counts as gone, as it does at once when the console
// says there is no controller. The tracker is plain data: the caller takes the interrupts off around
// it when a retrace handler feeds it (input_wii.c). No libogc call, no byte-order assumption.

#define WL_PAD_ERROR_GRACE_SAMPLES 20u // a third of a second at 60 Hz

typedef enum {
    WL_PAD_SAMPLE_OK,     // fresh data: `pad` holds the reading
    WL_PAD_SAMPLE_ABSENT, // no controller on the port
    WL_PAD_SAMPLE_ERROR   // a momentary failure: no reading this time
} WlPadSampleKind;

typedef struct {
    bool was;       // the pad has answered and not been found absent since
    uint8_t errors; // failed samples in a row, up to the grace
    uint16_t latched;  // buttons of every good sample since the last take
    uint32_t samples;  // samples since the last take (any kind)
    WlRawPad last;  // the latest good reading (connected set)
} WlPadTracker;

void wl_pad_tracker_init(WlPadTracker *t);
// One sample of the pad. `pad` is read for WL_PAD_SAMPLE_OK only.
void wl_pad_tracker_sample(WlPadTracker *t, WlPadSampleKind kind, const WlRawPad *pad);
// Writes what the program sees into `out` (zeroed, disconnected, for a pad that is gone) and starts
// the next look's latch; `samples` is how many samples the look covers.
void wl_pad_tracker_take(WlPadTracker *t, WlRawPad *out, uint32_t *samples);
