// SPDX-License-Identifier: MIT
// The GPU hang report (platform.h, wl_gx_hang_where). A periodic alarm (the decrementer's
// interrupt, once a second) reads the command processor's FIFO read pointer: when it has not
// moved for HANG_SECONDS while the GPU has work (commands in the FIFO, or a draw-sync token not
// reached), the alarm takes a snapshot of the command processor's and the processor interface's
// FIFO registers, the bytes at the read pointer and the ring of the last GX events, and wakes the
// report thread, which sleeps on its queue until then. The thread logs the snapshot, syncs the log
// to the SD card, and leaves to the loader LEAVE_SECONDS later. Until it fires it costs one
// interrupt a second and a few stores an event: no thread polls.
//
// The events that put commands in the FIFO (primitives, quads, copies, flushes, tokens, frames' ends)
// keep the FIFO's write pointer as they began, so the report finds the event the GPU stopped after:
// with a full FIFO the commands it stopped in are a megabyte behind the newest, which the ring's
// last events do not reach. It logs the events around it and the layout and textures it drew with.
//
// The interrupt is the only place that sees a hang whose GX thread is suspended for good (libogc
// suspends it at the FIFO's high-water mark, and a hung GPU never lowers it): nothing else runs
// then to notice.
#include <gccore.h>
#include <ogc/gx.h>
#include <ogc/lwp.h>
#include <ogc/system.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wl/base/base.h"
#include "wl/platform/platform.h"
#include "wl/platform/wii/gx_wii_internal.h"

#define TAG "GXHANG"
#define HANG_SECONDS 10u
#define LEAVE_SECONDS 20u
// (the ring reaches back a full FIFO: a megabyte of commands, at worst about 64 bytes an event
// (a texture's or an array's registers); a30's bench averaged 650, the game's frames less)
#define RING 16384u
#define LAST 16u
#define AROUND_BEFORE 24u
#define AROUND_AFTER 6u
#define PEEK_BYTES 64u
#define WHERE_SLOTS 4u
#define STACK_BYTES 16384u

// the command processor's registers (16 bits each; a 32-bit value low half first) and the
// processor interface's FIFO registers
#define CP_REG(off) (*(volatile uint16_t *)(0xCC000000u + (off)))
#define CP_REG32(off) ((uint32_t)CP_REG((off) + 2u) << 16 | CP_REG(off))
#define PI_REG(off) (*(volatile uint32_t *)(0xCC003000u + (off)))
#define CP_STATUS 0x00u
#define CP_CONTROL 0x02u
#define CP_BASE 0x20u
#define CP_END 0x24u
#define CP_HIGH 0x28u
#define CP_LOW 0x2Cu
#define CP_DISTANCE 0x30u
#define CP_WRITE 0x34u
#define CP_READ 0x38u
#define CP_BREAK 0x3Cu
#define PI_BASE 0x0Cu
#define PI_END 0x10u
#define PI_WRITE 0x14u

typedef struct {
    uint8_t kind;
    uint32_t a, b;
    // the PI's FIFO write pointer as the event began (its address; 0: an event that sends no commands)
    uint32_t write;
} Event;

static struct {
    bool up;
    syswd_t alarm;
    lwpq_t queue;
    lwp_t thread;
    // the alarm's watch: the read pointer last seen, and for how many seconds it has not moved
    // while the GPU had work
    uint32_t last_read, stuck;
    volatile bool fired;
    // the events, the newest at head - 1
    Event ring[RING];
    uint32_t head;
    // what the program says it is at
    const char *where_name[WHERE_SLOTS];
    uint32_t where_value[WHERE_SLOTS];
    // the snapshot the alarm takes
    struct {
        uint16_t status, control, sync;
        uint32_t base, end, high, low, distance, write, read, brk;
        uint32_t pi_base, pi_end, pi_write;
        uint32_t token, frames;
        bool in_frame;
        uint8_t before[PEEK_BYTES], at[PEEK_BYTES];
        bool peeked;
        uint32_t where_value[WHERE_SLOTS];
    } snap;
    uint8_t stack[STACK_BYTES] __attribute__((aligned(32)));
} h;

