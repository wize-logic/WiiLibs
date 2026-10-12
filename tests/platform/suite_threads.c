// SPDX-License-Identifier: MIT
// The threads suite: thread wrappers, the SPSC ring under real preemption, the IO thread, the
// priority table, and the heap lock under contention. Platform-independent code only:
// test_platform_thread.c runs it against the pthreads host stub (where priorities are not
// enforced, so the order checks that need them are skipped via
// wl_platform_thread_priorities_real); an on-console runner can run the same file.
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "wl/platform/io.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "wl/platform/ring.h"
#include "tests/platform/check.h"

#define ROUNDS 10000u
#define STACK_SIZE (16u * 1024u)
#define MAX_WORKERS 8u

typedef struct {
    WlPlatformThread th;
    void *stack;
    bool live;
} Worker;

static bool spawn(Worker *w, WlPlatformThreadFn fn, void *arg, WlThreadPriority prio) {
    w->stack = WL_MEM_ALLOC_GPU(WL_TAG_STACK, STACK_SIZE);
    if (w->stack == NULL) {
        return false;
    }
    if (wl_platform_thread_create(&w->th, fn, arg, w->stack, STACK_SIZE, prio) != WL_OK) {
        wl_mem_free(w->stack);
        w->stack = NULL;
        return false;
    }
    w->live = true;
    return true;
}

static void *reap(Worker *w) {
    void *ret = NULL;
    if (w->live) {
        if (wl_platform_thread_join(w->th, &ret) != WL_OK) {
            ret = (void *)(uintptr_t)0xBAD;
        }
        wl_mem_free(w->stack);
        w->live = false;
    }
    return ret;
}

static uint32_t mix(uint32_t i) {
    return i * 2654435761u + 12345u;
}

// ---------------------------------------------------------------------------------------------
// API checks and the ring on one thread
// ---------------------------------------------------------------------------------------------

static void *return_arg(void *arg) {
    return arg;
}

static void check_api(void) {
    WlPlatformThread t;
    uint8_t *s = WL_MEM_ALLOC_GPU(WL_TAG_STACK, STACK_SIZE);
    wlt_check("thread_arg_null_stack",
              wl_platform_thread_create(&t, return_arg, NULL, NULL, STACK_SIZE,
                                        WL_THREAD_PRIO_IO) == WL_ERR_ARG,
              "NULL stack refused");
    wlt_check("thread_arg_misaligned",
              wl_platform_thread_create(&t, return_arg, NULL, s + 8, STACK_SIZE - 32u,
                                        WL_THREAD_PRIO_IO) == WL_ERR_ARG,
              "misaligned stack refused");
    wlt_check("thread_arg_small",
              wl_platform_thread_create(&t, return_arg, NULL, s, 256, WL_THREAD_PRIO_IO) ==
                  WL_ERR_ARG,
              "tiny stack refused");
    WlPlatformSemaphore sem;
    wlt_check("sem_arg", wl_platform_sem_create(&sem, 3, 2) == WL_ERR_ARG, "initial > max refused");
    wl_mem_free(s);

    Worker w;
    bool ok = spawn(&w, return_arg, (void *)(uintptr_t)0x1234, WL_THREAD_PRIO_NET);
    wlt_check("thread_create", ok, "thread created");
    void *ret = reap(&w);
    wlt_check("thread_join_result", ret == (void *)(uintptr_t)0x1234, "join returns the result");

    // A semaphore counts, and a post beyond max is dropped.
    WlPlatformSemaphore c;
    bool sem_ok = wl_platform_sem_create(&c, 0, 2) == WL_OK;
    wl_platform_sem_post(c);
    wl_platform_sem_post(c);
    wl_platform_sem_post(c); // dropped
    wl_platform_sem_wait(c);
    wl_platform_sem_wait(c); // would block forever if the third post had counted twice
    wlt_check("sem_counting", sem_ok, "two posts, two waits");
    wl_platform_sem_destroy(c);

    WlPlatformMutex m;
    bool mu_ok = wl_platform_mutex_create(&m) == WL_OK;
    wl_platform_mutex_lock(m);
    wl_platform_mutex_unlock(m);
    wl_platform_mutex_destroy(m);
    wlt_check("mutex_basic", mu_ok, "create, lock, unlock, destroy");
}

