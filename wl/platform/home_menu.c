// SPDX-License-Identifier: MIT
// The HOME button's menu and the Wii's notices: see home_menu.h.
#include "wl/platform/home_menu.h"

#include "wl/platform/home_internal.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_STICK 64
#define DEFAULT_FADE_MS 300u
#define DEFAULT_NO_EXTENSION_US 1500000u
#define DEFAULT_NOTICE_US 7000000u
#define DEFAULT_BATTERY_CHECK_US 2000000u
#define DEFAULT_SWALLOW_US 1500000u
#define DEFAULT_BATTERY_LOW 10
#define DEFAULT_BATTERY_GOOD 15

WlHomeConfig wl_home_config_default(void) {
    WlHomeConfig config;
    memset(&config, 0, sizeof config);
    config.title = "HOME";
    config.resume_label = "Resume";
    config.reset_label = NULL;
    config.loader_label = "Return to the loader";
    config.confirm_text = "Unsaved progress will be lost";
    config.cancel_label = "Cancel";
    config.batteries_heading = "WII REMOTE BATTERIES";
    config.remote_label = "REMOTE {port}";
    config.hint_menu = "A: CHOOSE   B OR HOME: RESUME";
    config.hint_confirm = "A: CHOOSE   B: BACK";
    config.notice_battery = "Wii Remote {port}: Battery low ({percent}%)";
    config.notice_no_extension = "Wii Remote {port}: Connect a Nunchuk or Classic Controller to move";
    config.classic_swap = false;
    config.stick_threshold = DEFAULT_STICK;
    config.fade_ms = DEFAULT_FADE_MS;
    config.no_extension_us = DEFAULT_NO_EXTENSION_US;
    config.notice_us = DEFAULT_NOTICE_US;
    config.battery_check_us = DEFAULT_BATTERY_CHECK_US;
    config.swallow_us = DEFAULT_SWALLOW_US;
    config.battery_low = DEFAULT_BATTERY_LOW;
    config.battery_good = DEFAULT_BATTERY_GOOD;
    return config;
}

// ---- what the controllers give the menu

static unsigned remote_navigation(const WlRawRemote *remote, const WlHomeConfig *config) {
    uint32_t held = remote->buttons;
    unsigned nav = 0;
    bool classic = remote->extension == WL_EXT_CLASSIC;

    if (held & WL_WM_UP) {
        nav |= WL_HOME_NAV_UP;
    }
    if (held & WL_WM_DOWN) {
        nav |= WL_HOME_NAV_DOWN;
    }
    if (held & WL_WM_A) {
        nav |= WL_HOME_NAV_ACCEPT;
    }
    if (held & (WL_WM_B | WL_WM_1)) {
        nav |= WL_HOME_NAV_BACK;
    }
    if (classic) {
        uint32_t accept = config->classic_swap ? WL_CC_B : WL_CC_A;
        uint32_t back = config->classic_swap ? WL_CC_A : WL_CC_B;
        if (held & WL_CC_UP) {
            nav |= WL_HOME_NAV_UP;
        }
        if (held & WL_CC_DOWN) {
            nav |= WL_HOME_NAV_DOWN;
        }
        if (held & accept) {
            nav |= WL_HOME_NAV_ACCEPT;
        }
        if (held & back) {
            nav |= WL_HOME_NAV_BACK;
        }
    }
    if (classic || remote->extension == WL_EXT_NUNCHUK) {
        if (remote->stick[0][1] > config->stick_threshold) {
            nav |= WL_HOME_NAV_UP;
        } else if (remote->stick[0][1] < -config->stick_threshold) {
            nav |= WL_HOME_NAV_DOWN;
        }
    }
    return nav;
}

