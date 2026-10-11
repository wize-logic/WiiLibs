// SPDX-License-Identifier: MIT
// A sampling profiler: a periodic interrupt records where the program was (the program counter,
// the link register and the return addresses up the stack of whichever thread it interrupted)
// into a fixed ring of samples. Nothing is allocated, and the interrupt does a few dozen loads
// and stores. Afterwards the samples are dumped as compact text lines to a file or to the log,
// and tools/wl_profile.py symbolizes them against the program's ELF: the functions that took the
// time, and who called them.
//
//   static uint32_t storage[64 * 1024];                // 4 + depth words a sample
//   WlProfileRing ring;
//   wl_profile_ring_init(&ring, storage, 64 * 1024, 10, WL_PROFILE_STOP_WHEN_FULL);
//   wl_profile_start(&ring, 1000);                     // 1000 samples a second, from here ...
//   ... the work to profile ...
//   wl_profile_stop();                                 // ... to here
//   wl_profile_dump_file(&ring, "sd:/profile.txt");    // and/or wl_profile_dump_log(&ring)
//
// The ring (init, record, get, dump) is portable and tested on every host, with a synthetic
// stack. The sampler (start, stop) exists on the Wii only: the host's returns
// WL_ERR_UNSUPPORTED.
#pragma once

#include "wl/base/base.h"
#include "wl/platform/backtrace.h"

// Callers recorded per sample: at most this many.
#define WL_PROFILE_MAX_DEPTH 16u
// Words in a sample before its callers: the time base's low word, the thread, pc and lr.
#define WL_PROFILE_HEADER_WORDS 4u

typedef enum WlProfileMode {
    WL_PROFILE_STOP_WHEN_FULL = 0, // a full ring keeps its first samples; later ones are counted and dropped
    WL_PROFILE_OVERWRITE = 1       // a full ring keeps the latest samples; the oldest are replaced
} WlProfileMode;

// The ring. One writer (the sampling interrupt, or a test) and, once the writer has stopped, any
// reader. The fields are read-only to users.
typedef struct WlProfileRing {
    uint32_t *words;                // the caller's storage
    uint32_t capacity;              // samples that fit
    uint32_t depth;                 // callers recorded per sample
    uint32_t stride;                // words per sample: WL_PROFILE_HEADER_WORDS + depth
    uint32_t mode;                  // WlProfileMode
    volatile uint32_t head;         // the slot the next sample goes to
    volatile uint32_t count;        // samples held (at most capacity)
    volatile uint32_t total;        // samples taken, including replaced ones
    volatile uint32_t dropped;      // samples lost to a full ring (STOP_WHEN_FULL)
} WlProfileRing;

// What a sample holds, word by word:
//   [0] tick    the time base's low 32 bits when it was taken (60.75 MHz on the Wii)
//   [1] thread  the interrupted thread (an opaque identifier; the address of its control block)
//   [2] pc      the interrupted instruction
//   [3] lr      the interrupted link register
//   [4..]       the return addresses up the stack from the interrupted frame, `depth` of them,
//               0 past the last
// Of lr and the stack walk: a function that has not called anything yet keeps its caller in lr,
// one that has keeps its own frame's return address in the stack; the symbolizer takes lr as a
// caller only when it lies outside the function pc is in.

// Storage of `words` 32-bit words (capacity = words / (4 + depth)). WL_ERR_ARG for a depth over
// WL_PROFILE_MAX_DEPTH or a storage too small for one sample.
WlResult wl_profile_ring_init(WlProfileRing *ring, uint32_t *storage, uint32_t words, uint32_t depth,
                              WlProfileMode mode);
// Empties the ring (the storage keeps what it had). Not while the sampler is running.
void wl_profile_ring_reset(WlProfileRing *ring);

// Records a sample, walking the stack from `sp` through the platform's reader (wl_platform_
// backtrace_from: the Wii's MEM1 and MEM2, the big-endian test build's own stack, nothing
// elsewhere); `_with` takes the reader (tests give a synthetic stack). False when the sample was
// dropped. Does not allocate, lock or log.
bool wl_profile_ring_record(WlProfileRing *ring, uint32_t tick, uint32_t thread, uint32_t pc, uint32_t lr,
                            uint32_t sp);
bool wl_profile_ring_record_with(WlProfileRing *ring, uint32_t tick, uint32_t thread, uint32_t pc, uint32_t lr,
                                 uint32_t sp, WlBacktraceRead read, void *user);

// Sample `index` counted from the oldest held, or NULL past the end: stride words, laid out above.
const uint32_t *wl_profile_ring_get(const WlProfileRing *ring, uint32_t index);

// The next sampling period for one nominally `base` ticks long, varied by up to +-1/16 so that a
// sampler cannot lock to a program that repeats at the same rate (a 30 Hz frame against a 1 kHz
// sampler). `*state` is the generator's, any value; the period is never 0.
uint32_t wl_profile_jitter(uint32_t *state, uint32_t base);

// --- text dumps -------------------------------------------------------------------------------
// One header line, then a line per sample, oldest first, all hexadecimal:
//   WLPROF 1 hz=1000 tbhz=60750000 depth=10 samples=4000 total=4000 dropped=0 thread=80123456 note=...
//   WLPS <tick> <thread> <pc> <lr> <caller> <caller> ...      (callers up to the last non-zero)
// Every line stays under 200 characters (SYS_Report's limit is 256). The Python tool reads these
// lines anywhere in a text (a log with other lines among them).
// `note` is free text for the header (no line breaks), e.g. what was being profiled.
typedef struct WlProfileDumpInfo {
    uint32_t hz;           // the sampling rate (0 if unknown)
    uint32_t timebase_hz;  // ticks a second (0: the platform's)
    uint32_t main_thread;  // the thread that started the profile (0 if unknown)
    const char *note;      // or NULL
} WlProfileDumpInfo;

typedef void (*WlProfileSink)(void *user, const char *line); // a line without its line break

// Sends the dump through `sink`. Returns the lines sent.
uint32_t wl_profile_dump(const WlProfileRing *ring, const WlProfileDumpInfo *info, WlProfileSink sink, void *user);
// The dump to a text file (stdio): WL_ERR_IO when it cannot be opened or written.
WlResult wl_profile_dump_file(const WlProfileRing *ring, const WlProfileDumpInfo *info, const char *path);
// The dump to the log (the Wii: SYS_Report, Dolphin's OSREPORT, straight, not through the log
// ring or the SD log file; the host: standard output). Returns the lines sent.
uint32_t wl_profile_dump_log(const WlProfileRing *ring, const WlProfileDumpInfo *info);

// --- the sampler (Wii) ------------------------------------------------------------------------
// Samples into `ring` `hz` times a second (1 to 10000; the period varies a little, wl_profile_jitter)
// until wl_profile_stop. The interrupt is the decrementer's (libogc's tick tasks), so it
// interrupts any thread, and the sample is the interrupted thread's context. One profile at a
// time: WL_ERR_STATE when one is running, WL_ERR_UNSUPPORTED off the Wii.
WlResult wl_profile_start(WlProfileRing *ring, uint32_t hz);
// Stops the sampler. When it returns the interrupt has finished with the ring. Harmless when
// nothing runs.
void wl_profile_stop(void);
bool wl_profile_running(void);
// The sampler's rate and time base, for a dump's header.
uint32_t wl_profile_timebase_hz(void);