static void check_ring_single(void) {
    uint32_t store[8];
    WlSpscRing r;
    wlt_check("ring_init_bad_capacity", wl_ring_init(&r, store, 4, 6) == WL_ERR_ARG, "not pow2");
    wlt_check("ring_init_bad_size", wl_ring_init(&r, store, 0, 8) == WL_ERR_ARG, "item size 0");
    wlt_check("ring_init", wl_ring_init(&r, store, 4, 8) == WL_OK, "8 x 4");
    uint32_t v = 0;
    bool ok = !wl_ring_pop(&r, &v) && wl_ring_peek(&r) == NULL && wl_ring_count(&r) == 0;
    for (uint32_t i = 0; i < 8; i++) {
        v = 100 + i;
        ok = ok && wl_ring_push(&r, &v);
    }
    v = 999;
    ok = ok && !wl_ring_push(&r, &v) && wl_ring_count(&r) == 8 && wl_ring_free_slots(&r) == 0;
    for (uint32_t i = 0; i < 8 && ok; i++) {
        const uint32_t *p = wl_ring_peek(&r);
        ok = p != NULL && *p == 100 + i;
        wl_ring_discard(&r);
    }
    ok = ok && wl_ring_count(&r) == 0;
    // Many wraps with a rolling occupancy.
    uint32_t next_in = 0;
    uint32_t next_out = 0;
    for (uint32_t i = 0; i < 1000u && ok; i++) {
        uint32_t burst = 1 + (i % 7);
        for (uint32_t k = 0; k < burst; k++) {
            if (wl_ring_push(&r, &next_in)) {
                next_in++;
            }
        }
        for (uint32_t k = 0; k < burst; k++) {
            if (wl_ring_pop(&r, &v)) {
                ok = ok && v == next_out;
                next_out++;
            }
        }
    }
    wlt_check("ring_single_thread", ok && next_in > 1000, "fill, drain, peek, wraparound");
}

// ---------------------------------------------------------------------------------------------
// SPSC ring between threads
// ---------------------------------------------------------------------------------------------

typedef struct {
    uint32_t seq;
    uint32_t check;
} Item;

typedef struct {
    WlSpscRing ring;
    Item store[16];
    WlPlatformSemaphore items; // counts queued items (blocking variant)
    WlPlatformSemaphore space; // counts free slots
    bool use_sems;
    uint32_t bad;      // consumer: out-of-order or corrupt items
    uint32_t received; // consumer: items seen
} Chan;

// Same-priority variant: no semaphores, both sides spin with yield. (A higher-priority spinner
// would starve a lower one: that is what the semaphore variant below is for.)
static void *consumer(void *arg) {
    Chan *c = arg;
    uint32_t expect = 0;
    while (expect < ROUNDS) {
        Item it;
        if (c->use_sems) {
            wl_platform_sem_wait(c->items);
        }
        if (!wl_ring_pop(&c->ring, &it)) {
            WL_ASSERT(!c->use_sems); // a counted wake always finds an item
            wl_platform_thread_yield();
            continue;
        }
        if (c->use_sems) {
            wl_platform_sem_post(c->space);
        }
        if (it.seq != expect || it.check != mix(it.seq)) {
            c->bad++;
        }
        expect++;
        c->received++;
    }
    return NULL;
}

static bool run_channel(Chan *c, bool use_sems, WlThreadPriority consumer_prio) {
    memset(c, 0, sizeof *c);
    c->use_sems = use_sems;
    if (wl_ring_init(&c->ring, c->store, sizeof(Item), 16) != WL_OK) {
        return false;
    }
    if (use_sems) {
        if (wl_platform_sem_create(&c->items, 0, 16) != WL_OK ||
            wl_platform_sem_create(&c->space, 16, 16) != WL_OK) {
            return false;
        }
    }
    Worker w;
    if (!spawn(&w, consumer, c, consumer_prio)) {
        return false;
    }
    for (uint32_t i = 0; i < ROUNDS; i++) {
        Item it = {i, mix(i)};
        if (use_sems) {
            wl_platform_sem_wait(c->space);
            bool ok = wl_ring_push(&c->ring, &it);
            WL_ASSERT(ok); // a counted slot always exists
            (void)ok;
            wl_platform_sem_post(c->items);
        } else {
            while (!wl_ring_push(&c->ring, &it)) {
                wl_platform_thread_yield();
            }
        }
    }
    (void)reap(&w);
    if (use_sems) {
        wl_platform_sem_destroy(c->items);
        wl_platform_sem_destroy(c->space);
    }
    return true;
}

static void check_ring_threads(void) {
    static Chan ch; // 16-byte items, 16 slots: the ring wraps ~600 times per run
    char d[96];

    bool ran = run_channel(&ch, false, WL_THREAD_PRIO_MAIN);
    snprintf(d, sizeof d, "%u rounds, %u received, %u bad", ROUNDS, ch.received, ch.bad);
    wlt_check("ring_spsc_yield_10k", ran && ch.received == ROUNDS && ch.bad == 0, d);

    ran = run_channel(&ch, true, WL_THREAD_PRIO_AUDIO); // consumer above the producer
    snprintf(d, sizeof d, "%u rounds, %u received, %u bad", ROUNDS, ch.received, ch.bad);
    wlt_check("ring_spsc_sem_consumer_high_10k", ran && ch.received == ROUNDS && ch.bad == 0, d);

    ran = run_channel(&ch, true, WL_THREAD_PRIO_IO); // consumer below the producer
    snprintf(d, sizeof d, "%u rounds, %u received, %u bad", ROUNDS, ch.received, ch.bad);
    wlt_check("ring_spsc_sem_consumer_low_10k", ran && ch.received == ROUNDS && ch.bad == 0, d);
}

