// SPDX-License-Identifier: MIT
// The wrist strap reminder's state machine: see strap_screen.h.
#include "wl/platform/strap_screen.h"

#include <string.h>

#define DEFAULT_IGNORE_US 1000000u
#define DEFAULT_SWALLOW_US 1500000u

WlStrapConfig wl_strap_config_default(void) {
    WlStrapConfig config;
    memset(&config, 0, sizeof config);
    config.title = "PLEASE USE THE WRIST STRAP";
    config.body[0].text = "FASTEN IT BEFORE YOU PLAY, SO THE REMOTE";
    config.body[1].text = "STAYS IN YOUR HAND IF YOU SWING IT.";
    config.body[2].text = "HOLD THE NUNCHUK FIRMLY TOO, AND KEEP ITS CABLE CLEAR.";
    config.body[2].with_nunchuk_only = true;
    config.body[3].text = "MAKE ROOM AROUND YOU, AND STAND WELL BACK FROM THE SCREEN.";
    config.prompt = "A: CONTINUE";
    config.ignore_us = DEFAULT_IGNORE_US;
    config.swallow_us = DEFAULT_SWALLOW_US;
    return config;
}

void wl_strap_screen_init(WlStrapScreen *strap, const WlStrapConfig *config, bool enabled) {
    WlStrapConfig defaults = wl_strap_config_default();

    memset(strap, 0, sizeof *strap);
    strap->config = config != NULL ? *config : defaults;
    if (strap->config.title == NULL) {
        strap->config.title = defaults.title;
    }
    if (strap->config.prompt == NULL) {
        strap->config.prompt = defaults.prompt;
    }
    if (strap->config.ignore_us == 0) {
        strap->config.ignore_us = defaults.ignore_us;
    }
    if (strap->config.swallow_us == 0) {
        strap->config.swallow_us = defaults.swallow_us;
    }
    strap->navigation = wl_home_config_default();
    strap->navigation.classic_swap = strap->config.classic_swap;
    strap->state = enabled ? WL_STRAP_WAITING : WL_STRAP_OFF;
}

static bool any_nunchuk(const WlRawInput *raw) {
    for (int port = 0; port < WL_HOME_PORTS; port++) {
        if (raw->remotes[port].connected && raw->remotes[port].extension == WL_EXT_NUNCHUK) {
            return true;
        }
    }
    return false;
}

bool wl_strap_screen_step(WlStrapScreen *strap, const WlRawInput *raw, unsigned extra_nav, uint64_t now_us) {
    bool accept = ((wl_home_navigation(raw, &strap->navigation) | extra_nav) & WL_HOME_NAV_ACCEPT) != 0;
    bool pressed = accept && !strap->accept_was;

    strap->accept_was = accept;
    switch (strap->state) {
    case WL_STRAP_WAITING:
        strap->state = WL_STRAP_UP;
        strap->shown_us = now_us;
        strap->nunchuk = any_nunchuk(raw);
        break;
    case WL_STRAP_UP:
        strap->nunchuk = any_nunchuk(raw);
        // (a fresh press: an A held from before, or through the first second, does not go on)
        if (pressed && wl_strap_screen_ready(strap, now_us)) {
            strap->state = WL_STRAP_SWALLOWING;
            strap->gone_us = now_us;
        }
        break;
    case WL_STRAP_SWALLOWING:
        if (!accept || now_us - strap->gone_us >= strap->config.swallow_us) {
            strap->state = WL_STRAP_DONE;
        }
        break;
    default:
        break;
    }
    return wl_strap_screen_up(strap);
}

bool wl_strap_screen_up(const WlStrapScreen *strap) {
    return strap->state == WL_STRAP_UP;
}

bool wl_strap_screen_blocks_input(const WlStrapScreen *strap) {
    return strap->state == WL_STRAP_UP || strap->state == WL_STRAP_SWALLOWING;
}

bool wl_strap_screen_ready(const WlStrapScreen *strap, uint64_t now_us) {
    return strap->state == WL_STRAP_UP && now_us - strap->shown_us >= strap->config.ignore_us;
}

bool wl_strap_screen_active(const WlStrapScreen *strap) {
    return strap->state == WL_STRAP_WAITING || strap->state == WL_STRAP_UP || strap->state == WL_STRAP_SWALLOWING;
}

const char *wl_strap_screen_title(const WlStrapScreen *strap) {
    return strap->config.title;
}

const char *wl_strap_screen_line(const WlStrapScreen *strap, int index) {
    int shown = 0;

    for (int i = 0; i < WL_STRAP_MAX_LINES && strap->config.body[i].text != NULL; i++) {
        if (strap->config.body[i].with_nunchuk_only && !strap->nunchuk) {
            continue;
        }
        if (shown++ == index) {
            return strap->config.body[i].text;
        }
    }
    return NULL;
}

const char *wl_strap_screen_prompt(const WlStrapScreen *strap) {
    return strap->config.prompt;
}
