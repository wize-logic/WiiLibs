// SPDX-License-Identifier: MIT
// The Xbox kernel for recompiled titles: the exports a title imports by ordinal, implemented over
// the guest memory, threads and interrupts of wl/xboxrt and the files of wl/platform. Portable C:
// guest structures are read and written field by field through little-endian accessors.
//
// What it covers: NT dispatcher objects in guest memory (events, semaphores, mutants, timers,
// threads) with waits on one or many, alertable waits and user APCs; critical sections; the
// handle table; files through a mount table (D:, the title's folder; the hard disk's partitions in
// a data folder; the cache partitions with sparse files), overlapped I/O completing at once with
// its event and APC; pools, contiguous and virtual memory; threads; DPCs, timers, interrupts and
// the IRQL; Rtl strings, time and the printf family; the EEPROM settings, AV and HAL queries;
// SHA-1, RC4 and HMAC. The rest of the 371 exports are logged stubs or have no body (the thunk
// dispatcher logs their first call). README.md has the table for Halo's 134 imports.
#pragma once

#include "wl/base/base.h"
#include "wl/xboxrt/xbe.h"

#ifdef __cplusplus
extern "C" {
#endif

// A device (or a part of one) and the platform path it is served from.
typedef struct {
    const char *device;    // an NT device path: "\\Device\\CdRom0", "\\Device\\Harddisk0\\Partition1", ...
    const char *host_path; // a wl/platform path: "sd:/xbox/halo" (no trailing slash)
    bool writable;
    bool sparse;           // files keep a logical size beyond what is written (the cache partitions)
} WlXkMount;

#define WL_XK_MAX_MOUNTS 16u

typedef struct {
    // The title's folder (\Device\CdRom0: D:) and a writable folder for the hard disk; the mounts
    // the kernel makes from them: CdRom0 -> title_dir; Partition1 (E:, TDATA/UDATA) ->
    // data_dir/E; Partition2 (C:) -> data_dir/C; Partitions 3-5 (the cache: X:, Y:, Z:) ->
    // data_dir/X, Y, Z, sparse. `mounts` (may be NULL) are applied after those and win.
    const char *title_dir;
    const char *data_dir;
    const WlXkMount *mounts;
    uint32_t mount_count;
    uint32_t title_id;      // for the default T:/U: links (the title's certificate)
    // EEPROM / settings the title reads (ExQueryNonVolatileSetting, XboxHardwareInfo, AV queries)
    uint32_t language;      // XC_LANGUAGE: 1 English
    uint32_t video_flags;   // XC_VIDEO_FLAGS: 0 (no widescreen, no 480p)
    uint32_t audio_flags;   // XC_AUDIO_FLAGS: 0 (stereo)
    uint32_t game_region;   // XC_FACTORY_GAME_REGION: 1 North America
    uint32_t av_region;     // XC_FACTORY_AV_REGION: 0x00400100 (NTSC-M, 60 Hz)
    uint32_t dvd_region;    // XC_DVD_REGION: 1
    uint32_t av_pack;       // the AV cable: 1 composite (AV_PACK_STANDARD), 4 HDTV component
    uint32_t parental_games; // XC_PARENTAL_CONTROL_GAMES: 0 (none)
    int32_t timezone_bias;  // minutes west of UTC: 0
    // Behaviour knobs
    bool async_io_pending;  // overlapped reads/writes return STATUS_PENDING (completing at once)
    bool gpu_instance_end;  // MmClaimGpuInstanceMemory returns the region's end (true) or start
    bool inline_first_thread; // the first PsCreateSystemThreadEx runs on the calling thread
                              // (xboxrecomp's INLINE model); else every thread is real
    bool trace_calls;       // log every kernel call; first calls are logged at debug level anyway
    bool no_read_ahead;     // no host thread reads ahead of a file read from start to end (a map
                            // being inflated): each read waits for the card
} WlXkConfig;

void wl_xk_config_default(WlXkConfig *cfg);

// Brings the kernel up: the data exports' page, the object and handle tables, the mounts and
// default links, the interrupt/DPC/timer handler on the runtime's service, KeTickCount. Needs
// wl_xb_mem_init and wl_xb_threads_init (the calling thread holds the guest lock).
WlResult wl_xk_init(const WlXkConfig *cfg);

// How an ordinal resolves: wl_xbe_resolve_thunks's resolver (`user` unused).
bool wl_xk_resolve(void *user, uint32_t ordinal, WlXbImport *out);
// The export's name ("Unknown" for none), its stdcall argument bytes, whether it has a body.
const char *wl_xk_export_name(uint32_t ordinal);
bool wl_xk_export_info(uint32_t ordinal, uint32_t *arg_bytes, bool *data, bool *implemented);

// Gives the boot thread its kernel side (a thread object, its KTHREAD signalled when it ends).
WlResult wl_xk_adopt_thread(void);

// What the title asked of the display (AvSetDisplayMode): the presenter's hook, may be NULL.
typedef void (*WlXkDisplayHook)(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t framebuffer_phys);
void wl_xk_set_display_hook(WlXkDisplayHook fn);

typedef struct {
    uint32_t handles, objects, files_open;
    uint32_t waits, wait_timeouts, apcs_delivered, dpcs_run, isrs_run, timers_fired;
    uint32_t pool_bytes;
} WlXkStats;
void wl_xk_stats(WlXkStats *out);
// The allocation census (diagnostics; WL_XB_ALLOCS=<KB> arms it on the host at the kernel's start):
// every contiguous, system or virtual allocation of at least `min_bytes` (0: off) is logged
// ("alloc ...") with its guest callers and kept until freed. The report logs each one kept ("allocs:
// ..."): KB asked, with a frame now, and of those not all zero, and a map of where. Lock held.
void wl_xk_alloc_census(uint32_t min_bytes);
void wl_xk_alloc_census_report(void);
// A host file the kernel may have open was changed by someone else (the play harness's checkpoint
// snapshots): guest pages filled from its bytes forget them (mem.h, wl_xb_file_pages). Before the
// change; `host` as the kernel names it (the data folder's path, then the file's).
void wl_xk_file_changed(const char *host);
// What a file handle's reads and writes moved (NtReadFile/NtWriteFile that succeeded): the log shows
// it every 64 MB and at close ("file I/O"), with the time the card took and how much of it kept the
// guest lock (every other thread and the interrupts waited). False for a handle that is
// not a file.
typedef struct {
    uint32_t reads, writes;
    uint32_t jumps; // requests that did not start where the one before ended
    uint64_t read_bytes, write_bytes;
    uint64_t ahead_bytes; // of the read bytes, those the read-ahead had ready
    uint64_t held_us;     // the card's time (reads, writes, flushes) with the guest lock kept
    uint64_t held_max_us; // its longest piece
} WlXkFileIo;
bool wl_xk_file_io(uint32_t handle, WlXkFileIo *out);
// Flushes every open file written since its last flush (its size and time reach the card's
// directory entry). For the title's exit, with no guest thread running: a title keeps files open
// for good, and leaving does not close them. A read-ahead in flight ends first.
void wl_xk_files_flush(void);
// The title has left (true): no file transfer reaches the card from now on. A guest thread's read or
// write outside the guest lock stops at its next piece (failed), and the read-ahead ends: the card
// can be flushed and unmounted under it. Before wl_xk_files_flush at the exit; false undoes it.
void wl_xk_files_stop(bool stop);

#ifdef __cplusplus
}
#endif