// Two semaphores, 10,000 round trips; the worker adds to a shared word the main thread reads
// only after the handshake.
typedef struct {
    WlPlatformSemaphore go;
    WlPlatformSemaphore back;
    volatile uint32_t value;
    volatile bool quit;
} Ping;

static void *ping_worker(void *arg) {
    Ping *p = arg;
    for (;;) {
        wl_platform_sem_wait(p->go);
        if (p->quit) {
            return NULL;
        }
        p->value = p->value * 3u + 1u;
        wl_platform_sem_post(p->back);
    }
}

static void check_ping_pong(void) {
    static Ping p;
    memset(&p, 0, sizeof p);
    bool ok = wl_platform_sem_create(&p.go, 0, 1) == WL_OK &&
              wl_platform_sem_create(&p.back, 0, 1) == WL_OK;
    Worker w;
    ok = ok && spawn(&w, ping_worker, &p, WL_THREAD_PRIO_NET);
    uint32_t expect = 0;
    for (uint32_t i = 0; ok && i < ROUNDS; i++) {
        expect = expect * 3u + 1u;
        wl_platform_sem_post(p.go);
        wl_platform_sem_wait(p.back);
        if (p.value != expect) {
            ok = false;
        }
    }
    p.quit = true;
    wl_platform_sem_post(p.go);
    (void)reap(&w);
    wl_platform_sem_destroy(p.go);
    wl_platform_sem_destroy(p.back);
    wlt_check("sem_ping_pong_10k", ok, "10000 round trips main <-> net-priority worker");
}

// ---------------------------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------------------------

typedef struct {
    WlPlatformMutex m;
    uint32_t counter;
} Shared;

static void *mutex_worker(void *arg) {
    Shared *s = arg;
    for (uint32_t i = 0; i < 2000u; i++) {
        wl_platform_mutex_lock(s->m);
        uint32_t v = s->counter;
        if ((i & 15u) == 0) {
            wl_platform_thread_yield(); // switch away while holding the lock
        }
        s->counter = v + 1u;
        wl_platform_mutex_unlock(s->m);
    }
    return NULL;
}

static void check_mutex(void) {
    static Shared s;
    memset(&s, 0, sizeof s);
    bool ok = wl_platform_mutex_create(&s.m) == WL_OK;
    Worker w[3];
    const WlThreadPriority prio[3] = {WL_THREAD_PRIO_MAIN, WL_THREAD_PRIO_NET, WL_THREAD_PRIO_IO};
    for (int i = 0; ok && i < 3; i++) {
        ok = spawn(&w[i], mutex_worker, &s, prio[i]);
    }
    if (ok) {
        mutex_worker(&s); // the main thread joins in
    }
    for (int i = 0; i < 3; i++) {
        (void)reap(&w[i]);
    }
    wl_platform_mutex_destroy(s.m);
    char d[64];
    snprintf(d, sizeof d, "counter %u of %u", s.counter, 4u * 2000u);
    wlt_check("mutex_contention", ok && s.counter == 4u * 2000u, d);
}

// ---------------------------------------------------------------------------------------------
// Priorities (console only)
// ---------------------------------------------------------------------------------------------

typedef struct {
    atomic_uint counter;
    atomic_bool stop;
    WlPlatformSemaphore wake;
    atomic_uint woke;
} Prio;

static void *spinner(void *arg) {
    Prio *p = arg;
    while (!atomic_load(&p->stop)) {
        atomic_fetch_add(&p->counter, 1u);
    }
    return NULL;
}

static void *waiter(void *arg) {
    Prio *p = arg;
    wl_platform_sem_wait(p->wake);
    atomic_store(&p->woke, 1u);
    return NULL;
}

