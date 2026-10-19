// SPDX-License-Identifier: MIT
// The service thread and the guest lock:
// - the service thread queued for the lock while the holder's poll runs the work it came for is
//   sent back unqueued (no round trip of the lock), unless the switch "oldservice" is set;
// - the title's exit ends within a bound (boot.c's wind-down): a guest thread that holds the lock
//   and never lets it go (parked for good) neither keeps the service thread's stop waiting (its wait
//   for the lock gives up: wl_xb_service_stop_within) nor the boot thread's last take of the lock
//   (wl_xb_lock_within times out and says who holds it).
#include <stdint.h>

#include "greatest.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/thread.h"

static WlXbThread *g_boot;
static WlPlatformSemaphore g_release;
static volatile uint32_t g_holding, g_handled;

static void handler(void *user, uint32_t reasons) {
    (void)user;
    (void)reasons;
    __atomic_add_fetch(&g_handled, 1u, __ATOMIC_SEQ_CST);
}

// holds the guest lock until the test lets it go (a holder parked for good)
static void stuck(WlXbThread *self, void *user) {
    (void)self;
    (void)user;
    __atomic_store_n(&g_holding, 1u, __ATOMIC_SEQ_CST);
    wl_platform_sem_wait(g_release);
    __atomic_store_n(&g_holding, 2u, __ATOMIC_SEQ_CST);
}

static bool until(volatile uint32_t *v, uint32_t value) {
    for (unsigned ms = 0; ms < 10000u; ms++) {
        if (__atomic_load_n(v, __ATOMIC_SEQ_CST) == value) {
            return true;
        }
        wl_platform_thread_sleep_us(1000);
    }
    return false;
}

// the service thread is in the ready queue (it waits for the lock the boot thread holds)
static bool service_queued(void) {
    for (unsigned ms = 0; ms < 10000u; ms++) {
        WlXbThread *head = wl_xb_thread_ready_head();
        if (head != NULL && head->is_service) {
            return true;
        }
        wl_platform_thread_sleep_us(1000);
    }
    return false;
}

// One raise of work while the boot thread holds the lock: its next safe point's poll runs it, then
// (`old` false) sends the queued service thread back, or (`old`) hands it the lock and gets it back.
static enum greatest_test_res holder_runs_the_work(bool old) {
    WlXbThreadStats before, after;
    if (old) {
        g_xb_compat |= WL_XB_COMPAT_OLDSERVICE;
    } else {
        g_xb_compat &= ~WL_XB_COMPAT_OLDSERVICE;
    }
    uint32_t handled = g_handled;
    wl_xb_service_raise(WL_XB_SVC_DEVICE);
    ASSERT(service_queued());
    wl_xb_thread_stats(&before);
    wl_xb_safe_point();
    wl_xb_thread_stats(&after);
    ASSERT_EQ(handled + 1u, g_handled); // (the holder's poll ran it)
    ASSERT_EQ(g_boot, wl_xb_thread_lock_holder());
    if (old) {
        ASSERT_EQ(before.handoffs + 2u, after.handoffs); // to the service thread and back
        ASSERT_EQ(before.service_spared, after.service_spared);
    } else {
        ASSERT_EQ(before.handoffs, after.handoffs);
        ASSERT_EQ(before.service_spared + 1u, after.service_spared);
        ASSERT(wl_xb_thread_ready_head() == NULL);
    }
    // (the service thread back asleep: nothing more ran)
    wl_platform_thread_sleep_us(20000);
    ASSERT_EQ(handled + 1u, g_handled);
    PASS();
}

TEST the_holder_that_ran_the_work_keeps_the_lock(void) {
    CHECK_CALL(holder_runs_the_work(false));
    CHECK_CALL(holder_runs_the_work(true));
    g_xb_compat &= ~WL_XB_COMPAT_OLDSERVICE;
    PASS();
}

TEST a_holder_that_never_lets_go_does_not_hold_the_exit(void) {
    WlXbThread *holder = NULL;

    ASSERT_EQ(WL_OK, wl_platform_sem_create(&g_release, 0, 1));
    ASSERT_EQ(WL_OK, wl_xb_thread_create_host("stuck", stuck, NULL, 0, WL_XB_PRIORITY_NORMAL + 1, &holder));
    wl_xb_unlock(g_boot);
    ASSERT(until(&g_holding, 1u));
    ASSERT_EQ(holder, wl_xb_thread_lock_holder());
    // interrupt work: the service thread waits for the lock the holder keeps
    uint32_t handled = g_handled;
    wl_xb_service_raise(WL_XB_SVC_DEVICE);
    ASSERT(service_queued());
    ASSERT_EQ(handled, g_handled);

    uint64_t t0 = wl_platform_time_ms();
    ASSERT(wl_xb_service_stop_within(3000)); // (its wait for the lock gave up: it ended)
    uint64_t stop_ms = wl_platform_time_ms() - t0;
    ASSERT(stop_ms < 2000u);
    ASSERT_EQ(handled, g_handled);

    t0 = wl_platform_time_ms();
    ASSERT_FALSE(wl_xb_lock_within(g_boot, 300));
    uint64_t lock_ms = wl_platform_time_ms() - t0;
    ASSERT(lock_ms >= 290u && lock_ms < 2000u);
    ASSERT_EQ(holder, wl_xb_thread_lock_holder());
    ASSERT_FALSE(g_boot->ready); // (out of the queue again)

    // let go: the lock comes back to a bounded take
    wl_platform_sem_post(g_release);
    ASSERT(until(&g_holding, 2u));
    ASSERT(wl_xb_lock_within(g_boot, 5000));
    ASSERT_EQ(g_boot, wl_xb_thread_lock_holder());
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    WlXbMemConfig cfg;
    WlXbThreadConfig tcfg;
    WlXbServiceConfig sc;

    GREATEST_MAIN_BEGIN();
    wl_xb_mem_config_default(&cfg);
    wl_xb_thread_config_default(&tcfg);
    wl_xb_service_config_default(&sc);
    sc.vblank = false;
    wl_xb_service_set_handler(handler, NULL);
    if (wl_xb_mem_init(&cfg) != WL_OK || wl_xb_threads_init(&tcfg, &g_boot) != WL_OK ||
        wl_xb_service_start(&sc) != WL_OK) {
        return 2;
    }
    RUN_TEST(the_holder_that_ran_the_work_keeps_the_lock);
    RUN_TEST(a_holder_that_never_lets_go_does_not_hold_the_exit);
    GREATEST_MAIN_END();
}