unsigned wl_home_navigation(const WlRawInput *raw, const WlHomeConfig *config) {
    WlHomeConfig fallback;
    unsigned nav = 0;

    if (config == NULL) {
        fallback = wl_home_config_default();
        config = &fallback;
    }
    for (int port = 0; port < WL_HOME_PORTS; port++) {
        if (raw->remotes[port].connected) {
            nav |= remote_navigation(&raw->remotes[port], config);
        }
        if (raw->pads[port].connected) {
            const WlRawPad *pad = &raw->pads[port];

            if (pad->buttons & WL_PAD_UP) {
                nav |= WL_HOME_NAV_UP;
            }
            if (pad->buttons & WL_PAD_DOWN) {
                nav |= WL_HOME_NAV_DOWN;
            }
            if (pad->buttons & WL_PAD_A) {
                nav |= WL_HOME_NAV_ACCEPT;
            }
            if (pad->buttons & WL_PAD_B) {
                nav |= WL_HOME_NAV_BACK;
            }
            if (pad->stick[0][1] > config->stick_threshold) {
                nav |= WL_HOME_NAV_UP;
            } else if (pad->stick[0][1] < -config->stick_threshold) {
                nav |= WL_HOME_NAV_DOWN;
            }
        }
    }
    return nav;
}

bool wl_home_pressed(const WlRawInput *raw) {
    for (int port = 0; port < WL_HOME_PORTS; port++) {
        const WlRawRemote *remote = &raw->remotes[port];

        if (remote->connected && ((remote->buttons & WL_WM_HOME) ||
                                  (remote->extension == WL_EXT_CLASSIC && (remote->buttons & WL_CC_HOME)))) {
            return true;
        }
    }
    return false;
}

// any button held on any controller
static bool buttons_held(const WlRawInput *raw, unsigned extra_nav) {
    if (extra_nav) {
        return true;
    }
    for (int port = 0; port < WL_HOME_PORTS; port++) {
        if ((raw->remotes[port].connected && raw->remotes[port].buttons) ||
            (raw->pads[port].connected && raw->pads[port].buttons)) {
            return true;
        }
    }
    return false;
}

// ---- the machine

static const char *or_default(const char *text, const char *fallback) {
    return text != NULL ? text : fallback;
}

void wl_home_menu_init(WlHomeMenu *menu, const WlHomeConfig *config) {
    WlHomeConfig defaults = wl_home_config_default();
    WlHomeConfig *own = &menu->config;

    memset(menu, 0, sizeof *menu);
    *own = config != NULL ? *config : defaults;
    // (the texts that can be absent are left as they were given; the others fall back)
    own->title = or_default(own->title, defaults.title);
    own->resume_label = or_default(own->resume_label, defaults.resume_label);
    own->loader_label = or_default(own->loader_label, defaults.loader_label);
    own->confirm_text = or_default(own->confirm_text, defaults.confirm_text);
    own->cancel_label = or_default(own->cancel_label, defaults.cancel_label);
    own->batteries_heading = or_default(own->batteries_heading, defaults.batteries_heading);
    own->remote_label = or_default(own->remote_label, defaults.remote_label);
    own->hint_menu = or_default(own->hint_menu, defaults.hint_menu);
    own->hint_confirm = or_default(own->hint_confirm, defaults.hint_confirm);
    if (own->stick_threshold <= 0) {
        own->stick_threshold = defaults.stick_threshold;
    }
    if (own->fade_ms == 0) {
        own->fade_ms = defaults.fade_ms;
    }
    if (own->no_extension_us == 0) {
        own->no_extension_us = defaults.no_extension_us;
    }
    if (own->notice_us == 0) {
        own->notice_us = defaults.notice_us;
    }
    if (own->battery_check_us == 0) {
        own->battery_check_us = defaults.battery_check_us;
    }
    if (own->swallow_us == 0) {
        own->swallow_us = defaults.swallow_us;
    }
    if (own->battery_low == 0) {
        own->battery_low = defaults.battery_low;
    }
    if (own->battery_good == 0) {
        own->battery_good = defaults.battery_good;
    }
}

