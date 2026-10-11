// SPDX-License-Identifier: MIT
// Host stub of the thread part of platform.h: pthreads behind the same handles, so
// the ring and IO-thread logic is testable on the host. Priorities are ignored (the host
// scheduler is preemptive and multicore: a stronger race test than the console's) and stacks are
// validated but not used (wl_platform_run_on_stack's is). Handles are pool index + 1; pools are
// fixed and guarded by one lock.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "wl/platform/platform.h"

#define MAX_THREADS 16u
#define MAX_SEMS 64u
#define MAX_MUTEXES 64u
#define MIN_STACK 4096u

typedef struct {
    bool used;
    pthread_t th;
    WlPlatformThreadFn fn;
    void *arg;
} HostThread;

typedef struct {
    bool used;
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t count;
    uint32_t max;
} HostSem;

typedef struct {
    bool used;
    pthread_mutex_t m;
} HostMutex;

static pthread_mutex_t g_pool = PTHREAD_MUTEX_INITIALIZER;
static HostThread g_threads[MAX_THREADS];
static HostSem g_sems[MAX_SEMS];
static HostMutex g_mutexes[MAX_MUTEXES];
static void *g_results[MAX_THREADS];

bool wl_platform_thread_priorities_real(void) {
    return false;
}

static void *thread_main(void *p) {
    HostThread *t = p;
    g_results[t - g_threads] = t->fn(t->arg);
    return NULL;
}

WlResult wl_platform_thread_create(WlPlatformThread *out, WlPlatformThreadFn fn, void *arg,
                                   void *stack, size_t stack_size, WlThreadPriority priority) {
    if (out == NULL || fn == NULL || stack == NULL || ((uintptr_t)stack & 31u) != 0 ||
        stack_size < MIN_STACK || (stack_size & 31u) != 0 || (unsigned)priority > WL_THREAD_PRIO_AUDIO) {
        return WL_ERR_ARG;
    }
    pthread_mutex_lock(&g_pool);
    uint32_t i = 0;
    while (i < MAX_THREADS && g_threads[i].used) {
        i++;
    }
    if (i == MAX_THREADS) {
        pthread_mutex_unlock(&g_pool);
        return WL_ERR_LIMIT;
    }
    HostThread *t = &g_threads[i];
    t->used = true;
    t->fn = fn;
    t->arg = arg;
    g_results[i] = NULL;
    int rc = pthread_create(&t->th, NULL, thread_main, t);
    if (rc != 0) {
        t->used = false;
        pthread_mutex_unlock(&g_pool);
        return WL_ERR_FAILED;
    }
    pthread_mutex_unlock(&g_pool);
    out->id = i + 1u;
    return WL_OK;
}

WlResult wl_platform_thread_join(WlPlatformThread thread, void **out_result) {
    if (thread.id == 0 || thread.id > MAX_THREADS) {
        return WL_ERR_ARG;
    }
    uint32_t i = thread.id - 1u;
    pthread_mutex_lock(&g_pool);
    bool ok = g_threads[i].used;
    pthread_t th = g_threads[i].th;
    pthread_mutex_unlock(&g_pool);
    if (!ok || pthread_join(th, NULL) != 0) {
        return WL_ERR_STATE;
    }
    if (out_result != NULL) {
        *out_result = g_results[i];
    }
    pthread_mutex_lock(&g_pool);
    g_threads[i].used = false;
    pthread_mutex_unlock(&g_pool);
    return WL_OK;
}

void wl_platform_thread_yield(void) {
    sched_yield();
}

typedef struct {
    WlPlatformThreadFn fn;
    void *arg;
    void *result;
} StackRun;

static void *stack_run_main(void *p) {
    StackRun *run = p;
    run->result = run->fn(run->arg);
    return NULL;
}

