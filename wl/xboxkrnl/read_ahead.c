// SPDX-License-Identifier: MIT
// Read-ahead of one file that a title reads from start to end (a map Halo inflates into a Z: cache
// slot): a host thread reads the next AHEAD_SLOTS x AHEAD_SLOT bytes through a handle of its own
// while the guest works on what it was given, so the card's reads overlap the guest's processing.
//
// One window at a time, for one file: its slots are a ring of consecutive pieces of the file, each
// empty, being read (the thread's, outside the lock) or ready. The guest takes bytes from the ready
// slots (waiting for the one being read), and the slots wholly behind its reads go back to the
// thread. A read outside the window is not served (the caller reads the file itself). The thread
// reads under the card's lock (wl_xb_card_lock), as the guest's transfers outside the guest lock do:
// they take turns, and wl_xb_card_busy sees its reads.
//
// The guest lock is never held while the guest waits here: a wait for a slot being read gives it up
// (wl_xb_block_begin), and a close does not wait for the read in flight (the thread lets go of a
// closed window's handle and buffer itself once its read is over; xk_ahead_settle waits for that).
#include <stdlib.h>
#include <string.h>

#include "wl/xboxkrnl/nt_file.h"
#include "wl/xboxrt/page_census.h"

#define AHEAD_SLOT (128u << 10)
#define AHEAD_SLOTS 4u
#define AHEAD_STACK (32u << 10)

enum { SLOT_EMPTY = 0, SLOT_FILLING, SLOT_READY };

typedef struct {
    uint64_t off;
    uint32_t len;
    uint8_t state;
} AheadSlot;

static struct {
    bool up;                // the thread and its lock exist
    bool failed;            // they could not be made: no read-ahead
    WlPlatformMutex lock;
    WlPlatformCond cond;    // the slots changed, a window was opened or closed
    WlPlatformThread thread;
    uint8_t *stack;
    // the window (under the lock)
    const void *owner;      // the handle it is for; NULL: none
    WlPlatformFile *pf;     // the thread's own handle on the file
    bool map;               // the file is a map (for the page census's counts)
    uint8_t *buf;           // AHEAD_SLOTS x AHEAD_SLOT, 64-byte aligned (straight to the card)
    AheadSlot slot[AHEAD_SLOTS];
    uint32_t head, count;   // the oldest slot of the window and how many are in it
    uint64_t next, end;     // where the next slot starts; the file's size (less after a short read)
    uint32_t gen;           // a window's number: a read finished for a closed one is dropped
    uint32_t seq;           // counts the broadcasts of `cond`: a waiter that let the lock go sees one it missed
    bool busy;              // the thread is reading into a slot (of a window that may be closed since)
} g_ra;

static void ahead_changed_locked(void) {
    g_ra.seq++;
    wl_platform_cond_broadcast(g_ra.cond);
}

static void *ahead_main(void *arg) {
    (void)arg;
    wl_platform_mutex_lock(g_ra.lock);
    for (;;) {
        while (!(g_ra.owner != NULL && g_ra.count < AHEAD_SLOTS && g_ra.next < g_ra.end)) {
            wl_platform_cond_wait(g_ra.cond, g_ra.lock);
        }
        uint32_t i = (g_ra.head + g_ra.count) % AHEAD_SLOTS;
        AheadSlot *s = &g_ra.slot[i];
        s->off = g_ra.next;
        s->len = g_ra.end - g_ra.next < AHEAD_SLOT ? (uint32_t)(g_ra.end - g_ra.next) : AHEAD_SLOT;
        s->state = SLOT_FILLING;
        g_ra.count++;
        g_ra.next += s->len;
        g_ra.busy = true;
        uint32_t gen = g_ra.gen;
        WlPlatformFile *pf = g_ra.pf;
        uint64_t off = s->off;
        uint32_t want = s->len;
        uint8_t *base = g_ra.buf;
        uint8_t *dst = base + (size_t)i * AHEAD_SLOT;
        bool map = g_ra.map;
        wl_platform_mutex_unlock(g_ra.lock);

        uint32_t got = 0;
        wl_xb_card_lock();
        if (wl_platform_fs_read_some_at(pf, off, dst, want, &got) != WL_OK) {
            got = 0;
        }
        wl_xb_card_unlock();
        wl_xb_census_card(WL_XB_CCARD_AHEAD, map, got); // (the page census: a diagnostic)

        wl_platform_mutex_lock(g_ra.lock);
        if (gen != g_ra.gen) {
            // closed meanwhile (ahead_close_locked left them to it): its handle and buffer go
            wl_platform_mutex_unlock(g_ra.lock);
            wl_xb_card_lock();
            wl_platform_fs_close(pf);
            wl_xb_card_unlock();
            free(base);
            wl_platform_mutex_lock(g_ra.lock);
        }
        g_ra.busy = false;
        if (gen == g_ra.gen) {
            s->len = got;
            s->state = SLOT_READY;
            if (got < want) {
                g_ra.end = off + got; // an error or a short file: the window stops here
                g_ra.next = g_ra.end;
            }
        }
        ahead_changed_locked();
    }
    return NULL;
}

