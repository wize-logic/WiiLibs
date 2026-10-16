// SPDX-License-Identifier: MIT
// A run chosen by the command line as well as by the title folder's files (boot.h, README "Running
// a title"). A Homebrew Channel loader (wiiload) passes arguments after the DOL, so a computer can
// choose each run with no change to the SD card. Each argument is one option, "key=value":
//   play=<setting>       the play harness's setting (play.h), as the first line of play.cfg
//   switch=a,b,c=v       flag files of the title folder, as if they were there: `noxf`, `novcache`,
//                        `deterministic`, `vcache=2048` (the file's text after "="), and any other
//                        a build reads; `profile` and `hwcount` are profile.txt and hwcount.txt
//                        (their defaults)
//   file=<name>:<text>   a file of the title folder with this text, `;` for a line end:
//                        file=profile.txt:after 30;seconds 10;repeat 2
//   net=<spec>           the remote log (wl/platform/net_stream.h): "<ip>:<port>" or "listen:<port>"
//   run=<name>           the name of the run in the remote log's first line
// The files are virtual (platform.h, wl_platform_fs_virtual_set): the code that reads them is
// unchanged. A file `netrun.txt` in the title folder may hold the same options, one a line (`#` for a
// comment), for a console or emulator started without arguments; the command line wins over it.
#pragma once

#include "wl/base/base.h"

typedef struct {
    char play[160];  // "" : none given
    char net[80];
    char name[48];
    uint32_t virtual_files;
} WlXbRunOptions;

// True when `arg` begins with one of the option keys above (the host's main uses it to tell an
// option from the hard disk folder).
bool wl_xb_run_option_is(const char *arg);
// Applies one option. WL_ERR_FORMAT for an unknown key or a bad name or value.
WlResult wl_xb_run_option_apply(const char *title_dir, const char *option, WlXbRunOptions *out);
// The arguments (pass argv + 1), then netrun.txt; what is wrong is logged. The arguments win: an
// option given by both is the argument's.
void wl_xb_run_options_load(const char *title_dir, int arg_count, char *const *args, WlXbRunOptions *out);