bool wl_home_menu_open(const WlHomeMenu *menu) {
    return menu->state != WL_HOME_CLOSED;
}

bool wl_home_menu_blocks_input(const WlHomeMenu *menu) {
    return menu->state != WL_HOME_CLOSED || menu->swallowing;
}

const char *wl_home_menu_item_label(const WlHomeMenu *menu, int item) {
    switch (item) {
    case WL_HOME_ITEM_RESUME:
        return menu->config.resume_label;
    case WL_HOME_ITEM_RESET:
        return menu->config.reset_label != NULL ? menu->config.reset_label : "";
    case WL_HOME_ITEM_LOADER:
        return menu->config.loader_label;
    default:
        return "";
    }
}

const char *wl_home_menu_choice_label(const WlHomeMenu *menu, int choice) {
    return choice == WL_HOME_CHOICE_LEAVE ? menu->config.loader_label : menu->config.cancel_label;
}

const char *wl_home_menu_confirm_text(const WlHomeMenu *menu) {
    return menu->config.confirm_text;
}

static void say(const WlHomeOps *ops, const char *text) {
    if (ops != NULL && ops->log != NULL) {
        ops->log(ops->user, text);
    }
}

static void fade(const WlHomeMenu *menu, const WlHomeOps *ops, float gain) {
    if (ops != NULL && ops->audio_fade != NULL) {
        ops->audio_fade(ops->user, gain, menu->config.fade_ms);
    }
}

static bool notice_showing(const WlHomeMenu *menu, uint64_t now_us) {
    for (int kind = 0; kind < WL_HOME_NOTICES; kind++) {
        for (int port = 0; port < WL_HOME_PORTS; port++) {
            if (menu->notice_until_us[kind][port] > now_us) {
                return true;
            }
        }
    }
    return false;
}

static void open_menu(WlHomeMenu *menu, const WlHomeWorld *world, const WlHomeOps *ops) {
    char text[96];
    int count = 0;

    menu->items[count++] = WL_HOME_ITEM_RESUME;
    if (world->in_play && menu->config.reset_label != NULL) {
        menu->items[count++] = WL_HOME_ITEM_RESET;
    }
    menu->items[count++] = WL_HOME_ITEM_LOADER;
    menu->item_count = count;
    menu->selection = 0;
    menu->state = WL_HOME_MENU;
    menu->swallowing = false;
    // (the program's own pause is left as it is: only a pause made here is undone)
    menu->we_paused = world->in_play && !world->paused;
    if (menu->we_paused && ops != NULL && ops->set_paused != NULL) {
        ops->set_paused(ops->user, true);
    }
    fade(menu, ops, 0.0f);
    snprintf(text, sizeof text, "home: menu opened (%s)",
             !world->in_play ? "no level running" : world->paused ? "the game was paused already" : "game paused");
    say(ops, text);
}

// the menu away: the program running again if the menu stopped it, the sound back, and the buttons
// that closed it kept from the program until let go
static void close_menu(WlHomeMenu *menu, uint64_t now_us, const WlHomeOps *ops, bool resume) {
    if (menu->we_paused && resume && ops != NULL && ops->set_paused != NULL) {
        ops->set_paused(ops->user, false);
    }
    menu->we_paused = false;
    fade(menu, ops, 1.0f);
    menu->state = WL_HOME_CLOSED;
    menu->swallowing = true;
    menu->swallow_since_us = now_us;
    say(ops, "home: menu closed");
}

