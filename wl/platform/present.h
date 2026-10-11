// SPDX-License-Identifier: MIT
// The display queue: which XFB is being drawn, which wait for the GPU to finish writing them,
// which was handed to the video interface, which is on the screen. A portable state machine:
// wii/video_wii.c drives it from the thread (choosing the back buffer, presenting) and from the
// retrace interrupt (committing a frame, confirming it reached the screen); the host tests drive
// it with simulated retraces.
//
// The rules it keeps:
//  - the back buffer is never one the video interface scans or is about to scan (no tearing);
//  - presented frames reach the screen in order, none skipped (a FIFO, not a mailbox);
//  - a frame goes to the video interface only once the GPU has finished writing it (its `done`
//    test, polled at the retrace: a flip can only happen at a retrace anyway);
//  - each frame stays on the screen at least `interval` retraces (1 = every retrace, 2 = 30 Hz on
//    a 60 Hz display), counted from the retrace at which the previous one was latched;
//  - nothing here waits: the thread waits (for a retrace) only when acquire finds no free buffer.
//
// Concurrency: the retrace functions run in the interrupt; the thread calls the others with
// interrupts disabled (video_wii.c), so each call sees a consistent queue. A `done` test runs in
// the interrupt and must be cheap and safe there (a GPU register read).
#pragma once

#include "wl/base/base.h"

#define WL_PRESENT_MAX_BUFFERS 3u

// Has the GPU finished writing the frame? (`arg` is what the presenter passed.)
typedef bool (*WlPresentDone)(uint32_t arg);

typedef enum {
    WL_XFB_FREE = 0,  // may be drawn
    WL_XFB_BACK,      // being drawn (acquired, not yet presented)
    WL_XFB_PENDING,   // presented, waiting for the GPU or for its retrace
    WL_XFB_COMMITTED, // handed to the video interface at a retrace, not yet confirmed scanned
    WL_XFB_SHOWN      // on the screen
} WlXfbState;

typedef struct {
    uint8_t count;    // buffers, 2 or 3
    uint8_t interval; // minimum retraces a frame stays on the screen (>= 1)
    int8_t back;      // the buffer being drawn, -1 when none is acquired
    int8_t shown;     // the buffer on the screen, -1 when none of ours
    int8_t committed; // the buffer handed to the video interface, -1 when none
    int8_t scanned;   // the buffer the video interface reported at the last retrace (-1: not ours)
    uint8_t state[WL_PRESENT_MAX_BUFFERS];
    uint8_t queue[WL_PRESENT_MAX_BUFFERS]; // presented buffers, oldest first
    uint8_t queued;
    WlPresentDone done[WL_PRESENT_MAX_BUFFERS];
    uint32_t done_arg[WL_PRESENT_MAX_BUFFERS];
    bool latched_any;
    uint32_t retraces;   // retraces seen (wl_present_retrace_begin calls)
    uint32_t last_latch; // the retrace at which the frame on the screen was latched
    uint32_t presented;  // frames presented
    uint32_t latched;    // frames that reached the screen
    uint32_t late;       // frames latched more than `interval` retraces after the one before
    uint32_t reclaimed;  // pending frames dropped by wl_present_reclaim (a GPU that never finished)
    uint32_t last_gap;   // retraces between the last two frames to reach the screen
} WlPresentQueue;

// `count` buffers (2 or 3), with buffer `shown` (or -1) on the screen. WL_ERR_ARG otherwise.
WlResult wl_present_init(WlPresentQueue *q, uint32_t count, uint32_t interval, int shown);
// Changes the minimum retraces per frame (clamped to 1..15); takes effect at the next commit.
void wl_present_set_interval(WlPresentQueue *q, uint32_t interval);

// Thread: the back buffer (the one already acquired, else a free one, which becomes it). -1 when
// every buffer is on the screen or waiting to be: the caller waits for a retrace and asks again.
int wl_present_acquire(WlPresentQueue *q);
// Thread: the back buffer is presented; it reaches the screen once `done(done_arg)` is true (NULL:
// at once) and the interval allows. false (and nothing presented) when no buffer is acquired.
bool wl_present_submit(WlPresentQueue *q, WlPresentDone done, uint32_t done_arg);
// Thread, when waiting for a buffer has gone on too long (a GPU that stopped): the oldest pending
// frame is dropped (its buffer freed); with none pending, a free buffer the video interface scans
// (given to it by someone else) stops being avoided. The buffer that became usable, or -1.
int wl_present_reclaim(WlPresentQueue *q);
// Every presented frame is on the screen (none pending or committed).
bool wl_present_idle(const WlPresentQueue *q);

// Interrupt, before the video interface's registers are written at a retrace: the buffer to hand
// it now (the caller sets it as the next framebuffer and flushes), or -1 for none.
int wl_present_retrace_begin(WlPresentQueue *q);
// Interrupt, after the registers are written: `current` is the buffer the video interface now
// scans (-1 when it is none of ours). A committed buffer it scans has reached the screen.
void wl_present_retrace_end(WlPresentQueue *q, int current);
