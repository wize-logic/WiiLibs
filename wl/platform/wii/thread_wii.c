// SPDX-License-Identifier: MIT
// Wii thread backend: LWP threads, counting semaphores and mutexes behind the
// handles of platform.h, plus the priority table of WlThreadPriority. Only this directory may
// include <ogc/*.h>. Handles are the 32-bit LWP ids by value.
#define _POSIX_C_SOURCE 200809L
#include <ogc/irq.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/cond.h>
#include <ogc/semaphore.h>
#include <pthread.h>
#include <time.h>
#include <tuxedo/thread.h>

#include "wl/platform/platform.h"
#include "wl/platform/posix.h"

#define MIN_STACK 4096u

// LWP priority numbers (0 idle .. 127 highest), indexed by WlThreadPriority.
static const uint8_t k_lwp_prio[] = {
    32,                 // WL_THREAD_PRIO_IO    below main
    WL_POSIX_PRIO_MAIN, // WL_THREAD_PRIO_MAIN  (posix.h places pthreads around it)
    80,                 // WL_THREAD_PRIO_NET   above main
    100,                // WL_THREAD_PRIO_AUDIO high
};

static bool g_main_pinned;

static void reap_detached(void);

bool wl_platform_thread_priorities_real(void) {
    return true;
}

WlResult wl_platform_thread_create(WlPlatformThread *out, WlPlatformThreadFn fn, void *arg,
                                   void *stack, size_t stack_size, WlThreadPriority priority) {
    if (out == NULL || fn == NULL || stack == NULL || ((uintptr_t)stack & 31u) != 0 ||
        stack_size < MIN_STACK || (stack_size & 31u) != 0 ||
        (unsigned)priority >= WL_ARRAY_COUNT(k_lwp_prio)) {
        return WL_ERR_ARG;
    }
    if (!g_main_pinned) {
        // The creating thread is the main thread: make the table authoritative for it.
        LWP_SetThreadPriority(LWP_GetSelf(), k_lwp_prio[WL_THREAD_PRIO_MAIN]);
        g_main_pinned = true;
    }
    reap_detached();
    lwp_t t = 0;
    if (LWP_CreateThread(&t, fn, arg, stack, (u32)stack_size, k_lwp_prio[priority]) < 0) {
        return WL_ERR_FAILED;
    }
    out->id = t;
    return WL_OK;
}

WlResult wl_platform_thread_join(WlPlatformThread thread, void **out_result) {
    void *ret = NULL;
    if (LWP_JoinThread(thread.id, &ret) < 0) {
        return WL_ERR_STATE;
    }
    if (out_result != NULL) {
        *out_result = ret;
    }
    return WL_OK;
}

void wl_platform_thread_yield(void) {
    LWP_YieldThread();
}

// devkitPPC's sched_yield returns ENOSYS without yielding; the link's --wrap=sched_yield
// (wl_platform's CMake target) sends its callers here.
int __wrap_sched_yield(void);
int __wrap_sched_yield(void) {
    LWP_YieldThread();
    return 0;
}

WlResult wl_platform_run_on_stack(WlPlatformThreadFn fn, void *arg, void *stack, size_t stack_size,
                                  void **out_result) {
    if (fn == NULL || stack == NULL) {
        return WL_ERR_ARG;
    }
    wl_platform_stack_fill(stack, stack_size);
    WlPlatformThread t;
    WlResult r = wl_platform_thread_create(&t, fn, arg, stack, stack_size, WL_THREAD_PRIO_MAIN);
    if (r != WL_OK) {
        return r;
    }
    return wl_platform_thread_join(t, out_result);
}

void wl_platform_thread_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL); // libogc blocks only the calling thread (timed LWP sleep)
}

WlResult wl_platform_sem_create(WlPlatformSemaphore *out, uint32_t initial, uint32_t max) {
    if (out == NULL || max == 0 || initial > max) {
        return WL_ERR_ARG;
    }
    sem_t s = 0;
    if (LWP_SemInit(&s, initial, max) < 0) {
        return WL_ERR_NOMEM;
    }
    out->id = s;
    return WL_OK;
}

void wl_platform_sem_post(WlPlatformSemaphore sem) {
    (void)LWP_SemPost(sem.id); // beyond max: dropped
}

void wl_platform_sem_wait(WlPlatformSemaphore sem) {
    s32 r = LWP_SemWait(sem.id);
    WL_ASSERT(r == 0);
    (void)r;
}

void wl_platform_sem_destroy(WlPlatformSemaphore sem) {
    (void)LWP_SemDestroy(sem.id);
}

WlResult wl_platform_mutex_create(WlPlatformMutex *out) {
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    mutex_t m = 0;
    if (LWP_MutexInit(&m, false) < 0) {
        return WL_ERR_NOMEM;
    }
    out->id = m;
    return WL_OK;
}

void wl_platform_mutex_lock(WlPlatformMutex mutex) {
    s32 r = LWP_MutexLock(mutex.id);
    WL_ASSERT(r == 0);
    (void)r;
}

void wl_platform_mutex_unlock(WlPlatformMutex mutex) {
    s32 r = LWP_MutexUnlock(mutex.id);
    WL_ASSERT(r == 0);
    (void)r;
}

void wl_platform_mutex_destroy(WlPlatformMutex mutex) {
    (void)LWP_MutexDestroy(mutex.id);
}

// ---- Detached threads, exit, ids, priorities ------------------------------------------------
// An lwp_t is the bitwise complement of its KThread's address (LWP_JoinThread does
// KThreadJoin((KThread *)~t)), not the address itself.
static KThread *kthread_of(lwp_t t) {
    return (KThread *)(uintptr_t)~t;
}