static void check_priorities(void) {
    if (!wl_platform_thread_priorities_real()) {
        wlt_report("INFO priorities skipped (host stub ignores them)");
        return;
    }
    static Prio p;
    memset(&p, 0, sizeof p);

    // An IO-priority thread never runs while the main thread computes, and runs when it blocks.
    Worker w;
    bool ok = spawn(&w, spinner, &p, WL_THREAD_PRIO_IO);
    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < 4000000u; i++) {
        sink += i;
    }
    (void)sink;
    unsigned during = atomic_load(&p.counter);
    wlt_check("prio_io_starved_while_main_busy", ok && during == 0, "io thread did not run");
    wl_platform_thread_sleep_ms(20); // main blocks: the io thread gets the CPU
    unsigned after = atomic_load(&p.counter);
    wlt_check("prio_io_runs_when_main_blocks", after > 0, "io thread ran during the sleep");
    atomic_store(&p.stop, true);
    (void)reap(&w);

    // An audio-priority thread preempts main the moment a post makes it ready.
    memset(&p, 0, sizeof p);
    ok = wl_platform_sem_create(&p.wake, 0, 1) == WL_OK && spawn(&w, waiter, &p, WL_THREAD_PRIO_AUDIO);
    wl_platform_thread_yield(); // lets it reach its wait (it is the higher priority: already there)
    wl_platform_sem_post(p.wake);
    unsigned woke = atomic_load(&p.woke); // read straight after the post, no blocking call between
    wlt_check("prio_audio_preempts_on_post", ok && woke == 1u, "ran before post returned");
    (void)reap(&w);
    wl_platform_sem_destroy(p.wake);

    // An IO-priority thread made ready does not preempt main.
    memset(&p, 0, sizeof p);
    ok = wl_platform_sem_create(&p.wake, 0, 1) == WL_OK && spawn(&w, waiter, &p, WL_THREAD_PRIO_IO);
    wl_platform_thread_sleep_ms(5); // let it reach its wait
    wl_platform_sem_post(p.wake);
    woke = atomic_load(&p.woke);
    wlt_check("prio_io_does_not_preempt_on_post", ok && woke == 0u, "still waiting after post");
    (void)reap(&w); // main blocks in join: now it runs
    wlt_check("prio_io_runs_after_join_blocks", atomic_load(&p.woke) == 1u, "ran once main blocked");
    wl_platform_sem_destroy(p.wake);
}

// ---------------------------------------------------------------------------------------------
// sched_yield (wrapped on the console) and running on a measured stack
// ---------------------------------------------------------------------------------------------

typedef struct {
    atomic_uint ran;
} YieldFlag;

static void *set_flag(void *arg) {
    atomic_store(&((YieldFlag *)arg)->ran, 1u);
    return NULL;
}

static void check_sched_yield(void) {
    // A thread of the main priority: on the console it runs only when main yields to it (or
    // blocks). devkitPPC's own sched_yield yields nothing, so without the wrap this spin ends
    // at its bound with the flag still clear.
    static YieldFlag f;
    atomic_store(&f.ran, 0u);
    Worker w;
    bool ok = spawn(&w, set_flag, &f, WL_THREAD_PRIO_MAIN);
    uint32_t spins = 0;
    while (ok && atomic_load(&f.ran) == 0u && spins < 1000000u) {
        int r = sched_yield();
        ok = r == 0;
        spins++;
    }
    wlt_check("sched_yield_returns_0", ok, "sched_yield() == 0 (not the ENOSYS stub)");
    wlt_check("sched_yield_runs_equal_priority", atomic_load(&f.ran) == 1u,
              "an equal-priority thread ran while main spun on sched_yield");
    (void)reap(&w);
}

#define DEEP_STACK_SIZE (256u * 1024u) // above PowerPC Linux's PTHREAD_STACK_MIN (128 KiB)
#define DEEP_FRAME 1024u
#define DEEP_LEVELS 64u // at least 64 KiB deep

static uint8_t g_deep_stack[DEEP_STACK_SIZE] __attribute__((aligned(64)));

static uint32_t deep(uint32_t level) {
    volatile uint8_t frame[DEEP_FRAME];
    for (uint32_t i = 0; i < DEEP_FRAME; i += 64u) {
        frame[i] = (uint8_t)(level + i);
    }
    uint32_t below = level > 1u ? deep(level - 1u) : 0u;
    return below + frame[DEEP_FRAME / 2u]; // used after the call: no tail call
}

static void *deep_main(void *arg) {
    uint32_t levels = (uint32_t)(uintptr_t)arg;
    size_t during = wl_platform_stack_used(g_deep_stack, DEEP_STACK_SIZE);
    uint32_t sum = deep(levels);
    (void)sum;
    // The depth seen from inside, before the deep calls: small but not 0 (this frame is there).
    return (void *)(uintptr_t)during;
}