void wl_gx_wii_hang_event(char kind, uint32_t a, uint32_t b) {
    Event *e;

    if (h.fired) {
        return; // (the ring is the report's now)
    }
    e = &h.ring[h.head % RING];

    e->kind = (uint8_t)kind;
    e->a = a;
    e->b = b;
    e->write = kind == 'P' || kind == 'Q' || kind == 'C' || kind == 'X' || kind == 'S' || kind == 'E'
                   ? PI_REG(PI_WRITE) & 0x03FFFFFFu
                   : 0u;
    h.head++;
}

void wl_gx_hang_where(unsigned slot, const char *name, uint32_t value) {
    if (slot < WHERE_SLOTS) {
        h.where_name[slot] = name;
        h.where_value[slot] = value;
    }
}

// uncached bytes of the FIFO at a physical address (none outside MEM1 and MEM2)
static bool peek(uint32_t phys, uint8_t *out) {
    uint32_t i;

    if (!(phys + PEEK_BYTES <= 0x01800000u || (phys >= 0x10000000u && phys + PEEK_BYTES <= 0x14000000u))) {
        return false;
    }
    for (i = 0; i < PEEK_BYTES; i++) {
        out[i] = *(volatile uint8_t *)(0xC0000000u | (phys + i));
    }
    return true;
}

static void snapshot(void) {
    uint32_t start;

    h.snap.status = CP_REG(CP_STATUS);
    h.snap.control = CP_REG(CP_CONTROL);
    h.snap.sync = GX_GetDrawSync();
    h.snap.base = CP_REG32(CP_BASE);
    h.snap.end = CP_REG32(CP_END);
    h.snap.high = CP_REG32(CP_HIGH);
    h.snap.low = CP_REG32(CP_LOW);
    h.snap.distance = CP_REG32(CP_DISTANCE);
    h.snap.write = CP_REG32(CP_WRITE);
    h.snap.read = CP_REG32(CP_READ);
    h.snap.brk = CP_REG32(CP_BREAK);
    h.snap.pi_base = PI_REG(PI_BASE);
    h.snap.pi_end = PI_REG(PI_END);
    h.snap.pi_write = PI_REG(PI_WRITE);
    wl_gx_wii_hang_state(&h.snap.token, &h.snap.frames, &h.snap.in_frame);
    // (the bytes before the read pointer are the commands the GPU took last, those at it the next)
    start = h.snap.read >= h.snap.base + PEEK_BYTES ? h.snap.read - PEEK_BYTES : h.snap.base;
    h.snap.peeked = peek(start, h.snap.before) && peek(h.snap.read & ~31u, h.snap.at);
    memcpy(h.snap.where_value, h.where_value, sizeof h.where_value);
}

static void tick(syswd_t alarm, void *user) {
    uint32_t read, token, frames;
    bool in_frame, work;

    (void)alarm;
    (void)user;
    if (h.fired) {
        return;
    }
    read = CP_REG32(CP_READ);
    wl_gx_wii_hang_state(&token, &frames, &in_frame);
    work = CP_REG32(CP_DISTANCE) != 0u || (token != 0u && (uint16_t)(GX_GetDrawSync() - (uint16_t)token) >= 0x8000u);
    h.stuck = read == h.last_read && work ? h.stuck + 1u : 0u;
    h.last_read = read;
    if (h.stuck >= HANG_SECONDS) {
        h.fired = true;
        snapshot();
        LWP_ThreadSignal(h.queue);
    }
}

static const char *event_name(uint8_t kind) {
    switch (kind) {
    case 'F': return "frame begun (frame)";
    case 'E': return "frame ended (frame, token)";
    case 'S': return "token sent (token)";
    case 'L': return "vertex layout (indexed | colours << 16 | lit << 20 | projected << 24, texcoords | generated << 8 | s << 16)";
    case 'A': return "array (address, attribute << 8 | stride)";
    case 'T': return "texture (address, map << 28 | format << 22 | width << 11 | height)";
    case 'U': return "TLUT loaded (address, slot << 16 | entries)";
    case 'P': return "primitive (primitive << 16 | vertices, draws so far)";
    case 'X': return "transform unit flush strip";
    case 'C': return "EFB copy (address, width << 16 | height)";
    case 'Q': return "quad of the targets (kind, width << 16 | height)";
    default: return "?";
    }
}

