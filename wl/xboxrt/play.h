// SPDX-License-Identifier: MIT
// The play harness: a scripted player for a recompiled title, so that runs without a person go
// through the menus, into levels and through them, and can be checked from their logs. Modelled
// on OpenCE-Wii's port/linux/src/test_input.c (CC0), whose script language it keeps.
//
// It is started by the boot code with a setting (WlXbTitleConfig.play; on the host the
// WL_XB_PLAY environment variable wins; on the Wii the first line of <title folder>/play.cfg):
//     script:<file>     the steps of a file (relative to the title folder, or absolute)
//     bot:<seed>        controller 1 plays as the bot (walks, turns, looks, fires, jumps, grenades)
//     look:<seed>       controller 1 only turns and looks up and down
// It drives the four controller ports (input.h) once a frame: on a runtime thread of its own
// (wl_xb_thread_create_host, above the title's priorities), woken at each vertical blank, or, when
// the title has a better place, at the title's own frame point (wl_xb_play_frame_point). Either way
// the work runs holding the guest lock, so it reads and writes guest memory and calls guest code
// as a title thread would.
//
// Every line it logs starts with "test_input:" (tag "play"): each step as it begins, with the
// script's clock; what the title says it shows as it changes; the end ("script finished", or
// "script stopped at line N (...): why").
//
// The steps, one to a line ('#' starts a comment; a duration is seconds, or <n>f frames, or <n>t
// the title's ticks; a wait gives up after its timeout, 120 s unless given, which stops the script):
//     wait <duration>                    nothing pressed for that long
//     frames <n> / vblanks <n>           nothing pressed for n frames / vertical blanks
//     press <buttons> [<count>]          pressed and let go, count times (each phase at least
//                                        100 ms, 4 reports to the title and 2 of its frames)
//     hold <buttons> <duration>          held for that long
//     down <buttons> / up <buttons>      held from now on / let go (a latch, for several steps)
//     release                            every latched button let go, the sticks centred
//     stick <lx> <ly> <rx> <ry> <duration>   the sticks, each -1..1
//     trigger <lt> <rt> <duration>       the triggers, each 0..1
//     screen <text> [<timeout>]          until the title's "screen" holds the text (any case)
//     focus <text> [<timeout>]           until its "focus" holds it
//     select <text> [up|down|left|right] [<count>]   the D-pad until "focus" holds the text
//     play [<timeout>]                   until a game runs (the title's "play" predicate, or its
//                                        "tick" >= 0 with no "screen")
//     ticks <n> [<timeout>]              until the title's "tick" has moved on n
//     checkpoint [<timeout>]             until its "checkpoints" goes up
//     until <predicate> [<arg>] [<timeout>]  until a predicate the title registered holds
//     do <action> [<arg>]                an action the title registered ("load levels\a10\a10")
//     load <arg>                         the same as do load <arg>
//     memory <address>[:1|2|4] [&<mask>] <op> <value> [<timeout>]
//                                        until guest memory compares so (op: == != < <= > >=)
//     peek <address>[:1|2|4]             its value, logged
//     bot <duration> [checkpoint]        the bot's play for that long (or until a checkpoint)
//     look <duration>                    the look-only bot for that long
//     <step> [<args>] [<timeout>]        a long step the title registered (OpenCE's
//                                        "objective <seconds>"): until it says done or failed
//     log <text>                         a line in the log, with the title's values
//     screenshot [<name>]                the picture (the host: a PNG through wl/gxsoft; the
//                                        Wii: a one-frame GPU capture), in the output folder
//     capture <frames> [<name>]          the GPU's work for that many frames (xbox_nv2a_replay)
//     player <n> / <n>: <step>           the steps after it / the one step drive controller n
//     connect <n> [real] / disconnect <n>    controller n there (real: as the host has it) / gone
//     bot <n> <seed>|off                 controller n plays as the bot alongside the steps
//     repeat <count> ... end             the steps between, count times
//     quit                               the title's process ends (exit code 0)
// Buttons: a b x y black white lt rt up down left right start back lthumb rthumb, and the sticks
// at full throw lup ldown lleft lright rup rdown rleft rright, joined with '+'.
//
// A title teaches the harness about itself with the registration calls below (a title project's
// plugin source, from wl_xb_title_setup: boot.h); they may be made before the boot.
#pragma once

#include "wl/base/base.h"
#include "wl/xboxrt/pad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XB_PLAY_PORTS 4u

// Starts the harness with a setting (above; NULL or "" does nothing). `base_dir` is where a
// relative script path is looked for (the title folder), `out_dir` where screenshots go (NULL:
// beside the script). Called by the boot code once the service runs (lock held).
WlResult wl_xb_play_start(const char *setting, const char *base_dir, const char *out_dir);
bool wl_xb_play_active(void);

// ---- What a title tells the harness -----------------------------------------------------------