static void check_run_on_stack(void) {
    static uint8_t buf[4096] __attribute__((aligned(32)));
    wl_platform_stack_fill(buf, sizeof buf);
    wlt_check("stack_used_untouched", wl_platform_stack_used(buf, sizeof buf) == 0, "0 when fresh");
    buf[1000] = 0;
    wlt_check("stack_used_mark", wl_platform_stack_used(buf, sizeof buf) == sizeof buf - 1000u,
              "size less the untouched low bytes");
    buf[0] = 0;
    wlt_check("stack_used_full", wl_platform_stack_used(buf, sizeof buf) == sizeof buf, "all used");
    wlt_check("stack_used_null", wl_platform_stack_used(NULL, 64) == 0, "NULL is 0");

    void *res = NULL;
    wlt_check("run_on_stack_arg",
              wl_platform_run_on_stack(deep_main, NULL, g_deep_stack + 8, DEEP_STACK_SIZE - 32u,
                                       &res) == WL_ERR_ARG,
              "misaligned stack refused");
    memset(g_deep_stack, 0, sizeof g_deep_stack);
    WlResult r = wl_platform_run_on_stack(deep_main, (void *)(uintptr_t)DEEP_LEVELS, g_deep_stack,
                                          DEEP_STACK_SIZE, &res);
    size_t used = wl_platform_stack_used(g_deep_stack, DEEP_STACK_SIZE);
    size_t during = (size_t)(uintptr_t)res;
    char detail[96];
    snprintf(detail, sizeof detail, "r=%d before=%u deepest=%u", (int)r, (unsigned)during,
             (unsigned)used);
    wlt_report("INFO run_on_stack %s", detail);
    wlt_check("run_on_stack_ok", r == WL_OK, detail);
    wlt_check("run_on_stack_measured_inside", during > 0 && during < DEEP_STACK_SIZE / 4u, detail);
    wlt_check("run_on_stack_deep", used >= DEEP_LEVELS * DEEP_FRAME && used < DEEP_STACK_SIZE,
              detail);
    wlt_check("run_on_stack_deeper_than_start", used > during + DEEP_LEVELS * DEEP_FRAME / 2u,
              detail);
}

// ---------------------------------------------------------------------------------------------
// The IO thread
// ---------------------------------------------------------------------------------------------

static atomic_uint g_io_handled;

static void io_handler(void *user, WlIoJob *job) {
    (void)user;
    job->result = (int32_t)(job->op * 2u + job->size);
    atomic_fetch_add(&g_io_handled, 1u);
}

static void check_io(void) {
    wlt_check("io_start_needs_handler", wl_io_start(NULL, NULL) == WL_ERR_ARG, "NULL handler");
    wlt_check("io_submit_not_running", wl_io_submit(&(WlIoJob){0}) == WL_ERR_STATE, "not started");
    wlt_check("io_start", wl_io_start(io_handler, NULL) == WL_OK, "thread started");
    wlt_check("io_start_twice", wl_io_start(io_handler, NULL) == WL_ERR_STATE, "already running");

    uint32_t submitted = 0;
    uint32_t polled = 0;
    uint32_t bad = 0;
    uint32_t limits = 0;
    uint32_t prev_id = 0;
    bool first_pending = false;
    while (polled < ROUNDS) {
        if (submitted < ROUNDS) {
            WlIoJob j = {0};
            j.op = submitted;
            j.size = submitted ^ 0x55u;
            WlResult r = wl_io_submit(&j);
            if (r == WL_OK) {
                submitted++;
                if (submitted == 1 && wl_platform_thread_priorities_real()) {
                    WlIoJob none;
                    // Main has not blocked since the submit: the lower-priority thread cannot
                    // have run yet.
                    first_pending = !wl_io_poll(&none);
                }
            } else if (r == WL_ERR_LIMIT) {
                limits++;
            } else {
                bad++;
            }
        }
        WlIoJob done;
        while (wl_io_poll(&done)) {
            uint32_t k = polled++;
            if (done.id != prev_id + 1u || done.op != k ||
                done.result != (int32_t)(k * 2u + (k ^ 0x55u))) {
                bad++;
            }
            prev_id = done.id;
        }
        if (submitted == ROUNDS || wl_io_outstanding() >= WL_IO_QUEUE_DEPTH) {
            wl_platform_thread_sleep_ms(1); // let the IO thread run
        }
    }
    char d[96];
    snprintf(d, sizeof d, "%u jobs, handled %u, bad %u, queue-full %u", polled, g_io_handled, bad,
             limits);
    wlt_check("io_jobs_10k", polled == ROUNDS && g_io_handled == ROUNDS && bad == 0, d);
    wlt_check("io_outstanding_zero", wl_io_outstanding() == 0, "all completions polled");
    if (wl_platform_thread_priorities_real()) {
        wlt_check("io_runs_below_main", first_pending, "no completion before main blocks");
    }

    // Stop drains queued jobs; the thread can be started again.
    atomic_store(&g_io_handled, 0);
    for (uint32_t i = 0; i < 10; i++) {
        WlIoJob j = {0};
        j.op = i;
        (void)wl_io_submit(&j);
    }
    wl_io_stop();
    wlt_check("io_stop_drains", g_io_handled == 10u && !wl_io_running(), "10 queued jobs ran");
    wlt_check("io_restart", wl_io_start(io_handler, NULL) == WL_OK, "started again");
    WlIoJob j = {0};
    j.op = 7;
    bool ok = wl_io_submit(&j) == WL_OK && j.id == 1u;
    WlIoJob done;
    while (ok && !wl_io_poll(&done)) {
        wl_platform_thread_sleep_ms(1);
    }
    wlt_check("io_restart_job", ok && done.result == 14, "job after restart");
    wl_io_stop();
}