// Not through the pool: this thread really runs on the caller's stack, so the depth is measurable.
WlResult wl_platform_run_on_stack(WlPlatformThreadFn fn, void *arg, void *stack, size_t stack_size,
                                  void **out_result) {
    if (fn == NULL || stack == NULL || ((uintptr_t)stack & 31u) != 0 || stack_size < MIN_STACK ||
        (stack_size & 31u) != 0) {
        return WL_ERR_ARG;
    }
    wl_platform_stack_fill(stack, stack_size);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return WL_ERR_FAILED;
    }
    if (pthread_attr_setstack(&attr, stack, stack_size) != 0) {
        pthread_attr_destroy(&attr);
        return WL_ERR_ARG;
    }
    StackRun run = {fn, arg, NULL};
    pthread_t th;
    int rc = pthread_create(&th, &attr, stack_run_main, &run);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        return rc == EINVAL ? WL_ERR_ARG : WL_ERR_FAILED;
    }
    if (pthread_join(th, NULL) != 0) {
        return WL_ERR_STATE;
    }
    if (out_result != NULL) {
        *out_result = run.result;
    }
    return WL_OK;
}

void wl_platform_thread_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
}

WlResult wl_platform_sem_create(WlPlatformSemaphore *out, uint32_t initial, uint32_t max) {
    if (out == NULL || max == 0 || initial > max) {
        return WL_ERR_ARG;
    }
    pthread_mutex_lock(&g_pool);
    uint32_t i = 0;
    while (i < MAX_SEMS && g_sems[i].used) {
        i++;
    }
    if (i == MAX_SEMS) {
        pthread_mutex_unlock(&g_pool);
        return WL_ERR_LIMIT;
    }
    HostSem *s = &g_sems[i];
    s->used = true;
    s->count = initial;
    s->max = max;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    pthread_mutex_unlock(&g_pool);
    out->id = i + 1u;
    return WL_OK;
}

static HostSem *sem_of(WlPlatformSemaphore h) {
    WL_ASSERT(h.id >= 1 && h.id <= MAX_SEMS && g_sems[h.id - 1u].used);
    return &g_sems[h.id - 1u];
}

void wl_platform_sem_post(WlPlatformSemaphore sem) {
    HostSem *s = sem_of(sem);
    pthread_mutex_lock(&s->m);
    if (s->count < s->max) {
        s->count++;
        pthread_cond_signal(&s->c);
    }
    pthread_mutex_unlock(&s->m);
}

void wl_platform_sem_wait(WlPlatformSemaphore sem) {
    HostSem *s = sem_of(sem);
    pthread_mutex_lock(&s->m);
    while (s->count == 0) {
        pthread_cond_wait(&s->c, &s->m);
    }
    s->count--;
    pthread_mutex_unlock(&s->m);
}

void wl_platform_sem_destroy(WlPlatformSemaphore sem) {
    HostSem *s = sem_of(sem);
    pthread_mutex_lock(&g_pool);
    pthread_cond_destroy(&s->c);
    pthread_mutex_destroy(&s->m);
    s->used = false;
    pthread_mutex_unlock(&g_pool);
}

WlResult wl_platform_mutex_create(WlPlatformMutex *out) {
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    pthread_mutex_lock(&g_pool);
    uint32_t i = 0;
    while (i < MAX_MUTEXES && g_mutexes[i].used) {
        i++;
    }
    if (i == MAX_MUTEXES) {
        pthread_mutex_unlock(&g_pool);
        return WL_ERR_LIMIT;
    }
    g_mutexes[i].used = true;
    pthread_mutex_init(&g_mutexes[i].m, NULL);
    pthread_mutex_unlock(&g_pool);
    out->id = i + 1u;
    return WL_OK;
}

static HostMutex *mutex_of(WlPlatformMutex h) {
    WL_ASSERT(h.id >= 1 && h.id <= MAX_MUTEXES && g_mutexes[h.id - 1u].used);
    return &g_mutexes[h.id - 1u];
}

void wl_platform_mutex_lock(WlPlatformMutex mutex) {
    pthread_mutex_lock(&mutex_of(mutex)->m);
}

void wl_platform_mutex_unlock(WlPlatformMutex mutex) {
    pthread_mutex_unlock(&mutex_of(mutex)->m);
}

void wl_platform_mutex_destroy(WlPlatformMutex mutex) {
    HostMutex *m = mutex_of(mutex);
    pthread_mutex_lock(&g_pool);
    pthread_mutex_destroy(&m->m);
    m->used = false;
    pthread_mutex_unlock(&g_pool);
}

void wl_platform_tls_thread_exit(void) {
}