static void choose(WlHomeMenu *menu, uint64_t now_us, const WlHomeOps *ops) {
    if (menu->state == WL_HOME_CONFIRM) {
        if (menu->selection == WL_HOME_CHOICE_LEAVE) {
            say(ops, "home: returning to the loader");
            // (the program stays as it is: the console leaves)
            if (ops != NULL && ops->leave_for_loader != NULL) {
                ops->leave_for_loader(ops->user);
            }
            close_menu(menu, now_us, ops, false);
        } else {
            menu->state = WL_HOME_MENU;
            menu->selection = menu->item_count - 1;
            say(ops, "home: staying");
        }
        return;
    }
    switch (menu->items[menu->selection]) {
    case WL_HOME_ITEM_RESUME:
        close_menu(menu, now_us, ops, true);
        break;
    case WL_HOME_ITEM_RESET:
        // (the program's time running again before it goes to its menu, which sets its own)
        close_menu(menu, now_us, ops, true);
        say(ops, "home: reset to the title");
        if (ops != NULL && ops->reset != NULL) {
            ops->reset(ops->user);
        }
        break;
    case WL_HOME_ITEM_LOADER:
        menu->state = WL_HOME_CONFIRM;
        menu->selection = WL_HOME_CHOICE_CANCEL;
        say(ops, "home: asking before the loader");
        break;
    default:
        break;
    }
}

static void notice(WlHomeMenu *menu, int kind, int port, uint64_t now_us, const WlHomeOps *ops) {
    char text[96];

    menu->notice_until_us[kind][port] = now_us + menu->config.notice_us;
    menu->notice_percent[port] = menu->battery[port];
    if (kind == WL_HOME_NOTICE_BATTERY) {
        snprintf(text, sizeof text, "home: notice: Wii Remote %d battery low (%d%%)", port + 1,
                 menu->notice_percent[port]);
    } else {
        snprintf(text, sizeof text, "home: notice: Wii Remote %d has no Nunchuk or Classic Controller", port + 1);
    }
    say(ops, text);
}

// the notices that wait on time
static void notices(WlHomeMenu *menu, const WlRawInput *raw, const WlHomeWorld *world, uint64_t now_us,
                    const WlHomeOps *ops) {
    bool battery_check = now_us >= menu->next_battery_check_us;

    if (battery_check) {
        menu->next_battery_check_us = now_us + menu->config.battery_check_us;
    }
    for (int port = 0; port < WL_HOME_PORTS; port++) {
        const WlRawRemote *remote = &raw->remotes[port];

        menu->present[port] = remote->connected;
        menu->extension[port] = remote->extension;
        menu->battery[port] = remote->battery;
        if (!remote->connected) {
            // (a Remote that comes back is read afresh: the battery it first reports may be 0 before
            // its first status report)
            menu->battery_known[port] = false;
            menu->no_extension_since_us[port] = 0;
            menu->no_extension_told[port] = false;
            continue;
        }
        if (battery_check && menu->config.notice_battery != NULL) {
            if (remote->battery > 0) {
                menu->battery_known[port] = true;
            }
            if (menu->battery_known[port]) {
                if (remote->battery >= menu->config.battery_good) {
                    menu->battery_told[port] = false;
                } else if (remote->battery < menu->config.battery_low && !menu->battery_told[port]) {
                    menu->battery_told[port] = true;
                    notice(menu, WL_HOME_NOTICE_BATTERY, port, now_us, ops);
                }
            }
        }
        // a Remote alone moves nobody: in play, with a player on it, and for a while (a Nunchuk
        // plugged in is read as none for a moment)
        if (menu->config.notice_no_extension != NULL && world->in_play && world->local_player[port] &&
            remote->extension == WL_EXT_NONE) {
            if (!menu->no_extension_since_us[port]) {
                menu->no_extension_since_us[port] = now_us ? now_us : 1;
            } else if (!menu->no_extension_told[port] &&
                       now_us - menu->no_extension_since_us[port] >= menu->config.no_extension_us) {
                menu->no_extension_told[port] = true;
                notice(menu, WL_HOME_NOTICE_NO_EXTENSION, port, now_us, ops);
            }
        } else {
            menu->no_extension_since_us[port] = 0;
            menu->no_extension_told[port] = false;
        }
    }
}