// ---------------------------------------------------------------------------------------------
// The heap lock under contention (ENG-03 asked ENG-04 to recheck it)
// ---------------------------------------------------------------------------------------------

#define HEAP_SLOTS 12u
#define HEAP_LOW_ITERS 20000u

typedef struct {
    uint32_t seed;
    uint32_t sleep_every; // 0: never sleep (the low-priority hammer)
    uint32_t iters;       // 0: run until `done`
    uint32_t bad;
    uint32_t ops;
    uint32_t preempt_samples; // times a sleeper woke while the hammer was inside the allocator
} HeapJob;

static atomic_bool g_heap_done;
static atomic_uint g_heap_in_call;

static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void *heap_worker(void *arg) {
    HeapJob *j = arg;
    void *slot[HEAP_SLOTS] = {0};
    uint32_t len[HEAP_SLOTS] = {0};
    uint8_t pat[HEAP_SLOTS] = {0};
    uint32_t rng = j->seed;
    bool hammer = j->sleep_every == 0;
    for (uint32_t i = 0; hammer ? i < j->iters : !atomic_load(&g_heap_done); i++) {
        if (!hammer && (i % j->sleep_every) == 0) {
            wl_platform_thread_sleep_ms(1); // a timer wake-up lands at an arbitrary instruction
            if (atomic_load(&g_heap_in_call) != 0) {
                j->preempt_samples++;
            }
        }
        uint32_t s = xorshift(&rng) % HEAP_SLOTS;
        if (slot[s] != NULL) {
            const uint8_t *b = slot[s];
            for (uint32_t k = 0; k < len[s]; k += 61) {
                if (b[k] != (uint8_t)(pat[s] + k)) {
                    j->bad++;
                    break;
                }
            }
            if (hammer) {
                atomic_store(&g_heap_in_call, 1);
            }
            wl_mem_free(slot[s]);
            if (hammer) {
                atomic_store(&g_heap_in_call, 0);
            }
            slot[s] = NULL;
        }
        if ((xorshift(&rng) & 3u) != 0) {
            uint32_t n = 1u + xorshift(&rng) % 3000u;
            if (hammer) {
                atomic_store(&g_heap_in_call, 1);
            }
            uint8_t *p = WL_MEM_ALLOC(WL_TAG_IO, n);
            if (hammer) {
                atomic_store(&g_heap_in_call, 0);
            }
            if (p != NULL) {
                pat[s] = (uint8_t)xorshift(&rng);
                for (uint32_t k = 0; k < n; k += 61) {
                    p[k] = (uint8_t)(pat[s] + k);
                }
                slot[s] = p;
                len[s] = n;
            } else {
                j->bad++;
            }
        }
        j->ops++;
    }
    for (uint32_t s = 0; s < HEAP_SLOTS; s++) {
        wl_mem_free(slot[s]);
    }
    return NULL;
}

static void check_heap_contention(void) {
    uint32_t live0 = wl_mem_live_blocks(WL_TAG_IO);
    uint32_t badfree0 = wl_mem_bad_frees();
    atomic_store(&g_heap_done, false);
    atomic_store(&g_heap_in_call, 0);
    // The hammer runs below main so the sleepers' timer wake-ups preempt it mid-call; the
    // sleepers cover the other priorities, each pushing allocations through the same lock.
    static HeapJob jobs[4];
    memset(jobs, 0, sizeof jobs);
    jobs[0] = (HeapJob){.seed = 0x1234567u, .sleep_every = 0, .iters = HEAP_LOW_ITERS};
    jobs[1] = (HeapJob){.seed = 0x9e3779b9u, .sleep_every = 3};
    jobs[2] = (HeapJob){.seed = 0x7f4a7c15u, .sleep_every = 2};
    jobs[3] = (HeapJob){.seed = 0x2545f491u, .sleep_every = 5};
    const WlThreadPriority prio[4] = {WL_THREAD_PRIO_IO, WL_THREAD_PRIO_MAIN, WL_THREAD_PRIO_NET,
                                      WL_THREAD_PRIO_AUDIO};
    Worker w[4];
    bool ok = true;
    for (int i = 0; ok && i < 4; i++) {
        ok = spawn(&w[i], heap_worker, &jobs[i], prio[i]);
    }
    (void)reap(&w[0]); // main blocks here; the sleepers run alongside the hammer
    atomic_store(&g_heap_done, true);
    for (int i = 1; i < 4; i++) {
        (void)reap(&w[i]);
    }
    uint32_t bad = 0;
    uint32_t ops = 0;
    uint32_t preempts = 0;
    for (int i = 0; i < 4; i++) {
        bad += jobs[i].bad;
        ops += jobs[i].ops;
        preempts += jobs[i].preempt_samples;
    }
    char d[128];
    snprintf(d, sizeof d, "ops=%u wakes_during_alloc=%u", ops, preempts);
    wlt_report("INFO heap_contention %s", d);
    snprintf(d, sizeof d, "%u ops over 4 threads, %u corrupt", ops, bad);
    wlt_check("heap_contention_data_intact", ok && bad == 0 && jobs[0].ops == HEAP_LOW_ITERS, d);
    wlt_check("heap_contention_structure", wl_mem_check_all(), "heaps consistent");
    wlt_check("heap_contention_no_leaks",
              wl_mem_live_blocks(WL_TAG_IO) == live0 && wl_mem_bad_frees() == badfree0,
              "all blocks freed, no bad frees");
}

