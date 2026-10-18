// SPDX-License-Identifier: MIT
// Booting a recompiled Xbox title: platform, log, mounts, guest memory, the XBE from the title's
// folder, the kernel (wl/xboxkrnl), devices and the GPU, the service thread, and the entry point on
// a guest thread of its own; then it waits for the title to leave (HalReturnToFirmware, or its last
// thread ending) and returns the code. A title's main fills a WlXbTitleConfig and calls
// wl_xb_boot_run; wl_xboxrt_main is a main that does only that (a host path argument; on the Wii
// sd:/xbox/<title>/default.xbe).
#pragma once

#include "wl/xboxkrnl/xboxkrnl.h"
#include "wl/xboxrt/thunk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title_dir;       // the title's folder (D:): a wl/platform path ("sd:/xbox/halo")
    const char *xbe_name;        // "default.xbe"
    const char *data_dir;        // the hard disk's folder (writable); NULL: <title_dir>/hdd
    const char *log_dir;         // the Wii's log folder on SD; NULL: <title_dir>/logs ("" for none)
    const WlXkMount *mounts;     // more devices (they win over the default ones)
    uint32_t mount_count;
    uint32_t ram_mb;             // guest RAM: 64 (retail) or 128
    bool inline_first_thread;    // see WlXkConfig
    bool async_io_pending;       // see WlXkConfig
    const uint16_t *ordinal_remap; // a title built with another XDK: ordinal -> canonical (0: same)
    uint32_t ordinal_remap_count;
    bool thunk_look_behind;      // resolve a run of thunks just below the declared table
    WlXbManualLookup manual_lookup; // the title's hand-written functions (recomp_lookup_manual)
    bool gpu_stub;               // the idle NV2A even when wl/xbox's model is built in
    bool audio_off;              // the idle APU and AC'97 even when wl/xboxapu's models are built in
    uint32_t frame_pool_bytes;   // the Wii: host memory for guest frames (0: as much as can be had,
                                 // leaving `keep_free_bytes`). The host and ppc-be: 0 lets the OS
                                 // back every page; else a frame limit paged as the Wii pages
                                 // (WL_XB_FRAME_MB or WL_XB_FRAME_KB in the environment win)
    uint32_t keep_free_bytes;    // the Wii: the host side's memory besides the textures' arena,
                                 // 9 MB by default (demand paging covers the guest)
    uint32_t loan_bytes;         // the Wii: of keep_free_bytes, what serves as guest frames while
                                 // the heap does not need it (frame_loan.h: given back when an
                                 // allocation would fail), 5 MB by default; a file "oldreserve" in
                                 // the title's folder: none. The host: WL_XB_LOAN_MB/KB, the top of
                                 // the frame limit, the heap's cap growing as it is given back
    uint32_t texture_bytes;      // the GPU's converted textures: an arena of their own (15 MB by
                                 // default; the Wii takes it from the frames, beside
                                 // keep_free_bytes; the host: WL_XB_TEXTURE_MB wins)
    uint32_t htab_bytes;         // the Wii's page table: 256 KB by default
    const char *swap_path;       // the paging's swap file (NULL: <data_dir>/pagefile.sys; "": none)
    uint32_t swap_bytes;         // its size (0: the guest RAM's)
    bool trace_kernel_calls;     // log every kernel call
    const char *play;            // the play harness's setting (play.h: "script:<file>", "bot:<seed>",
                                 // "look:<seed>"); NULL: none, or on the Wii the first line of
                                 // <title_dir>/play.cfg when there is one
    const char *play_out;        // where its screenshots go (NULL: <title folder>/play)
    int arg_count;               // the run's options from the command line (runopts.h; argv + 1),
    char *const *args;           // which win over the title folder's files and over `play`
    bool deterministic;          // the deterministic mode (thread.h): a virtual clock, so that two runs
                                 // with the same input run alike (the host: WL_XB_DETERMINISTIC=1)
    void (*kernel_config)(WlXkConfig *cfg, void *user); // edits the kernel's settings before init
    void (*before_entry)(void *user); // after the image is loaded and the kernel is up
    void *user;
} WlXbTitleConfig;

void wl_xb_title_config_default(WlXbTitleConfig *cfg);

// wl_xboxrt_main calls this before the boot with the configuration it made: a weak default that
// does nothing, which a title project's own source replaces to change the configuration and to
// register what it tells the play harness (play.h).
void wl_xb_title_setup(WlXbTitleConfig *cfg);

// Runs the title; returns its exit code (HalReturnToFirmware's routine, or 0), or a negative
// WlResult when it could not start (a missing XBE, no memory).
int wl_xb_boot_run(const WlXbTitleConfig *cfg);

#if !defined(__wii__)
// The host: one run at a time on a hard disk's folder. Two runs on one folder write each other's
// files under them: the swap file, z:\savegame.bin, the Z: cache slots. A page the paging dropped
// because the file it was read from still holds its bytes (wl_xb_file_pages) comes back from that
// file, so another run's (or its harness's) write there is read back as this run's memory: a
// crash minutes later, at a point the virtual clock repeats exactly, that looks like a race.
// wl_xb_hdd_lock takes an exclusive lock (flock) on the folder `host_dir`, held until
// wl_xb_hdd_unlock or the process's end, a crash included; *out_fd is its descriptor. WL_ERR_DENIED:
// another process holds it. wl_xboxrt_main takes it and stops with a message when it is refused,
// unless WL_XB_HDD_LOCKED=1: the harness holds it for the run (tools/golden_check.py waits for it
// before its put-back of the hdd's files and passes its descriptor on to the program).
// Another error (no such folder, a file system without locks): nothing is held, *out_fd is -1.
WlResult wl_xb_hdd_lock(const char *host_dir, int *out_fd);
void wl_xb_hdd_unlock(int fd);
#endif

#ifdef __cplusplus
}
#endif
