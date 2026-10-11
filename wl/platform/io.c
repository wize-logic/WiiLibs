// SPDX-License-Identifier: MIT
// The IO thread: request ring -> handler -> completion ring. See io.h.
#include "wl/platform/io.h"

#include <stdatomic.h>

#include "wl/platform/mem.h"
#include "wl/platform/ring.h"

static struct {
    bool running;
    WlIoHandler handler;
    void *user;
    WlPlatformThread thread;
    WlPlatformSemaphore wake; // posted once per job, plus once by wl_io_stop
    void *stack;
    atomic_bool stopping;
    uint32_t next_id;
    uint32_t outstanding; // main-thread side only
    WlSpscRing requests;  // main -> io
    WlSpscRing done;      // io -> main
    WlIoJob req_store[WL_IO_QUEUE_DEPTH];
    WlIoJob done_store[WL_IO_QUEUE_DEPTH];
} g;

static void *io_main(void *unused) {
    (void)unused;
    for (;;) {
        wl_platform_sem_wait(g.wake);
        WlIoJob job;
        if (wl_ring_pop(&g.requests, &job)) {
            g.handler(g.user, &job);
            // done has the same capacity as requests and at most WL_IO_QUEUE_DEPTH jobs are
            // outstanding (submit enforces it), so this cannot fail.
            bool ok = wl_ring_push(&g.done, &job);
            WL_ASSERT(ok);
            (void)ok;
        } else if (atomic_load_explicit(&g.stopping, memory_order_acquire)) {
            break; // the stop post: every earlier job's post was already consumed
        }
    }
    return NULL;
}

WlResult wl_io_start(WlIoHandler handler, void *user) {
    if (handler == NULL) {
        return WL_ERR_ARG;
    }
    if (g.running || !wl_mem_ready()) {
        return WL_ERR_STATE;
    }
    g.stack = WL_MEM_ALLOC_GPU(WL_TAG_STACK, WL_IO_STACK_SIZE); // 32-byte aligned
    if (g.stack == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_platform_sem_create(&g.wake, 0, WL_IO_QUEUE_DEPTH + 1u);
    if (r == WL_OK) {
        r = wl_ring_init(&g.requests, g.req_store, sizeof(WlIoJob), WL_IO_QUEUE_DEPTH);
    }
    if (r == WL_OK) {
        r = wl_ring_init(&g.done, g.done_store, sizeof(WlIoJob), WL_IO_QUEUE_DEPTH);
    }
    if (r == WL_OK) {
        g.handler = handler;
        g.user = user;
        g.next_id = 1;
        g.outstanding = 0;
        atomic_store(&g.stopping, false);
        r = wl_platform_thread_create(&g.thread, io_main, NULL, g.stack, WL_IO_STACK_SIZE,
                                      WL_THREAD_PRIO_IO);
    }
    if (r != WL_OK) {
        wl_mem_free(g.stack);
        g.stack = NULL;
        return r;
    }
    g.running = true;
    return WL_OK;
}

WlResult wl_io_submit(WlIoJob *job) {
    if (job == NULL) {
        return WL_ERR_ARG;
    }
    if (!g.running) {
        return WL_ERR_STATE;
    }
    if (g.outstanding >= WL_IO_QUEUE_DEPTH) {
        return WL_ERR_LIMIT;
    }
    job->id = g.next_id++;
    bool ok = wl_ring_push(&g.requests, job);
    WL_ASSERT(ok);
    (void)ok;
    g.outstanding++;
    wl_platform_sem_post(g.wake); // after the push: the IO thread finds the job
    return WL_OK;
}

bool wl_io_poll(WlIoJob *out) {
    if (!g.running || !wl_ring_pop(&g.done, out)) {
        return false;
    }
    g.outstanding--;
    return true;
}

uint32_t wl_io_outstanding(void) {
    return g.running ? g.outstanding : 0;
}

bool wl_io_running(void) {
    return g.running;
}

void wl_io_stop(void) {
    if (!g.running) {
        return;
    }
    atomic_store_explicit(&g.stopping, true, memory_order_release);
    wl_platform_sem_post(g.wake);
    (void)wl_platform_thread_join(g.thread, NULL);
    wl_platform_sem_destroy(g.wake);
    wl_mem_free(g.stack);
    g.stack = NULL;
    g.running = false;
    g.outstanding = 0;
}