static void hex_line(const char *what, uint32_t phys, const uint8_t *bytes) {
    char text[PEEK_BYTES * 3 / 2 + 8];
    uint32_t i, n = 0;

    for (i = 0; i < PEEK_BYTES / 2u; i++) {
        n += (uint32_t)snprintf(text + n, sizeof text - n, "%02x%s", bytes[i], (i & 3u) == 3u ? " " : "");
    }
    WL_LOG_E(TAG, "%s %08x: %s", what, (unsigned)phys, text);
    n = 0;
    for (i = PEEK_BYTES / 2u; i < PEEK_BYTES; i++) {
        n += (uint32_t)snprintf(text + n, sizeof text - n, "%02x%s", bytes[i], (i & 3u) == 3u ? " " : "");
    }
    WL_LOG_E(TAG, "%s %08x: %s", what, (unsigned)(phys + PEEK_BYTES / 2u), text);
}

static void log_event(uint32_t i, const char *mark) {
    const Event *e = &h.ring[i % RING];

    WL_LOG_E(TAG, "%s%c %08x %08x @%08x  %s", mark, e->kind, (unsigned)e->a, (unsigned)e->b, (unsigned)e->write,
             event_name(e->kind));
}

// the event the GPU stopped after: walking back from the newest, the commands each event's write
// pointer leaves behind it add up to the FIFO's backlog (write pointer to read pointer) at the
// last event that began before the read pointer
static void stopped_after(void) {
    uint32_t size = (h.snap.pi_end & 0x03FFFFFFu) - (h.snap.pi_base & 0x03FFFFFFu);
    uint32_t write = h.snap.pi_write & 0x03FFFFFFu;
    uint32_t backlog, behind = 0, prev = write, oldest, i, n, stuck = 0;
    bool found = false;

    if (size == 0 || h.head == 0) {
        return;
    }
    backlog = (write + size - h.snap.read) % size;
    oldest = h.head > RING ? h.head - RING : 0;
    for (i = h.head; i != oldest; i--) {
        const Event *e = &h.ring[(i - 1u) % RING];

        if (e->write == 0) {
            continue;
        }
        behind += (prev + size - e->write) % size;
        prev = e->write;
        if (behind >= backlog) {
            stuck = i - 1u;
            found = true;
            break;
        }
    }
    if (!found) {
        uint32_t kept = h.head - oldest;

        WL_LOG_E(TAG, "the GPU stopped %u bytes behind the writer, before the oldest of the %u events kept (%u bytes "
                      "behind)", (unsigned)backlog, (unsigned)kept, (unsigned)behind);
        if (h.head <= RING) {
            WL_LOG_E(TAG, "  why: the ring holds every event since GX began: the GPU stopped in commands sent before "
                          "the first event (wl_gx_init's own)");
        } else if (behind == 0) {
            WL_LOG_E(TAG, "  why: none of the %u events kept sent commands", (unsigned)kept);
        } else {
            WL_LOG_E(TAG, "  why: %u events of %u bytes on average reach %u bytes back; %u bytes need about %u events "
                          "(RING in gx_hang_wii.c is %u)",
                     (unsigned)kept, (unsigned)(behind / kept), (unsigned)behind, (unsigned)backlog,
                     (unsigned)(backlog / (behind / kept != 0 ? behind / kept : 1u) + 1u), (unsigned)RING);
        }
        return;
    }
    WL_LOG_E(TAG, "the GPU stopped %u bytes behind the writer: in the commands after event %u, %u bytes into them",
             (unsigned)backlog, (unsigned)(h.head - 1u - stuck), (unsigned)(behind - backlog));
    // (what it drew with: the layout and each map's texture last sent before it)
    for (i = stuck + 1u; i != oldest; i--) {
        if (h.ring[(i - 1u) % RING].kind == 'L') {
            log_event(i - 1u, "  its layout: ");
            break;
        }
    }
    for (n = 0; n < 8u; n++) {
        for (i = stuck + 1u; i != oldest; i--) {
            const Event *e = &h.ring[(i - 1u) % RING];

            if (e->kind == 'T' && (e->b >> 28) == n) {
                log_event(i - 1u, "  its map: ");
                break;
            }
        }
    }
    WL_LOG_E(TAG, "the events around it (the newest event is 0, it is %u):", (unsigned)(h.head - 1u - stuck));
    for (i = stuck >= oldest + AROUND_BEFORE ? stuck - AROUND_BEFORE : oldest;
         i != h.head && i <= stuck + AROUND_AFTER; i++) {
        log_event(i, i == stuck ? "> " : "  ");
    }
}