// ---------------------------------------------------------------------------------------------
// Waits with deadlines, recursive mutexes, condition variables, detach/exit/ids/priorities
// ---------------------------------------------------------------------------------------------

typedef struct {
    WlPlatformMutex m;
    WlPlatformCond c;
    uint32_t value;  // guarded by m
    uint32_t woken;  // guarded by m
    uint32_t target; // consumers stop once value reaches it
} CondBox;

static void *cond_consumer(void *arg) {
    CondBox *b = arg;
    wl_platform_mutex_lock(b->m);
    while (b->value < b->target) {
        wl_platform_cond_wait(b->c, b->m);
    }
    b->woken++;
    wl_platform_mutex_unlock(b->m);
    return NULL;
}

static void *cond_counter(void *arg) {
    CondBox *b = arg;
    for (uint32_t i = 0; i < 1000u; i++) {
        wl_platform_mutex_lock(b->m);
        b->value++;
        wl_platform_cond_broadcast(b->c);
        wl_platform_mutex_unlock(b->m);
    }
    return NULL;
}

static void *exit_early(void *arg) {
    if (arg != NULL) {
        wl_platform_thread_exit((void *)(uintptr_t)0x5EED);
    }
    return (void *)(uintptr_t)0xBAD;
}

static atomic_uint g_detached_ran;

static void *detached_worker(void *arg) {
    uint32_t *id = arg;
    *id = wl_platform_thread_self_id();
    atomic_fetch_add(&g_detached_ran, 1u);
    return NULL;
}

