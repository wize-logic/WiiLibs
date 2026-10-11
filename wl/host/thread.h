// SPDX-License-Identifier: MIT
// Small threading wrapper (pthreads on the host). The job runner uses only this, so the
// on-console installer can supply its own implementation (libogc LWP).
#pragma once

#include <pthread.h>

#include "wl/host/base.h"

typedef struct {
    pthread_t handle;
    void (*fn)(void *);
    void *arg;
} WlThread;
typedef struct {
    pthread_mutex_t m;
} WlMutex;
typedef struct {
    pthread_cond_t c;
} WlCond;

WlResult wl_thread_start(WlThread *t, void (*fn)(void *), void *arg);
void wl_thread_join(WlThread *t);
WlResult wl_mutex_init(WlMutex *m);
void wl_mutex_lock(WlMutex *m);
void wl_mutex_unlock(WlMutex *m);
void wl_mutex_destroy(WlMutex *m);
WlResult wl_cond_init(WlCond *c);
void wl_cond_wait(WlCond *c, WlMutex *m);
void wl_cond_broadcast(WlCond *c);
void wl_cond_destroy(WlCond *c);
