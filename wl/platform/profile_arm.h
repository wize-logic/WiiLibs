// SPDX-License-Identifier: MIT
// Arming the sampling profiler (profile.h) from a control file, for a program that cannot be
// started under a profiler by hand: a console program, run by an emulator or from the Homebrew
// Channel, which should profile one stretch of its own run (a level, a menu) and nothing else.
//
// A text file in the program's folder, `profile.txt`, holds lines of "name value" (a "#" starts a
// comment). The program reads it when it starts and, if it is there, launches a watcher thread
// that waits until the program says it is ready, samples for a while, writes `profile<N>.txt`
// beside the control file (and, with `log 1`, the same lines to the log, where tools/wl_profile.py
// reads them from an emulator's output), and repeats. Without the file nothing is allocated or
// started: a normal run costs nothing.
//
//   seconds  20   how long to sample (at least 1)
//   after    10   the program's own measure of how far in it must be (the hooks read it; the
//                 watcher does not know what the unit is)
//   hz       500  samples a second (1 to 10000)
//   depth    12   callers kept for each sample (0 to WL_PROFILE_MAX_DEPTH)
//   skip     1    stretches of the program to let go by first (the hooks count them)
//   repeat   1    profiles to take one after another (a tour; at least 1)
//   log      1    also write the samples to the log
//   note     b30  words for the dump's header (the rest of the line)
//
// What is the program's own is three hooks: `ready` (is it in the state to profile now?), `stop`
// (end the profile early?) and `advanced` (has it moved on from the stretch just profiled, so the
// next profile may wait for its own?). The watcher calls them every `poll_ms` from its thread, so
// they read the program's state the way a debugger would, and must not block.
//
//   static bool ready(void *user, const WlProfileArmSettings *s, uint32_t run, char *note, size_t size) { ... }
//   ...
//   WlProfileArmConfig config;
//   if (wl_profile_arm_config_load(&config, "sd:/apps/game", warn, NULL) == WL_OK) {
//       config.hooks = (WlProfileArmHooks){ready, stop, advanced};
//       wl_profile_arm_launch(&arm, &config);
//   }
//
// The settings' parser, the watcher's step function and the dump naming are portable and tested on
// every host with a fake clock; only the thread (platform.h) and the sampler (wl_profile_start)
// are the platform's.
#pragma once

#include "wl/base/base.h"
#include "wl/platform/platform.h"
#include "wl/platform/profile.h"

#define WL_PROFILE_ARM_NOTE_SIZE 48u
// The control file's name in the folder.
#define WL_PROFILE_ARM_FILE "profile.txt"

typedef struct WlProfileArmSettings {
    uint32_t seconds;
    uint32_t after;
    uint32_t hz;
    uint32_t depth;
    uint32_t skip;
    uint32_t repeat;
    uint32_t log;
    char note[WL_PROFILE_ARM_NOTE_SIZE];
} WlProfileArmSettings;

typedef enum WlProfileArmIssue {
    WL_PROFILE_ARM_UNKNOWN_KEY = 1, // a name the file format does not have (the line is ignored)
    WL_PROFILE_ARM_BAD_VALUE = 2    // a number expected (the setting keeps its value)
} WlProfileArmIssue;

// Called for each line the parser could not use: the issue, the name as written (cut to 31
// characters) and the line's number (from 1).
typedef void (*WlProfileArmIssueFn)(void *user, WlProfileArmIssue issue, const char *key, uint32_t line);

// The defaults (seconds 20, after 10, hz 500, depth 12, skip 1, repeat 1, log 1, no note).
void wl_profile_arm_settings_default(WlProfileArmSettings *settings);
// Keeps the settings inside what the profiler can do: hz 1 to 10000, depth at most
// WL_PROFILE_MAX_DEPTH, seconds and repeat at least 1.
void wl_profile_arm_settings_clamp(WlProfileArmSettings *settings);
// Reads the file's text over the settings in `settings` (so the defaults come first) and clamps
// them. Names end at a blank or "="; a value is decimal digits, then the end of the line, a blank
// or "#". Lines may be indented; a blank line, a "#" line and a line with a name alone are skipped.
// `issue` may be NULL. Returns the number of lines reported.
uint32_t wl_profile_arm_settings_parse(WlProfileArmSettings *settings, const char *text, size_t length,
                                       WlProfileArmIssueFn issue, void *user);
// The same on a file (stdio): defaults, then the file. WL_ERR_NOT_FOUND when it cannot be opened
// (the usual case: the profiler is not armed), WL_ERR_IO when reading fails, WL_ERR_LIMIT past 16 KiB.
WlResult wl_profile_arm_settings_load(WlProfileArmSettings *settings, const char *path, WlProfileArmIssueFn issue,
                                      void *user);

// "<folder>/profile<run + 1>.txt" (run counted from 0). WL_ERR_RANGE when it does not fit.
WlResult wl_profile_arm_dump_path(char *out, size_t size, const char *folder, uint32_t run);
// Samples the ring must hold for the settings: seconds * hz and a tenth over (the jitter), and
// room for a few more; at most `max_bytes` of ring (0: WL_PROFILE_ARM_DEFAULT_RING_BYTES).
// `*clipped` is set when the limit cut it.
uint32_t wl_profile_arm_ring_samples(const WlProfileArmSettings *settings, uint32_t max_bytes, bool *clipped);