// A text value ("screen": the menu shown; "focus": its focused item): false when none.
typedef bool (*WlXbPlayTextFn)(void *user, char *out, size_t cap);
// A number ("tick": the game's ticks, -1 outside a game; "checkpoints": how many saved since the
// start; "frame": the title's frames, for presses; "menu": nonzero while menus are up;
// "menu_busy": nonzero while the menus take no input): false when it cannot say now.
typedef bool (*WlXbPlayNumberFn)(void *user, int64_t *out);
// A predicate for "until <name> <arg>" (and "play" when a title has its own): `first` is true on
// its first call of a step, `scratch` is the step's to keep between calls.
typedef bool (*WlXbPlayPredicateFn)(void *user, const char *arg, bool first, int64_t *scratch);
// An action for "do <name> <arg>": runs once, holding the guest lock (wl_xb_call_guest is fine).
typedef WlResult (*WlXbPlayActionFn)(void *user, const char *arg);

WlResult wl_xb_play_register_text(const char *name, WlXbPlayTextFn fn, void *user);
WlResult wl_xb_play_register_number(const char *name, WlXbPlayNumberFn fn, void *user);
WlResult wl_xb_play_register_predicate(const char *name, WlXbPlayPredicateFn fn, void *user);
WlResult wl_xb_play_register_action(const char *name, WlXbPlayActionFn fn, void *user);

// A long step the title runs frame by frame ("objective 3600"): it gets the controller and returns
// RUNNING until it is done or has failed; the harness stops it at its timeout (the last word of
// the line when it is a number, else `default_timeout_s`; counted in the title's ticks while a game
// runs, as the bot's time is).
typedef enum { WL_XB_STEP_RUNNING = 0, WL_XB_STEP_DONE = 1, WL_XB_STEP_FAILED = 2 } WlXbStepStatus;
typedef enum { WL_XB_STEP_END_DONE = 0, WL_XB_STEP_END_FAILED, WL_XB_STEP_END_TIMEOUT, WL_XB_STEP_END_STOPPED } WlXbStepEnd;
typedef struct {
    const char *arg;       // the line's words after the step's name (the timeout taken off)
    double timeout_s;
    uint32_t port;         // the controller it drives (0..3)
    uint32_t frames;       // frames since it began
    int64_t ticks;         // the game's time played since it began, in ticks (a revert to a
                           // checkpoint does not put it back; -1: not in a game)
    double seconds;        // the step's time (ticks / tick rate in a game, else frames)
    WlXboxPad *pad;        // this frame's controller (zeroed before each call): the step fills it
    void *state;           // the step's own
    char why[128];         // why it failed (logged)
} WlXbPlayStep;
typedef struct {
    WlResult (*begin)(void *user, WlXbPlayStep *step); // an error stops the script
    WlXbStepStatus (*frame)(void *user, WlXbPlayStep *step);
    void (*end)(void *user, WlXbPlayStep *step, WlXbStepEnd how); // NULL: nothing to undo
    double default_timeout_s;
} WlXbPlayStepType;
WlResult wl_xb_play_register_step(const char *name, const WlXbPlayStepType *type, void *user);

// Called at the start of every frame of the harness, before the steps (lock held): a title reads
// its state here, may set the menu flag (input.h) and may lay its own presses over the ports'.
typedef struct {
    uint32_t frame;        // the harness's frames
    uint32_t vblank;       // the guest's vertical blanks
    WlXboxPad *pads;       // the four ports' script states this frame (after the steps, see below)
} WlXbPlayFrame;
typedef void (*WlXbPlayFrameFn)(void *user, WlXbPlayFrame *frame);
void wl_xb_play_set_frame_hook(WlXbPlayFrameFn fn, void *user);

// The title's own frame point: a title that calls this from its game loop (once per frame, from
// guest code's thread, lock held: Halo does it from a function its main loop calls, through the
// generated code's trace hook) gets its frames run there instead of at each vertical blank, from
// the first call on.
void wl_xb_play_frame_point(void);

// A line in the log with the harness's prefix ("test_input: ...").
void wl_xb_play_log(const char *fmt, ...) WL_PRINTF_FMT(1, 2);

// The harness's clock and state, for a title's steps.
uint32_t wl_xb_play_frames(void);
bool wl_xb_play_number(const char *name, int64_t *out);
bool wl_xb_play_text(const char *name, char *out, size_t cap);

// The bot's controller at time `t_ms` (seeded, deterministic): walking, strafing, turning, firing
// every three seconds, jumping every five, a grenade every seven; `look`: only turning and looking.
void wl_xb_play_bot(WlXboxPad *pad, uint64_t t_ms, int seed, bool look);

// Parses a script into steps without running it (tests): the count, or -1 with the line number in
// `*bad_line`.
int wl_xb_play_parse(const char *text, int *bad_line);

#ifdef __cplusplus
}
#endif
