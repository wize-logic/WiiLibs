// SPDX-License-Identifier: MIT
// Wii thread backend: LWP threads, counting semaphores and mutexes behind the
// handles of platform.h, plus the priority table of WlThreadPriority. Only this directory may
// include <ogc/*.h>. Handles are the 32-bit LWP ids by value.
#define _POSIX_C_SOURCE 200809L
#include <ogc/lwp.h>
#include <ogc/mutex.h>
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