static bool ahead_up(void) {
    if (g_ra.up || g_ra.failed) {
        return g_ra.up;
    }
    g_ra.failed = true;
    if (wl_platform_mutex_create(&g_ra.lock) != WL_OK) {
        return false;
    }
    if (wl_platform_cond_create(&g_ra.cond) != WL_OK) {
        wl_platform_mutex_destroy(g_ra.lock);
        return false;
    }
    g_ra.stack = aligned_alloc(64, AHEAD_STACK);
    if (g_ra.stack == NULL) {
        wl_platform_cond_destroy(g_ra.cond);
        wl_platform_mutex_destroy(g_ra.lock);
        return false;
    }
    wl_platform_stack_fill(g_ra.stack, AHEAD_STACK);
    // above the guest's threads: it takes the card again the moment a read completes, then waits
    if (wl_platform_thread_create(&g_ra.thread, ahead_main, NULL, g_ra.stack, AHEAD_STACK, WL_THREAD_PRIO_NET) != WL_OK) {
        free(g_ra.stack);
        g_ra.stack = NULL;
        wl_platform_cond_destroy(g_ra.cond);
        wl_platform_mutex_destroy(g_ra.lock);
        return false;
    }
    g_ra.failed = false;
    g_ra.up = true;
    return true;
}

// Under the lock: the window closed at once. A read in flight (into its buffer, through its handle)
// is the thread's to finish: it then closes the handle and frees the buffer itself.
static void ahead_close_locked(void) {
    bool flying = g_ra.busy;
    g_ra.owner = NULL;
    g_ra.gen++;
    g_ra.count = 0;
    if (g_ra.pf != NULL) {
        if (!flying) {
            wl_platform_fs_close(g_ra.pf);
        }
        g_ra.pf = NULL;
        xk_files_open_add(-1); // (one being closed by the thread still counts as gone)
    }
    if (!flying) {
        free(g_ra.buf);
    }
    g_ra.buf = NULL;
    ahead_changed_locked();
}

// Under the lock (the guest lock held or not): until the thread's read in flight is over.
static void ahead_settle_locked(void) {
    while (g_ra.busy) {
        wl_platform_cond_wait(g_ra.cond, g_ra.lock);
    }
}

// Under the lock, called with the guest lock (a kernel call's): waits for the next change of the
// window, the guest lock given up meanwhile (as a read from the card: the other threads and the
// interrupts go on). The window may be anything after it.
static void ahead_wait_locked(void) {
    uint32_t seen = g_ra.seq;
    wl_platform_mutex_unlock(g_ra.lock);
    WlXbThread *self = wl_xb_block_begin(); // (kept in the deterministic mode: the wait is as before)
    wl_platform_mutex_lock(g_ra.lock);
    while (g_ra.seq == seen) {
        wl_platform_cond_wait(g_ra.cond, g_ra.lock);
    }
    if (self != NULL) {
        wl_platform_mutex_unlock(g_ra.lock); // (the guest lock is never taken under this one)
        wl_xb_block_end(self);
        wl_platform_mutex_lock(g_ra.lock);
    }
}

