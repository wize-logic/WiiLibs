// SPDX-License-Identifier: MIT
#include "wl/host/thread.h"

static void *trampoline(void *p) {
    WlThread *t = (WlThread *)p;
    t->fn(t->arg);
    return NULL;
}

WlResult wl_thread_start(WlThread *t, void (*fn)(void *), void *arg) {
    t->fn = fn;
    t->arg = arg;
    return pthread_create(&t->handle, NULL, trampoline, t) == 0 ? WL_OK : WL_ERR_FAILED;
}

void wl_thread_join(WlThread *t) {
    (void)pthread_join(t->handle, NULL);
}

WlResult wl_mutex_init(WlMutex *m) {
    return pthread_mutex_init(&m->m, NULL) == 0 ? WL_OK : WL_ERR_FAILED;
}
void wl_mutex_lock(WlMutex *m) {
    (void)pthread_mutex_lock(&m->m);
}
void wl_mutex_unlock(WlMutex *m) {
    (void)pthread_mutex_unlock(&m->m);
}
void wl_mutex_destroy(WlMutex *m) {
    (void)pthread_mutex_destroy(&m->m);
}
WlResult wl_cond_init(WlCond *c) {
    return pthread_cond_init(&c->c, NULL) == 0 ? WL_OK : WL_ERR_FAILED;
}
void wl_cond_wait(WlCond *c, WlMutex *m) {
    (void)pthread_cond_wait(&c->c, &m->m);
}
void wl_cond_broadcast(WlCond *c) {
    (void)pthread_cond_broadcast(&c->c);
}
void wl_cond_destroy(WlCond *c) {
    (void)pthread_cond_destroy(&c->c);
}