static void *report(void *arg) {
    uint32_t i, count, start;

    (void)arg;
    LWP_ThreadSleep(h.queue);
    WL_LOG_E(TAG, "the GPU has read no command for %u s with work in its FIFO: a GPU hang", HANG_SECONDS);
    WL_LOG_E(TAG, "frames ended %u, in a frame %s; token sent %u, the GPU's token %u",
             (unsigned)h.snap.frames, h.snap.in_frame ? "yes" : "no", (unsigned)(h.snap.token & 0xFFFFu),
             (unsigned)h.snap.sync);
    for (i = 0; i < WHERE_SLOTS; i++) {
        if (h.where_name[i] != NULL) {
            WL_LOG_E(TAG, "where: %s %u", h.where_name[i], (unsigned)h.snap.where_value[i]);
        }
    }
    WL_LOG_E(TAG, "CP status %04x (high water %u, low water %u, read idle %u, commands idle %u, breakpoint %u) "
                  "control %04x",
             h.snap.status, h.snap.status & 1u, (h.snap.status >> 1) & 1u, (h.snap.status >> 2) & 1u,
             (h.snap.status >> 3) & 1u, (h.snap.status >> 4) & 1u, h.snap.control);
    WL_LOG_E(TAG, "CP FIFO base %08x end %08x high %08x low %08x distance %08x write %08x read %08x break %08x",
             (unsigned)h.snap.base, (unsigned)h.snap.end, (unsigned)h.snap.high, (unsigned)h.snap.low,
             (unsigned)h.snap.distance, (unsigned)h.snap.write, (unsigned)h.snap.read, (unsigned)h.snap.brk);
    WL_LOG_E(TAG, "PI FIFO base %08x end %08x write %08x (wrapped %u)", (unsigned)h.snap.pi_base,
             (unsigned)h.snap.pi_end, (unsigned)(h.snap.pi_write & 0x03FFFFFFu),
             (unsigned)((h.snap.pi_write >> 26) & 1u));
    if (h.snap.peeked) {
        start = h.snap.read >= h.snap.base + PEEK_BYTES ? h.snap.read - PEEK_BYTES : h.snap.base;
        hex_line("before the read pointer", start, h.snap.before);
        hex_line("at the read pointer's 32 bytes", h.snap.read & ~31u, h.snap.at);
    }
    count = h.head < LAST ? h.head : LAST;
    WL_LOG_E(TAG, "the last %u GX events, oldest first:", (unsigned)count);
    for (i = h.head - count; i != h.head; i++) {
        log_event(i, "  ");
    }
    stopped_after();
    wl_platform_log_flush();
    WL_LOG_E(TAG, "leaving to the loader in %u s", LEAVE_SECONDS);
    wl_platform_log_flush();
    sleep(LEAVE_SECONDS);
    WL_LOG_E(TAG, "leaving to the loader");
    GX_AbortFrame(); // (the GPU reset: the exit's own GX work then cannot wait on it)
    wl_platform_exit(WL_EXIT_LOADER);
    return NULL;
}

void wl_gx_wii_hang_init(void) {
    struct timespec period = {1, 0};

    if (h.up) {
        return;
    }
    if (LWP_InitQueue(&h.queue) != 0) {
        WL_LOG_W(TAG, "no queue: no GPU hang report");
        return;
    }
    if (LWP_CreateThread(&h.thread, report, NULL, h.stack, STACK_BYTES, LWP_PRIO_HIGHEST) != 0) {
        WL_LOG_W(TAG, "no thread: no GPU hang report");
        return;
    }
    if (SYS_CreateAlarm(&h.alarm) != 0 || SYS_SetPeriodicAlarm(h.alarm, &period, &period, tick, NULL) != 0) {
        WL_LOG_W(TAG, "no alarm: no GPU hang report");
        return;
    }
    h.up = true;
}