#define WL_PROFILE_ARM_DEFAULT_RING_BYTES (4u * 1024u * 1024u)
#define WL_PROFILE_ARM_DEFAULT_POLL_MS 100u
#define WL_PROFILE_ARM_DEFAULT_STACK_SIZE (16u * 1024u)

// --- what is the program's --------------------------------------------------------------------
typedef struct WlProfileArmHooks {
    // Called while waiting to begin. True to begin sampling now. `note` (`size` bytes) takes words
    // for the dump's header after the settings' note (e.g. "map 2 game_time 400"); it starts
    // empty. `run` counts from 0. Required.
    bool (*ready)(void *user, const WlProfileArmSettings *settings, uint32_t run, char *note, size_t size);
    // Called while sampling. True to end this profile before its `seconds` are up. May be NULL.
    bool (*stop)(void *user, const WlProfileArmSettings *settings, uint32_t run);
    // Called after a profile, while more runs remain. True when the program has moved on from the
    // stretch just profiled (so `ready` is asked about the next one). May be NULL: at once.
    bool (*advanced)(void *user, const WlProfileArmSettings *settings, uint32_t run);
} WlProfileArmHooks;

// --- the watcher's state machine --------------------------------------------------------------
typedef enum WlProfileArmPhase {
    WL_PROFILE_ARM_WAITING = 0, // for ready
    WL_PROFILE_ARM_SAMPLING,
    WL_PROFILE_ARM_NEXT,        // for advanced
    WL_PROFILE_ARM_DONE
} WlProfileArmPhase;

typedef enum WlProfileArmAction {
    WL_PROFILE_ARM_IDLE = 0, // nothing to do: sleep a poll
    WL_PROFILE_ARM_START,    // begin sampling now (state.note is the hook's note)
    WL_PROFILE_ARM_STOP,     // stop sampling and write the dump (state.stopped_early says why)
    WL_PROFILE_ARM_FINISH    // all the runs are done
} WlProfileArmAction;

typedef struct WlProfileArmState {
    WlProfileArmPhase phase;
    uint32_t run;            // the run in progress or just finished, from 0
    uint64_t started_us;     // when START was returned
    bool stopped_early;      // the last STOP was the stop hook's, not the time's
    char note[64];           // what ready wrote at START
} WlProfileArmState;

void wl_profile_arm_state_init(WlProfileArmState *state);
// One look at the program, at time `now_us`. START and STOP are for the caller to carry out; the
// state has already moved on. `settings` and `hooks` must stay the same across calls.
WlProfileArmAction wl_profile_arm_step(WlProfileArmState *state, const WlProfileArmSettings *settings,
                                       const WlProfileArmHooks *hooks, void *user, uint64_t now_us);
// Gives up the run START began (the sampler would not start): the state moves on as after a STOP.
void wl_profile_arm_abandon(WlProfileArmState *state, const WlProfileArmSettings *settings);

// --- the watcher ------------------------------------------------------------------------------
// The platform pieces the watcher uses; tests give their own. Any member may be NULL for the
// platform's (wl_platform_time_us, wl_platform_thread_sleep_ms, wl_profile_start, wl_profile_stop).
typedef struct WlProfileArmOps {
    uint64_t (*time_us)(void *user);
    void (*sleep_ms)(void *user, uint32_t ms);
    WlResult (*start)(void *user, WlProfileRing *ring, uint32_t hz);
    void (*stop)(void *user);
    void *user;
} WlProfileArmOps;

typedef struct WlProfileArmConfig {
    WlProfileArmSettings settings;
    WlProfileArmHooks hooks;
    void *hooks_user;            // given to every hook
    char folder[96];             // where profile<N>.txt go (and profile.txt is read from)
    uint32_t poll_ms;            // 0: WL_PROFILE_ARM_DEFAULT_POLL_MS
    uint32_t max_ring_bytes;     // 0: WL_PROFILE_ARM_DEFAULT_RING_BYTES
    uint32_t stack_size;         // of the watcher's thread; 0: WL_PROFILE_ARM_DEFAULT_STACK_SIZE
    WlThreadPriority priority;   // of the watcher's thread: above the program's own, so it keeps looking
    const WlProfileArmOps *ops;  // or NULL
} WlProfileArmConfig;

// Zeroes `config` and fills in the folder, the settings from "<folder>/profile.txt" and the
// priority WL_THREAD_PRIO_NET; the hooks are the caller's to set after. WL_ERR_NOT_FOUND
// when the file is not there: nothing is armed. WL_ERR_RANGE when the folder is too long.
WlResult wl_profile_arm_config_load(WlProfileArmConfig *config, const char *folder, WlProfileArmIssueFn issue,
                                    void *user);

// Watches and profiles on the calling thread until every run is done. WL_ERR_ARG without a ready
// hook. The files and the log lines are the watcher's: "[profile] sampling ...", "<path>: WL_OK",
// "[profile] done".
WlResult wl_profile_arm_run(const WlProfileArmConfig *config);

typedef struct WlProfileArm {
    WlProfileArmConfig config; // (a copy: the caller's may go)
    WlPlatformThread thread;
    void *stack;
    bool launched;
} WlProfileArm;

// Runs wl_profile_arm_run on a thread of its own (the settings are copied; the hooks' user data
// must outlive the thread) and logs how it was armed. WL_ERR_NOMEM without memory for the stack.
WlResult wl_profile_arm_launch(WlProfileArm *arm, const WlProfileArmConfig *config);
// Waits for the watcher to finish and frees its stack. The Wii program that arms the profiler
// need not call it; a test does.
WlResult wl_profile_arm_join(WlProfileArm *arm);