bool xk_ahead_open(const void *owner, const char *host, uint64_t from, uint64_t size) {
    if (owner == NULL || from >= size || !ahead_up()) {
        return false;
    }
    wl_platform_mutex_lock(g_ra.lock);
    while (g_ra.owner == NULL && g_ra.busy) {
        ahead_wait_locked(); // a closed window's read in flight (its buffer and handle), the guest lock given up
    }
    bool ok = false;
    if (g_ra.owner == NULL) {
        g_ra.buf = aligned_alloc(64, (size_t)AHEAD_SLOTS * AHEAD_SLOT);
        if (g_ra.buf != NULL && wl_platform_fs_open(host, WL_FILE_READ, &g_ra.pf) == WL_OK) {
            xk_files_open_add(1); // (the card's open files are few: it counts with the title's)
            g_ra.owner = owner;
            g_ra.map = strstr(host, ".map") != NULL;
            g_ra.head = 0;
            g_ra.count = 0;
            g_ra.next = from;
            g_ra.end = size;
            ok = true;
            ahead_changed_locked();
        } else {
            free(g_ra.buf);
            g_ra.buf = NULL;
            g_ra.pf = NULL;
        }
    }
    wl_platform_mutex_unlock(g_ra.lock);
    return ok;
}

bool xk_ahead_owned(const void *owner) {
    if (!g_ra.up || owner == NULL) {
        return false;
    }
    wl_platform_mutex_lock(g_ra.lock);
    bool own = g_ra.owner == owner;
    wl_platform_mutex_unlock(g_ra.lock);
    return own;
}

const uint8_t *xk_ahead_peek(const void *owner, uint64_t off, uint32_t *avail) {
    *avail = 0;
    if (!g_ra.up) {
        return NULL;
    }
    const uint8_t *p = NULL;
    wl_platform_mutex_lock(g_ra.lock);
    while (g_ra.owner == owner) {
        if (g_ra.count == 0) {
            if (off == g_ra.next && off < g_ra.end) {
                ahead_wait_locked(); // the thread is about to read it
                continue;
            }
            break;
        }
        // the slots wholly behind `off` go back to the thread (once read), also when `off` is where
        // the window ends: the guest caught the thread up, and waits for its next slot above
        AheadSlot *h = &g_ra.slot[g_ra.head];
        if (h->off + h->len <= off && off <= g_ra.next) {
            if (h->state == SLOT_FILLING) {
                ahead_wait_locked();
                continue;
            }
            h->state = SLOT_EMPTY;
            g_ra.head = (g_ra.head + 1u) % AHEAD_SLOTS;
            g_ra.count--;
            ahead_changed_locked();
            continue;
        }
        if (off < h->off || off >= g_ra.next) {
            break; // outside the window
        }
        const AheadSlot *s = NULL;
        uint32_t i = g_ra.head;
        for (uint32_t k = 0; k < g_ra.count; k++, i = (i + 1u) % AHEAD_SLOTS) {
            if (off >= g_ra.slot[i].off && off < g_ra.slot[i].off + g_ra.slot[i].len) {
                s = &g_ra.slot[i];
                break;
            }
        }
        if (s != NULL && s->state == SLOT_FILLING) {
            ahead_wait_locked();
            continue;
        }
        if (s != NULL && s->state == SLOT_READY) {
            *avail = (uint32_t)(s->off + s->len - off);
            p = g_ra.buf + (size_t)i * AHEAD_SLOT + (off - s->off);
        }
        break;
    }
    wl_platform_mutex_unlock(g_ra.lock);
    return p;
}

void xk_ahead_close(const void *owner) {
    if (!g_ra.up) {
        return;
    }
    wl_platform_mutex_lock(g_ra.lock);
    if (owner == NULL || g_ra.owner == owner) {
        if (g_ra.owner != NULL) {
            ahead_close_locked();
        }
        if (owner == NULL) {
            ahead_settle_locked();
        }
    }
    wl_platform_mutex_unlock(g_ra.lock);
}

void xk_ahead_settle(void) {
    if (!g_ra.up) {
        return;
    }
    wl_platform_mutex_lock(g_ra.lock);
    ahead_settle_locked();
    wl_platform_mutex_unlock(g_ra.lock);
}

size_t xk_ahead_host_bytes(void) {
    size_t bytes = __atomic_load_n(&g_ra.stack, __ATOMIC_RELAXED) != NULL ? AHEAD_STACK : 0u;
    if (__atomic_load_n(&g_ra.buf, __ATOMIC_RELAXED) != NULL) {
        bytes += (size_t)AHEAD_SLOTS * AHEAD_SLOT;
    }
    return bytes;
}