static void check_waits(void) {
    // a semaphore wait with a deadline: the deadline passes, then a count arrives in time
    WlPlatformSemaphore sem;
    wlt_check("sem_create_t", wl_platform_sem_create(&sem, 0, 4) == WL_OK, "create");
    uint64_t t0 = wl_platform_time_ms();
    bool got = wl_platform_sem_wait_timeout(sem, 30);
    uint64_t waited = wl_platform_time_ms() - t0;
    wlt_check("sem_timeout_expires", !got && waited >= 25 && waited < 2000, "a wait with nothing to take times out");
    wlt_check("sem_try_empty", !wl_platform_sem_wait_timeout(sem, 0), "a try on an empty semaphore fails at once");
    wl_platform_sem_post(sem);
    wlt_check("sem_try_ready", wl_platform_sem_wait_timeout(sem, 0), "a try on a posted semaphore takes it");
    wl_platform_sem_post(sem);
    wlt_check("sem_timeout_ready", wl_platform_sem_wait_timeout(sem, 1000), "a posted count is taken before the deadline");
    wl_platform_sem_destroy(sem);

    // a recursive mutex: the holder locks it again; another thread's try fails until it is free
    WlPlatformMutex rm;
    wlt_check("mutex_recursive_create", wl_platform_mutex_create_recursive(&rm) == WL_OK, "create");
    wl_platform_mutex_lock(rm);
    wlt_check("mutex_recursive_relock", wl_platform_mutex_try_lock(rm), "the holder takes it again");
    wl_platform_mutex_lock(rm);
    wl_platform_mutex_unlock(rm);
    wl_platform_mutex_unlock(rm);
    wl_platform_mutex_unlock(rm);
    wlt_check("mutex_recursive_free", wl_platform_mutex_try_lock(rm), "free again after as many unlocks");
    wl_platform_mutex_unlock(rm);
    wl_platform_mutex_destroy(rm);
    WlPlatformMutex pm;
    wlt_check("mutex_plain_create", wl_platform_mutex_create(&pm) == WL_OK, "create");
    wlt_check("mutex_try_free", wl_platform_mutex_try_lock(pm), "a free mutex is taken by a try");
    wl_platform_mutex_unlock(pm);
    wl_platform_mutex_destroy(pm);

    // a condition variable: timed wait expires; a broadcast wakes every consumer
    CondBox box = {0};
    bool ok = wl_platform_mutex_create(&box.m) == WL_OK && wl_platform_cond_create(&box.c) == WL_OK;
    wlt_check("cond_create", ok, "mutex + condition");
    if (!ok) {
        return;
    }
    wl_platform_mutex_lock(box.m);
    t0 = wl_platform_time_ms();
    bool woke = wl_platform_cond_wait_timeout(box.c, box.m, 30);
    waited = wl_platform_time_ms() - t0;
    wl_platform_mutex_unlock(box.m);
    wlt_check("cond_timeout_expires", !woke && waited >= 25 && waited < 2000, "a timed wait with no signal times out");
    wl_platform_mutex_lock(box.m);
    wlt_check("cond_timeout_zero", !wl_platform_cond_wait_timeout(box.c, box.m, 0), "a zero wait returns at once");
    wl_platform_mutex_unlock(box.m);

    box.target = 1000;
    Worker consumers[4] = {0};
    Worker counter = {0};
    bool spawned = true;
    for (uint32_t i = 0; i < 4; i++) {
        spawned &= spawn(&consumers[i], cond_consumer, &box, WL_THREAD_PRIO_MAIN);
    }
    spawned &= spawn(&counter, cond_counter, &box, WL_THREAD_PRIO_IO);
    wlt_check("cond_spawn", spawned, "consumers and counter");
    reap(&counter);
    for (uint32_t i = 0; i < 4; i++) {
        reap(&consumers[i]);
    }
    wlt_check("cond_broadcast", box.value == 1000 && box.woken == 4, "every consumer saw the count reach its target");
    wl_platform_cond_destroy(box.c);
    wl_platform_mutex_destroy(box.m);

    // exit: a thread ends early with a result its join sees
    Worker ex = {0};
    if (spawn(&ex, exit_early, &ex, WL_THREAD_PRIO_MAIN)) {
        void *r = reap(&ex);
        wlt_check("thread_exit_result", r == (void *)(uintptr_t)0x5EED, "the join sees the exit's result");
    } else {
        wlt_check("thread_exit_spawn", false, "spawn");
    }

    // ids: the caller's is stable and not 0; detached threads run, end and give their slots back
    uint32_t me = wl_platform_thread_self_id();
    wlt_check("thread_self_id", me != 0 && me == wl_platform_thread_self_id(), "stable, not 0");
    enum { DETACHED = 6 };
    static uint8_t stacks[DETACHED][STACK_SIZE] __attribute__((aligned(32)));
    static uint32_t ids[DETACHED];
    atomic_store(&g_detached_ran, 0);
    bool detached_ok = true;
    for (uint32_t round = 0; round < 3 && detached_ok; round++) {
        uint32_t before = atomic_load(&g_detached_ran);
        for (uint32_t i = 0; i < DETACHED; i++) {
            WlPlatformThread t;
            ids[i] = 0;
            detached_ok &= wl_platform_thread_create(&t, detached_worker, &ids[i], stacks[i], STACK_SIZE,
                                                     WL_THREAD_PRIO_MAIN) == WL_OK;
            detached_ok &= wl_platform_thread_detach(t) == WL_OK;
        }
        for (uint32_t spin = 0; spin < 2000 && atomic_load(&g_detached_ran) < before + DETACHED; spin++) {
            wl_platform_thread_sleep_ms(1);
        }
        detached_ok &= atomic_load(&g_detached_ran) == before + DETACHED;
        for (uint32_t i = 0; i < DETACHED; i++) {
            detached_ok &= ids[i] != 0 && ids[i] != me;
        }
        wl_platform_thread_sleep_ms(5); // let them finish their returns before their stacks are reused
    }
    wlt_check("thread_detach", detached_ok, "three rounds of detached threads ran and were reclaimed");

    // priority change of a live thread
    Worker pw = {0};
    box.target = 0;
    if (spawn(&pw, return_arg, NULL, WL_THREAD_PRIO_IO)) {
        WlResult pr = wl_platform_thread_set_priority(pw.th, WL_THREAD_PRIO_NET);
        reap(&pw);
        wlt_check("thread_set_priority", pr == WL_OK || pr == WL_ERR_STATE, "accepted (or already ended)");
    }

    uint64_t wall = wl_platform_wall_time_us();
    wlt_check("wall_clock", wall > 1262304000000000ull && wl_platform_wall_time_us() >= wall,
              "after 2010 and not stepping back");
}

void wlt_suite_threads(void);

void wlt_suite_threads(void) {
    if (!wl_mem_ready()) {
        wlt_check("mem_init", wl_mem_init(NULL) == WL_OK, "wl_mem_init");
    }
    wlt_report("INFO priorities_enforced=%d", wl_platform_thread_priorities_real());
    check_api();
    check_ring_single();
    check_ring_threads();
    check_ping_pong();
    check_mutex();
    check_priorities();
    check_sched_yield();
    check_run_on_stack();
    check_io();
    check_heap_contention();
    check_waits();
}
