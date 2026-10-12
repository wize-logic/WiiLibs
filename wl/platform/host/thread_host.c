// SPDX-License-Identifier: MIT
// Host stub of the thread part of platform.h: pthreads behind the same handles, so
// the ring and IO-thread logic is testable on the host. Priorities are ignored (the host
// scheduler is preemptive and multicore: a stronger race test than the console's) and stacks are
// validated but not used (wl_platform_run_on_stack's is). Handles are pool index + 1; pools are
// fixed and guarded by one lock. Condition variables wait on CLOCK_MONOTONIC, so a change of the
// wall clock does not stretch or cut a timeout.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>

#include "wl/platform/platform.h"
#include "wl/platform/posix.h"

#define MAX_THREADS 64u
#define MAX_SEMS 64u
#define MAX_MUTEXES 128u
#define MAX_CONDS 256u
#define MIN_STACK 4096u

typedef struct {
    bool used;
    bool detached; // nobody joins it: the slot is freed when it ends
    bool finished; // its function returned (or it called wl_platform_thread_exit)
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

typedef struct {
    bool used;
    pthread_cond_t c;
} HostCond;

static pthread_mutex_t g_pool = PTHREAD_MUTEX_INITIALIZER;
static HostThread g_threads[MAX_THREADS];
static HostSem g_sems[MAX_SEMS];
static HostMutex g_mutexes[MAX_MUTEXES];
static HostCond g_conds[MAX_CONDS];
static void *g_results[MAX_THREADS];
static _Thread_local HostThread *t_self;   // the pool thread running here (NULL: not one)
static _Thread_local uint32_t t_id;        // wl_platform_thread_self_id, handed out on first use
static atomic_uint g_next_id = 1;

bool wl_platform_thread_priorities_real(void) {
    return false;
}

// The end of a pool thread, by return or by wl_platform_thread_exit: a detached one gives its
// slot back, a joinable one keeps it for the join.
static void thread_finish(HostThread *t, void *result) {
    pthread_mutex_lock(&g_pool);
    g_results[t - g_threads] = result;
    t->finished = true;
    if (t->detached) {
        t->used = false;
    }
    pthread_mutex_unlock(&g_pool);
}

static void *thread_main(void *p) {
    HostThread *t = p;
    t_self = t;
    thread_finish(t, t->fn(t->arg));
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
    t->detached = false;
    t->finished = false;
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
    bool ok = g_threads[i].used && !g_threads[i].detached;
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

// The host's scheduler decides: the priority is checked and ignored, like create's.
WlResult wl_posix_thread_set_priority(pthread_t thread, uint32_t priority) {
    (void)thread;
    return priority > WL_POSIX_PRIO_MIN ? WL_ERR_ARG : WL_OK;
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

WlResult wl_platform_thread_detach(WlPlatformThread thread) {
    if (thread.id == 0 || thread.id > MAX_THREADS) {
        return WL_ERR_ARG;
    }
    HostThread *t = &g_threads[thread.id - 1u];
    pthread_mutex_lock(&g_pool);
    if (!t->used || t->detached) {
        pthread_mutex_unlock(&g_pool);
        return WL_ERR_STATE;
    }
    pthread_detach(t->th);
    t->detached = true;
    if (t->finished) {
        t->used = false; // it ended before anyone said it would not be joined
    }
    pthread_mutex_unlock(&g_pool);
    return WL_OK;
}

void wl_platform_thread_exit(void *result) {
    if (t_self != NULL) {
        thread_finish(t_self, result);
    }
    pthread_exit(NULL);
}

uint32_t wl_platform_thread_self_id(void) {
    if (t_id == 0) {
        t_id = atomic_fetch_add(&g_next_id, 1u);
    }
    return t_id;
}

WlResult wl_platform_thread_set_priority(WlPlatformThread thread, WlThreadPriority priority) {
    if (thread.id == 0 || thread.id > MAX_THREADS || (unsigned)priority > WL_THREAD_PRIO_AUDIO) {
        return WL_ERR_ARG;
    }
    pthread_mutex_lock(&g_pool);
    bool live = g_threads[thread.id - 1u].used && !g_threads[thread.id - 1u].finished;
    pthread_mutex_unlock(&g_pool);
    return live ? WL_OK : WL_ERR_STATE;
}

// A deadline `ms` from now on `clock`.
static struct timespec deadline_after(clockid_t clock, uint32_t ms) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

bool wl_platform_sem_wait_timeout(WlPlatformSemaphore sem, uint32_t timeout_ms) {
    if (timeout_ms == WL_WAIT_FOREVER) {
        wl_platform_sem_wait(sem);
        return true;
    }
    HostSem *s = sem_of(sem);
    // the semaphore's condition variable keeps the default clock (realtime): the deadline is taken
    // on that clock
    struct timespec until = deadline_after(CLOCK_REALTIME, timeout_ms);
    pthread_mutex_lock(&s->m);
    while (s->count == 0) {
        if (timeout_ms == 0 || pthread_cond_timedwait(&s->c, &s->m, &until) == ETIMEDOUT) {
            break;
        }
    }
    bool got = s->count > 0;
    if (got) {
        s->count--;
    }
    pthread_mutex_unlock(&s->m);
    return got;
}

WlResult wl_platform_mutex_create_recursive(WlPlatformMutex *out) {
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
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    g_mutexes[i].used = true;
    pthread_mutex_init(&g_mutexes[i].m, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_mutex_unlock(&g_pool);
    out->id = i + 1u;
    return WL_OK;
}

bool wl_platform_mutex_try_lock(WlPlatformMutex mutex) {
    return pthread_mutex_trylock(&mutex_of(mutex)->m) == 0;
}

WlResult wl_platform_cond_create(WlPlatformCond *out) {
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    pthread_mutex_lock(&g_pool);
    uint32_t i = 0;
    while (i < MAX_CONDS && g_conds[i].used) {
        i++;
    }
    if (i == MAX_CONDS) {
        pthread_mutex_unlock(&g_pool);
        return WL_ERR_LIMIT;
    }
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    g_conds[i].used = true;
    pthread_cond_init(&g_conds[i].c, &attr);
    pthread_condattr_destroy(&attr);
    pthread_mutex_unlock(&g_pool);
    out->id = i + 1u;
    return WL_OK;
}

static HostCond *cond_of(WlPlatformCond h) {
    WL_ASSERT(h.id >= 1 && h.id <= MAX_CONDS && g_conds[h.id - 1u].used);
    return &g_conds[h.id - 1u];
}

void wl_platform_cond_wait(WlPlatformCond cond, WlPlatformMutex mutex) {
    pthread_cond_wait(&cond_of(cond)->c, &mutex_of(mutex)->m);
}

bool wl_platform_cond_wait_timeout(WlPlatformCond cond, WlPlatformMutex mutex, uint32_t timeout_ms) {
    if (timeout_ms == WL_WAIT_FOREVER) {
        wl_platform_cond_wait(cond, mutex);
        return true;
    }
    HostMutex *m = mutex_of(mutex);
    if (timeout_ms == 0) {
        pthread_mutex_unlock(&m->m);
        sched_yield();
        pthread_mutex_lock(&m->m);
        return false;
    }
    struct timespec until = deadline_after(CLOCK_MONOTONIC, timeout_ms);
    return pthread_cond_timedwait(&cond_of(cond)->c, &m->m, &until) != ETIMEDOUT;
}

void wl_platform_cond_signal(WlPlatformCond cond) {
    pthread_cond_signal(&cond_of(cond)->c);
}

void wl_platform_cond_broadcast(WlPlatformCond cond) {
    pthread_cond_broadcast(&cond_of(cond)->c);
}

void wl_platform_cond_destroy(WlPlatformCond cond) {
    HostCond *c = cond_of(cond);
    pthread_mutex_lock(&g_pool);
    pthread_cond_destroy(&c->c);
    c->used = false;
    pthread_mutex_unlock(&g_pool);
}