void wl_home_menu_step(WlHomeMenu *menu, const WlRawInput *raw, unsigned extra_nav, bool home_extra,
                       const WlHomeWorld *world, uint64_t now_us, const WlHomeOps *ops) {
    unsigned nav = wl_home_navigation(raw, &menu->config) | extra_nav;
    unsigned pressed = nav & ~menu->previous_nav;
    bool home_down = wl_home_pressed(raw) || home_extra;
    bool home_edge = home_down && !menu->previous_home;
    bool visible;

    menu->previous_nav = nav;
    menu->previous_home = home_down;

    if (menu->state == WL_HOME_CLOSED) {
        if (home_edge) {
            open_menu(menu, world, ops);
        } else if (menu->swallowing && (!buttons_held(raw, extra_nav) ||
                                        now_us - menu->swallow_since_us >= menu->config.swallow_us)) {
            menu->swallowing = false;
        }
    } else if (home_edge) {
        close_menu(menu, now_us, ops, true);
    } else if (pressed & WL_HOME_NAV_BACK) {
        if (menu->state == WL_HOME_CONFIRM) {
            menu->state = WL_HOME_MENU;
            menu->selection = menu->item_count - 1;
        } else {
            close_menu(menu, now_us, ops, true);
        }
    } else if (pressed & (WL_HOME_NAV_UP | WL_HOME_NAV_DOWN)) {
        int count = menu->state == WL_HOME_CONFIRM ? WL_HOME_CHOICES : menu->item_count;

        menu->selection = (menu->selection + (pressed & WL_HOME_NAV_UP ? count - 1 : 1)) % count;
    } else if (pressed & WL_HOME_NAV_ACCEPT) {
        choose(menu, now_us, ops);
    }

    notices(menu, raw, world, now_us, ops);

    visible = menu->state != WL_HOME_CLOSED || notice_showing(menu, now_us);
    if (visible != menu->shown) {
        menu->shown = visible;
        if (ops != NULL && ops->visible != NULL) {
            ops->visible(ops->user, visible);
        }
    }
}

// ---- the notices' words

unsigned wl_home_fill_template(const char *templ, int port, int percent, char *out, unsigned size) {
    unsigned length = 0;

    if (size == 0) {
        return 0;
    }
    while (*templ != '\0' && length + 1 < size) {
        char number[12];
        const char *put = NULL;

        if (strncmp(templ, "{port}", 6) == 0) {
            snprintf(number, sizeof number, "%d", port);
            put = number;
            templ += 6;
        } else if (strncmp(templ, "{percent}", 9) == 0) {
            snprintf(number, sizeof number, "%d", percent);
            put = number;
            templ += 9;
        }
        if (put != NULL) {
            while (*put != '\0' && length + 1 < size) {
                out[length++] = *put++;
            }
        } else {
            out[length++] = *templ++;
        }
    }
    out[length] = '\0';
    return length;
}

unsigned wl_home_menu_notice_text(const WlHomeMenu *menu, uint64_t now_us, char *text, unsigned size) {
    unsigned length = 0;

    if (size) {
        text[0] = '\0';
    }
    for (int port = 0; port < WL_HOME_PORTS; port++) {
        for (int pass = 0; pass < 2; pass++) {
            // (a Remote with nothing to move with is told before its battery)
            int kind = pass == 0 ? WL_HOME_NOTICE_NO_EXTENSION : WL_HOME_NOTICE_BATTERY;
            const char *templ = kind == WL_HOME_NOTICE_BATTERY ? menu->config.notice_battery
                                                               : menu->config.notice_no_extension;
            char line[160];
            unsigned written;

            if (templ == NULL || menu->notice_until_us[kind][port] <= now_us) {
                continue;
            }
            written = wl_home_fill_template(templ, port + 1, menu->notice_percent[port], line, sizeof line - 1);
            line[written++] = '\n';
            line[written] = '\0';
            if (length + written < size) {
                memcpy(text + length, line, (size_t)written + 1);
                length += written;
            }
        }
    }
    return length;
}
