// SPDX-License-Identifier: MIT
// The IO thread: file reads, decompression and upload preparation run here at WL_THREAD_PRIO_IO (below main), so a slow SD read never stalls a
// frame. The main thread submits jobs through one SPSC ring and collects completions through
// another; the IO thread sleeps on a semaphore while idle. Platform-independent (built on
// platform.h threads); what a job means is up to the handler (fs.h installs one for file reads).
//
// Threading contract: wl_io_start / wl_io_submit / wl_io_poll / wl_io_stop are called from one
// thread (the main thread). The handler runs on the IO thread only; it must not touch GX or main-thread
// state, and may allocate only from the IO-safe heaps (wl_mem is locked, so any tag works).
#pragma once

#include "wl/platform/platform.h"
#include "wl/base/base.h"

#define WL_IO_QUEUE_DEPTH 64u        // outstanding jobs (submitted, not yet polled); power of two
#define WL_IO_STACK_SIZE (32u * 1024u) // from the WL_TAG_STACK budget

typedef struct {
    uint32_t id;   // assigned by wl_io_submit, echoed in the completion
    uint32_t op;   // opaque to the IO thread: the handler's operation code
    void *ptr;     // opaque arguments (buffer, path, destination)...
    void *ptr2;
    uint32_t size;
    uint32_t aux;
    int32_t result; // set by the handler (WL_OK or a WL_ERR_*; bytes read, etc.)
} WlIoJob;

// Runs on the IO thread for every job, in submission order; fills job->result (and any output
// fields) before returning.
typedef void (*WlIoHandler)(void *user, WlIoJob *job);

// Starts the thread (stack from WL_TAG_STACK, so wl_mem_init must have run). WL_ERR_STATE if
// running or the memory system is not ready, WL_ERR_NOMEM / WL_ERR_FAILED from the platform.
WlResult wl_io_start(WlIoHandler handler, void *user);
// Queues a job and assigns job->id (ids start at 1). WL_ERR_LIMIT when WL_IO_QUEUE_DEPTH jobs
// are already outstanding (poll first), WL_ERR_STATE when not running.
WlResult wl_io_submit(WlIoJob *job);
// Pops one finished job in completion order (= submission order); false when none is ready.
bool wl_io_poll(WlIoJob *out);
// Jobs submitted and not yet returned by wl_io_poll.
uint32_t wl_io_outstanding(void);
bool wl_io_running(void);
// Runs the queued jobs to completion, joins the thread and frees its stack. Completions that
// were not polled are discarded. No-op when not running.
void wl_io_stop(void);
