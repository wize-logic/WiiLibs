// SPDX-License-Identifier: MIT
// A title with no code of its own, for the capture bench's tests (wl/xboxrt/bench.h): the default
// main (wl_xboxrt_main) and the boot, run with WL_XB_PLAY="replay:<capture> <runs>", which needs
// no XBE: guest memory and the GPU as a title has them, then the replays.
#include "wl/xboxrt/boot.h"

void wl_xb_title_setup(WlXbTitleConfig *cfg) {
    cfg->audio_off = true;
}