// An LWP thread's control block (a KThread, which LWP_CreateThread allocates; the lwp_t is its
// address) is freed by LWP_JoinThread. A detached thread is remembered here and joined once it
// has finished, at the next detach or create: no thread waits for it.
#define MAX_DETACHED 64u
static lwp_t g_detached[MAX_DETACHED];

static void reap_detached(void) {
    for (uint32_t i = 0; i < MAX_DETACHED; i++) {
        lwp_t t = g_detached[i];
        if (t != 0 && kthread_of(t)->state == KTHR_STATE_FINISHED) {
            g_detached[i] = 0;
            (void)LWP_JoinThread(t, NULL); // finished: returns at once and frees the block
        }
    }
}

WlResult wl_platform_thread_detach(WlPlatformThread thread) {
    if (thread.id == 0 || thread.id == LWP_THREAD_NULL) {
        return WL_ERR_ARG;
    }
    reap_detached();
    for (uint32_t i = 0; i < MAX_DETACHED; i++) {
        if (g_detached[i] == 0) {
            g_detached[i] = thread.id;
            return WL_OK;
        }
    }
    return WL_ERR_LIMIT; // still joinable
}

void wl_platform_thread_exit(void *result) {
    KThreadExit((sptr)(uintptr_t)result);
    for (;;) {
    }
}

uint32_t wl_platform_thread_self_id(void) {
    return LWP_GetSelf();
}

WlResult wl_platform_thread_set_priority(WlPlatformThread thread, WlThreadPriority priority) {
    if (thread.id == 0 || (unsigned)priority >= WL_ARRAY_COUNT(k_lwp_prio)) {
        return WL_ERR_ARG;
    }
    LWP_SetThreadPriority(thread.id, k_lwp_prio[priority]);
    return WL_OK;
}

// libogc's semaphores have no timed wait (they block on a thread queue without a deadline), so a
// wait with a deadline tries, then sleeps a millisecond and tries again: up to a millisecond late,
// and it may lose a count to an untimed waiter that arrives meanwhile.
bool wl_platform_sem_wait_timeout(WlPlatformSemaphore sem, uint32_t timeout_ms) {
    if (timeout_ms == WL_WAIT_FOREVER) {
        wl_platform_sem_wait(sem);
        return true;
    }
    uint64_t start = wl_platform_time_ms();
    for (;;) {
        u32 value = 0;
        u32 level = IRQ_Disable();
        bool ready = LWP_SemGetValue(sem.id, &value) == 0 && value > 0;
        if (ready) {
            (void)LWP_SemWait(sem.id); // a count is there: does not block
        }
        IRQ_Restore(level);
        if (ready) {
            return true;
        }
        if (wl_platform_time_ms() - start >= timeout_ms) {
            return false;
        }
        wl_platform_thread_sleep_ms(1);
    }
}

WlResult wl_platform_mutex_create_recursive(WlPlatformMutex *out) {
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    mutex_t m = 0;
    if (LWP_MutexInit(&m, true) < 0) {
        return WL_ERR_NOMEM;
    }
    out->id = m;
    return WL_OK;
}

bool wl_platform_mutex_try_lock(WlPlatformMutex mutex) {
    return LWP_MutexTryLock(mutex.id) == 0;
}

WlResult wl_platform_cond_create(WlPlatformCond *out) {
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    cond_t c = 0;
    if (LWP_CondInit(&c) < 0) {
        return WL_ERR_NOMEM;
    }
    out->id = c;
    return WL_OK;
}

void wl_platform_cond_wait(WlPlatformCond cond, WlPlatformMutex mutex) {
    (void)LWP_CondWait(cond.id, mutex.id);
}

bool wl_platform_cond_wait_timeout(WlPlatformCond cond, WlPlatformMutex mutex, uint32_t timeout_ms) {
    if (timeout_ms == WL_WAIT_FOREVER) {
        wl_platform_cond_wait(cond, mutex);
        return true;
    }
    if (timeout_ms == 0) {
        (void)LWP_MutexUnlock(mutex.id);
        LWP_YieldThread();
        (void)LWP_MutexLock(mutex.id);
        return false;
    }
    struct timespec rel = {(time_t)(timeout_ms / 1000u), (long)(timeout_ms % 1000u) * 1000000L};
    return LWP_CondTimedWait(cond.id, mutex.id, &rel) == 0;
}

void wl_platform_cond_signal(WlPlatformCond cond) {
    (void)LWP_CondSignal(cond.id);
}

void wl_platform_cond_broadcast(WlPlatformCond cond) {
    (void)LWP_CondBroadcast(cond.id);
}

void wl_platform_cond_destroy(WlPlatformCond cond) {
    (void)LWP_CondDestroy(cond.id);
}

// A pthread_t is libogc's thread: the block devkitPPC's pthread_create allocates begins with the
// KThread. A thread that is not one (an id that is not a live thread's) shows as a priority past
// the lowest, or not at all.
WlResult wl_posix_thread_set_priority(pthread_t thread, uint32_t priority) {
    KThread *kernel_thread = (KThread *)(uintptr_t)thread;
    if (priority > WL_POSIX_PRIO_MIN) {
        return WL_ERR_ARG;
    }
    if (kernel_thread == NULL || kernel_thread->prio > KTHR_MIN_PRIO) {
        return WL_ERR_STATE;
    }
    KThreadSetPrio(kernel_thread, (u16)priority);
    return WL_OK;
}
