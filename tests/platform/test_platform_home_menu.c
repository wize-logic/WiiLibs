// SPDX-License-Identifier: MIT
// The HOME menu's state machine and the Wii's notices (wl/platform/home_menu.h) against fake
// controllers, a fake program and a clock. HOME opens the menu (pausing the program once, unless it was
// paused already) and closes it on the next press or on B; the items, the confirmation before the
// loader, the buttons held while it closes kept from the program, the navigation each kind of
// controller gives it, the notices (a Remote with nothing to move with, a weak battery: once,
// debounced, not polled every frame), the configuration (a menu with no reset item, the Classic
// Controller's buttons either way round, the texts and times) and that nothing is drawn or asked
// while the menu is closed and no notice shows.
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/home_menu.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/overlay.h"

// the menu of a game that has a reset and swaps the Classic Controller's buttons
static WlHomeConfig g_config;

/* ---------- a fake game and console */

static struct
{
    int paused_calls, resumed_calls;
    bool paused;
    int fades;
    float gain;
    unsigned fade_ms;
    int resets, leaves;
    int visible_calls;
    bool visible;
    char log[64][112];
    int log_count;
} fake;

static void fake_set_paused(void *user, bool paused)
{
    (void)user;
    fake.paused = paused;
    if (paused)
        fake.paused_calls++;
    else
        fake.resumed_calls++;
}

static void fake_audio_fade(void *user, float gain, unsigned ms)
{
    (void)user;
    fake.fades++;
    fake.gain = gain;
    fake.fade_ms = ms;
}

static void fake_reset(void *user)
{
    (void)user;
    fake.resets++;
}

static void fake_leave(void *user)
{
    (void)user;
    fake.leaves++;
}

static void fake_visible(void *user, bool visible)
{
    (void)user;
    fake.visible_calls++;
    fake.visible = visible;
}

static void fake_log(void *user, const char *text)
{
    (void)user;
    if (fake.log_count < 64)
        snprintf(fake.log[fake.log_count++], sizeof(fake.log[0]), "%s", text);
}

static const WlHomeOps ops = { NULL, fake_set_paused, fake_audio_fade, fake_reset, fake_leave, fake_visible, fake_log };

static bool logged(const char *part)
{
    int index;

    for (index = 0; index < fake.log_count; index++)
    {
        if (strstr(fake.log[index], part))
            return true;
    }
    return false;
}

/* ---------- a rig: the machine, the controllers, the game, the time */

static struct
{
    WlHomeMenu home;
    WlRawInput raw;
    WlHomeWorld world;
    uint64_t now;
    unsigned extra_nav;
    bool home_extra;
} rig;

static void start(void)
{
    memset(&fake, 0, sizeof(fake));
    memset(&rig, 0, sizeof(rig));
    wl_home_menu_init(&rig.home, &g_config);
    rig.raw.remotes[0].connected = true;
    rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
    rig.raw.remotes[0].battery = 80;
    rig.world.in_play = true;
    rig.world.local_player[0] = true;
    rig.now = 1000000;
}

/* a look 33 ms later */
static void look(void)
{
    rig.now += 33000;
    wl_home_menu_step(&rig.home, &rig.raw, rig.extra_nav, rig.home_extra, &rig.world, rig.now, &ops);
}

static void looks(int count)
{
    while (count-- > 0)
        look();
}

/* a press: the buttons down for two looks, then up for two */
static void press_remote(uint32_t buttons)
{
    rig.raw.remotes[0].buttons = buttons;
    looks(2);
    rig.raw.remotes[0].buttons = 0;
    looks(2);
}

static void open_menu(void)
{
    press_remote(WL_WM_HOME);
}

/* ---------- the checks */

TEST check_closed_costs_nothing(void)
{
    start();
    looks(300);
    ASSERTm("closed at the start", !wl_home_menu_open(&rig.home) && !wl_home_menu_blocks_input(&rig.home));
    ASSERTm("a closed menu asks for nothing:  visible,  fades,  pauses,  log lines", fake.visible_calls == 0 && fake.fades == 0 && fake.paused_calls == 0 && fake.log_count == 0);
    /* the game's buttons are not the menu's while it is closed */
    rig.raw.remotes[0].buttons = WL_WM_A | WL_WM_UP | WL_WM_B | WL_WM_DOWN;
    looks(10);
    ASSERTm("A, B and the D-pad open nothing", !wl_home_menu_open(&rig.home) && fake.visible_calls == 0);
    /* a GameCube controller has no HOME: its Start and every other button open nothing */
    rig.raw.remotes[0].buttons = 0;
    rig.raw.pads[1].connected = true;
    rig.raw.pads[1].buttons = 0xFFFF;
    looks(10);
    ASSERTm("no GameCube button opens it", !wl_home_menu_open(&rig.home));
    /* (and HOME of a Remote not connected is nothing) */
    rig.raw.pads[1].connected = false;
    rig.raw.remotes[2].connected = false;
    rig.raw.remotes[2].buttons = WL_WM_HOME;
    looks(5);
    ASSERTm("a Remote that is not there has no HOME", !wl_home_menu_open(&rig.home));
    PASS();
}

TEST check_open_and_close(void)
{
    start();
    rig.raw.remotes[0].buttons = WL_WM_HOME;
    looks(1);
    ASSERTm("HOME opens the menu on the first look", wl_home_menu_open(&rig.home) && wl_home_menu_blocks_input(&rig.home));
    ASSERTm("the game is paused, once", fake.paused_calls == 1 && fake.paused);
    ASSERTm("the sound fades out over 300 ms", fake.fades == 1 && fake.gain == 0.0f && fake.fade_ms == 300);
    ASSERTm("the overlay is asked for", fake.visible && fake.visible_calls == 1);
    ASSERTm("Resume, Reset to title, Return to the loader, Resume chosen", rig.home.state == WL_HOME_MENU && rig.home.selection == 0 && rig.home.item_count == 3 && rig.home.items[0] == WL_HOME_ITEM_RESUME && rig.home.items[1] == WL_HOME_ITEM_RESET && rig.home.items[2] == WL_HOME_ITEM_LOADER);
    /* held, it is one press */
    looks(30);
    ASSERTm("HOME held does not toggle the menu", rig.home.state == WL_HOME_MENU && fake.paused_calls == 1 && fake.resumed_calls == 0);
    rig.raw.remotes[0].buttons = 0;
    looks(3);
    ASSERTm("letting go does not close it", wl_home_menu_open(&rig.home));
    /* the second press closes it */
    rig.raw.remotes[0].buttons = WL_WM_HOME;
    looks(1);
    ASSERTm("HOME again closes it", !wl_home_menu_open(&rig.home));
    ASSERTm("the game runs again, once", fake.resumed_calls == 1 && !fake.paused);
    ASSERTm("the sound fades back in over 300 ms", fake.fades == 2 && fake.gain == 1.0f && fake.fade_ms == 300);
    ASSERTm("the overlay is let go", !fake.visible && fake.visible_calls == 2);
    ASSERTm("HOME still down: the game still sees nothing", wl_home_menu_blocks_input(&rig.home));
    rig.raw.remotes[0].buttons = 0;
    looks(1);
    ASSERTm("and everything the game sees once the button is up", !wl_home_menu_blocks_input(&rig.home));
    PASS();
}

TEST check_back(void)
{
    start();
    open_menu();
    press_remote(WL_WM_B);
    ASSERTm("B closes it, resuming the game", !wl_home_menu_open(&rig.home) && fake.resumed_calls == 1 && fake.gain == 1.0f);

    start();
    open_menu();
    press_remote(WL_WM_1);
    ASSERTm("so does 1", !wl_home_menu_open(&rig.home));
    PASS();
}

TEST check_navigation(void)
{
    start();
    open_menu();
    press_remote(WL_WM_DOWN);
    ASSERTm("down moves to Reset to title", rig.home.selection == 1);
    press_remote(WL_WM_DOWN);
    ASSERTm("down moves to Return to the loader", rig.home.selection == 2);
    press_remote(WL_WM_DOWN);
    ASSERTm("down wraps to Resume", rig.home.selection == 0);
    press_remote(WL_WM_UP);
    ASSERTm("up wraps to the last", rig.home.selection == 2);
    /* held, it moves once */
    rig.raw.remotes[0].buttons = WL_WM_UP;
    looks(30);
    ASSERTm("a held direction moves once", rig.home.selection == 1);
    rig.raw.remotes[0].buttons = 0;
    looks(2);
    /* the Nunchuk's stick */
    rig.raw.remotes[0].stick[0][1] = 100;
    looks(2);
    rig.raw.remotes[0].stick[0][1] = 0;
    looks(2);
    ASSERTm("the Nunchuk stick up moves up", rig.home.selection == 0);
    rig.raw.remotes[0].stick[0][1] = -100;
    looks(2);
    rig.raw.remotes[0].stick[0][1] = 20;
    looks(2);
    ASSERTm("down on the stick moves down, a small push does nothing", rig.home.selection == 1);
    /* a stick of a Remote with no extension is not read */
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    rig.raw.remotes[0].stick[0][1] = -100;
    looks(4);
    ASSERTm("no stick without a Nunchuk", rig.home.selection == 1);
    rig.raw.remotes[0].stick[0][1] = 0;

    /* the Classic Controller: its own bits, b accepts and a goes back */
    rig.raw.remotes[0].extension = WL_EXT_CLASSIC;
    press_remote(WL_CC_DOWN);
    ASSERTm("the Classic Controller's D-pad moves", rig.home.selection == 2);
    press_remote(WL_CC_UP);
    press_remote(WL_CC_UP);
    ASSERTm("and up", rig.home.selection == 0);
    rig.raw.remotes[0].buttons = 0;
    press_remote(WL_CC_A);
    ASSERTm("the Classic Controller's a goes back", !wl_home_menu_open(&rig.home));

    /* the Nunchuk's Z (0x10000) is not the Classic Controller's up */
    start();
    open_menu();
    rig.raw.remotes[0].buttons = WL_NC_Z;
    looks(3);
    rig.raw.remotes[0].buttons = 0;
    looks(3);
    ASSERTm("the Nunchuk's Z is not a direction", rig.home.selection == 0);

    /* a GameCube controller steers it (not opens it) */
    start();
    open_menu();
    rig.raw.pads[2].connected = true;
    rig.raw.pads[2].buttons = WL_PAD_DOWN;
    looks(2);
    rig.raw.pads[2].buttons = 0;
    looks(2);
    ASSERTm("a GameCube D-pad moves it", rig.home.selection == 1);
    rig.raw.pads[2].stick[0][1] = -120;
    looks(2);
    rig.raw.pads[2].stick[0][1] = 0;
    looks(2);
    ASSERTm("and its stick", rig.home.selection == 2);
    rig.raw.pads[2].buttons = WL_PAD_B;
    looks(2);
    rig.raw.pads[2].buttons = 0;
    looks(2);
    ASSERTm("and B goes back", !wl_home_menu_open(&rig.home));

    /* a controller the console does not have (a script's) */
    {
        start();
        rig.home_extra = true;
        looks(2);
        rig.home_extra = false;
        ASSERTm("a script's HOME opens it", wl_home_menu_open(&rig.home));
        rig.extra_nav = WL_HOME_NAV_DOWN;
        looks(2);
        rig.extra_nav = 0;
        looks(2);
        ASSERTm("a script's down moves it", rig.home.selection == 1);
        rig.extra_nav = WL_HOME_NAV_ACCEPT;
        looks(2);
        rig.extra_nav = 0;
        looks(2);
        ASSERTm("a script's A chooses the reset", fake.resets == 1);
    }
    PASS();
}

TEST check_items(void)
{
    /* Resume */
    start();
    open_menu();
    press_remote(WL_WM_A);
    ASSERTm("Resume closes it", !wl_home_menu_open(&rig.home) && fake.resumed_calls == 1 && fake.resets == 0 && fake.leaves == 0);
    ASSERTm("and the sound comes back", fake.gain == 1.0f);

    /* Reset to title: the game runs again, then goes to its menu */
    start();
    open_menu();
    press_remote(WL_WM_DOWN);
    press_remote(WL_WM_A);
    ASSERTm("Reset to title resumes the game and goes to the menu once", !wl_home_menu_open(&rig.home) && fake.resets == 1 && fake.resumed_calls == 1 && fake.leaves == 0);
    ASSERTm("and says so in the log", logged("reset to the title"));

    /* the loader asks first */
    start();
    open_menu();
    press_remote(WL_WM_DOWN);
    press_remote(WL_WM_DOWN);
    press_remote(WL_WM_A);
    ASSERTm("the loader asks first, with Cancel chosen", rig.home.state == WL_HOME_CONFIRM && rig.home.selection == WL_HOME_CHOICE_CANCEL && fake.leaves == 0 && wl_home_menu_open(&rig.home));
    ASSERTm("the warning says what is lost", strstr(wl_home_menu_confirm_text(&rig.home), "Unsaved progress will be lost") != NULL);
    press_remote(WL_WM_A);
    ASSERTm("Cancel is back in the menu, on the loader", rig.home.state == WL_HOME_MENU && rig.home.selection == 2 && fake.leaves == 0);
    press_remote(WL_WM_A);
    press_remote(WL_WM_B);
    ASSERTm("B in the question is back in the menu", rig.home.state == WL_HOME_MENU && fake.leaves == 0);
    press_remote(WL_WM_A);
    press_remote(WL_WM_DOWN);
    ASSERTm("down is Return to the loader", rig.home.selection == WL_HOME_CHOICE_LEAVE);
    press_remote(WL_WM_DOWN);
    ASSERTm("and wraps", rig.home.selection == WL_HOME_CHOICE_CANCEL);
    press_remote(WL_WM_UP);
    press_remote(WL_WM_A);
    ASSERTm("choosing it leaves, once", fake.leaves == 1);
    ASSERTm("after logging it", logged("returning to the loader"));
    ASSERTm("and does not reset", fake.resets == 0);

    /* HOME in the question resumes */
    start();
    open_menu();
    press_remote(WL_WM_UP);
    press_remote(WL_WM_A);
    ASSERTm("up from Resume is the loader", rig.home.state == WL_HOME_CONFIRM);
    press_remote(WL_WM_HOME);
    ASSERTm("HOME in the question resumes", !wl_home_menu_open(&rig.home) && fake.leaves == 0 && fake.resumed_calls == 1);
    PASS();
}

TEST check_pause_rules(void)
{
    /* the game was paused by its own menu: left as it was */
    start();
    rig.world.paused = true;
    fake.paused = true;
    open_menu();
    ASSERTm("a game paused already is not paused again", wl_home_menu_open(&rig.home) && fake.paused_calls == 0);
    press_remote(WL_WM_HOME);
    ASSERTm("and is not resumed by the menu", !wl_home_menu_open(&rig.home) && fake.resumed_calls == 0 && fake.paused);
    ASSERTm("the sound is the menu's either way", fake.gain == 1.0f && fake.fades == 2);

    /* no level running: no Reset to title, no pause */
    start();
    rig.world.in_play = false;
    open_menu();
    ASSERTm("no level: nothing paused", wl_home_menu_open(&rig.home) && fake.paused_calls == 0);
    ASSERTm("Resume and the loader only", rig.home.item_count == 2 && rig.home.items[0] == WL_HOME_ITEM_RESUME && rig.home.items[1] == WL_HOME_ITEM_LOADER);
    press_remote(WL_WM_A);
    ASSERTm("Resume closes it, resuming nothing", !wl_home_menu_open(&rig.home) && fake.resumed_calls == 0);

    /* the console leaving does not run the game again on the way out */
    start();
    open_menu();
    press_remote(WL_WM_UP);
    press_remote(WL_WM_A);
    press_remote(WL_WM_DOWN);
    press_remote(WL_WM_A);
    ASSERTm("leaving for the loader does not run the game", fake.leaves == 1 && fake.resumed_calls == 0);
    PASS();
}

TEST check_swallow(void)
{
    /* A chose Resume and is still down: the game does not jump */
    start();
    open_menu();
    rig.raw.remotes[0].buttons = WL_WM_A;
    looks(2);
    ASSERTm("closed with A down: the game sees nothing yet", !wl_home_menu_open(&rig.home) && wl_home_menu_blocks_input(&rig.home));
    looks(10);
    ASSERTm("for as long as it is down", wl_home_menu_blocks_input(&rig.home));
    rig.raw.remotes[0].buttons = 0;
    looks(1);
    ASSERTm("and no longer", !wl_home_menu_blocks_input(&rig.home));

    /* a button stuck down does not hold the game off for ever */
    start();
    open_menu();
    rig.raw.remotes[0].buttons = WL_WM_A;
    looks(2);
    rig.now += g_config.swallow_us;
    looks(1);
    ASSERTm("after a second and a half the game has its controller back", !wl_home_menu_blocks_input(&rig.home));

    /* another controller's button counts as held too */
    start();
    open_menu();
    rig.raw.pads[3].connected = true;
    rig.raw.pads[3].buttons = WL_PAD_A;
    rig.raw.remotes[0].buttons = WL_WM_A;
    looks(2);
    rig.raw.remotes[0].buttons = 0;
    looks(2);
    ASSERTm("a GameCube button held keeps it off as well", wl_home_menu_blocks_input(&rig.home));
    rig.raw.pads[3].buttons = 0;
    looks(1);
    ASSERTm("until it is let go", !wl_home_menu_blocks_input(&rig.home));

    /* a button down as HOME is pressed does not choose anything */
    start();
    rig.raw.remotes[0].buttons = WL_WM_A | WL_WM_HOME;
    looks(3);
    ASSERTm("A held as the menu opens does not choose Resume", wl_home_menu_open(&rig.home) && rig.home.state == WL_HOME_MENU && fake.resumed_calls == 0);
    PASS();
}

TEST check_home_variants(void)
{
    /* a Classic Controller's own HOME */
    start();
    rig.raw.remotes[0].extension = WL_EXT_CLASSIC;
    press_remote(WL_CC_HOME);
    ASSERTm("a Classic Controller's HOME opens it", wl_home_menu_open(&rig.home));

    /* its bit means nothing on a Remote without one */
    start();
    rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
    press_remote(WL_CC_HOME);
    ASSERTm("that bit is not HOME on a Nunchuk's Remote", !wl_home_menu_open(&rig.home));

    /* any Remote's */
    start();
    rig.raw.remotes[3].connected = true;
    rig.raw.remotes[3].extension = WL_EXT_NONE;
    rig.raw.remotes[3].buttons = WL_WM_HOME;
    looks(2);
    ASSERTm("any Remote's HOME opens it", wl_home_menu_open(&rig.home));
    /* ... and any Remote's closes it */
    rig.raw.remotes[3].buttons = 0;
    looks(2);
    rig.raw.remotes[0].buttons = WL_WM_HOME;
    looks(2);
    ASSERTm("any Remote's HOME closes it", !wl_home_menu_open(&rig.home));
    PASS();
}

TEST check_menu_batteries(void)
{
    start();
    rig.raw.remotes[1].connected = true;
    rig.raw.remotes[1].extension = WL_EXT_CLASSIC;
    rig.raw.remotes[1].battery = 35;
    open_menu();
    ASSERTm("the menu knows which Remotes are there", rig.home.present[0] && rig.home.present[1] && !rig.home.present[2] && !rig.home.present[3]);
    ASSERTm("and their batteries", rig.home.battery[0] == 80 && rig.home.battery[1] == 35);
    rig.raw.remotes[1].battery = 12;
    looks(1);
    ASSERTm("as they change", rig.home.battery[1] == 12);
    PASS();
}

TEST check_no_extension_notice(void)
{
    char text[256];
    int index;

    /* a Remote alone in play, with a player on it */
    start();
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(20); /* 0.66 s */
    ASSERTm("nothing said at once: a Nunchuk plugged in takes a moment", !rig.home.no_extension_told[0] && !fake.visible);
    looks(30); /* 1.65 s */
    ASSERTm("said after a moment and a half", fake.visible && rig.home.no_extension_told[0]);
    ASSERTm("the words: connect a Nunchuk or Classic Controller to move: ''", wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 && strstr(text, "Wii Remote 1") && strstr(text, "Connect a Nunchuk or Classic Controller to move"));
    ASSERTm("a notice does not pause the game or take the controller", !wl_home_menu_open(&rig.home) && !wl_home_menu_blocks_input(&rig.home) && fake.paused_calls == 0);
    ASSERTm("the overlay asked for once", fake.visible_calls == 1);
    /* it goes after its time, and is not said again while it lasts */
    for (index = 0; index < 400; index++)
        look();
    ASSERTm("the notice goes after a few seconds", !fake.visible && fake.visible_calls == 2);
    ASSERTm("with no text", wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof(text)) == 0);
    looks(100);
    ASSERTm("and is not said again while nothing changes", fake.visible_calls == 2);
    /* a Nunchuk plugged in, and pulled out: said again */
    rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
    looks(5);
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(60);
    ASSERTm("pulled out again: said again", fake.visible && fake.visible_calls == 3);

    /* a Nunchuk that is plugged in within the moment is not complained of */
    start();
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(30); /* 1 s */
    rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
    looks(3);
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(30);
    ASSERTm("the moment starts afresh when something was plugged in", !fake.visible);

    /* not in play: the menus work with a Remote alone */
    start();
    rig.world.in_play = false;
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(120);
    ASSERTm("no notice in the menus", !fake.visible);

    /* no player on that Remote */
    start();
    rig.world.local_player[0] = false;
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(120);
    ASSERTm("no notice for a Remote with no player", !fake.visible);

    /* a Classic Controller, a Motion Plus, or a Remote that is gone */
    start();
    rig.raw.remotes[0].extension = WL_EXT_CLASSIC;
    looks(120);
    rig.raw.remotes[0].extension = WL_EXT_MOTIONPLUS;
    looks(120);
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    rig.raw.remotes[0].connected = false;
    looks(120);
    ASSERTm("no notice for a Remote with something plugged in, or none there", !fake.visible);

    /* it comes with the menu, and goes on after the menu closes */
    start();
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(50);
    ASSERTm("notice up", fake.visible);
    open_menu();
    ASSERTm("the menu opening does not ask for the overlay again", fake.visible && fake.visible_calls == 1);
    press_remote(WL_WM_HOME);
    ASSERTm("the notice is still up after the menu closes", fake.visible);
    PASS();
}

TEST check_battery_notice(void)
{
    char text[256];
    int index;
    int told_at = -1;

    start();
    /* well charged for five seconds */
    for (index = 0; index < 150; index++)
        look();
    ASSERTm("a good battery says nothing", !fake.visible);
    rig.raw.remotes[0].battery = 8;
    for (index = 0; index < 200 && told_at < 0; index++)
    {
        look();
        if (fake.visible)
            told_at = index;
    }
    /* looked at every two seconds (60 looks), not on the look it fell */
    ASSERTm("told within two seconds of the fall, and not at once (look )", told_at > 0 && told_at <= 62);
    ASSERTm("the words name the Remote and the level: ''", wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 && strstr(text, "Wii Remote 1") && strstr(text, "Battery low (8%)"));
    /* once, however long it stays low */
    for (index = 0; index < 1500; index++)
        look();
    ASSERTm("told once: up, and down after its seconds", fake.visible_calls == 2 && !fake.visible);
    /* a little recovery does not rearm it, a full one does */
    rig.raw.remotes[0].battery = 12;
    for (index = 0; index < 300; index++)
        look();
    rig.raw.remotes[0].battery = 9;
    for (index = 0; index < 300; index++)
        look();
    ASSERTm("9% after 12% is the same low battery", fake.visible_calls == 2);
    rig.raw.remotes[0].battery = 60;
    for (index = 0; index < 300; index++)
        look();
    rig.raw.remotes[0].battery = 7;
    for (index = 0; index < 300; index++)
        look();
    ASSERTm("new batteries and a new fall: told again", fake.visible_calls == 4);

    /* a 0 is a Remote that has not said (its first status report is on its way) */
    start();
    rig.raw.remotes[0].battery = 0;
    looks(300);
    ASSERTm("0% at the start is not known yet", !fake.visible);
    rig.raw.remotes[0].battery = 60;
    looks(100);
    rig.raw.remotes[0].battery = 0;
    looks(100);
    ASSERTm("a battery that was known and then reads 0 is flat", fake.visible);

    /* the second Remote's, by its number */
    start();
    rig.raw.remotes[2].connected = true;
    rig.raw.remotes[2].extension = WL_EXT_NUNCHUK;
    rig.raw.remotes[2].battery = 4;
    looks(70);
    ASSERTm("Remote 3's battery: ''", fake.visible && wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 && strstr(text, "Wii Remote 3: Battery low (4%)"));

    /* a Remote that is gone and returns is read afresh */
    start();
    rig.raw.remotes[0].battery = 60;
    looks(100);
    rig.raw.remotes[0].connected = false;
    looks(100);
    rig.raw.remotes[0].connected = true;
    rig.raw.remotes[0].battery = 0;
    looks(150);
    ASSERTm("a reconnected Remote's 0 is not yet known", !fake.visible);

    /* both notices at once, in a text of two lines */
    start();
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    rig.raw.remotes[0].battery = 3;
    looks(80);
    {
        unsigned length = wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof(text));
        int lines = 0;

        for (index = 0; index < (int)length; index++)
            lines += text[index] == '\n';
        ASSERTm("both at once, a line each:  lines ''", lines == 2 && strstr(text, "Nunchuk") && strstr(text, "Battery low"));
    }
    PASS();
}

TEST check_not_polled_every_frame(void)
{
    /* a battery that fell between two looks of the battery is not seen until the next: the
    look is every two seconds */
    start();
    looks(10);
    rig.raw.remotes[0].battery = 5;
    looks(10);
    ASSERTm("a fall is not seen on the next look, only on the next battery look", !fake.visible);
    PASS();
}

/* ---------- the configuration */

TEST a_menu_with_no_reset_item_has_only_resume_and_the_loader(void)
{
    WlHomeConfig config = wl_home_config_default();

    start();
    ASSERT(config.reset_label == NULL);
    wl_home_menu_init(&rig.home, &config);
    rig.raw.remotes[0].buttons = WL_WM_HOME;
    looks(2);
    ASSERT_EQ(2, rig.home.item_count);
    ASSERT_EQ(WL_HOME_ITEM_RESUME, rig.home.items[0]);
    ASSERT_EQ(WL_HOME_ITEM_LOADER, rig.home.items[1]);
    ASSERTm("the program is still paused", fake.paused);
    ASSERT_STR_EQ("", wl_home_menu_item_label(&rig.home, WL_HOME_ITEM_RESET));
    PASS();
}

TEST the_classic_controllers_buttons_go_either_way(void)
{
    WlHomeConfig plain = wl_home_config_default();
    WlRawInput raw;

    memset(&raw, 0, sizeof raw);
    raw.remotes[0].connected = true;
    raw.remotes[0].extension = WL_EXT_CLASSIC;
    raw.remotes[0].buttons = WL_CC_A;
    ASSERT_EQ((unsigned)WL_HOME_NAV_ACCEPT, wl_home_navigation(&raw, &plain));
    raw.remotes[0].buttons = WL_CC_B;
    ASSERT_EQ((unsigned)WL_HOME_NAV_BACK, wl_home_navigation(&raw, &plain));
    ASSERT_EQ((unsigned)WL_HOME_NAV_BACK, wl_home_navigation(&raw, NULL));
    raw.remotes[0].buttons = WL_CC_A;
    ASSERT_EQ((unsigned)WL_HOME_NAV_BACK, wl_home_navigation(&raw, &g_config));
    raw.remotes[0].buttons = WL_CC_B;
    ASSERT_EQ((unsigned)WL_HOME_NAV_ACCEPT, wl_home_navigation(&raw, &g_config));
    /* the Remote's own buttons are the same either way */
    raw.remotes[0].extension = WL_EXT_NONE;
    raw.remotes[0].buttons = WL_WM_A | WL_WM_UP;
    ASSERT_EQ((unsigned)(WL_HOME_NAV_ACCEPT | WL_HOME_NAV_UP), wl_home_navigation(&raw, &plain));
    ASSERT_EQ((unsigned)(WL_HOME_NAV_ACCEPT | WL_HOME_NAV_UP), wl_home_navigation(&raw, &g_config));
    PASS();
}

TEST the_stick_threshold_is_the_configurations(void)
{
    WlHomeConfig config = wl_home_config_default();
    WlRawInput raw;

    memset(&raw, 0, sizeof raw);
    raw.remotes[0].connected = true;
    raw.remotes[0].extension = WL_EXT_NUNCHUK;
    raw.remotes[0].stick[0][1] = 50;
    ASSERT_EQ(0u, wl_home_navigation(&raw, &config)); /* (64 by default) */
    config.stick_threshold = 40;
    ASSERT_EQ((unsigned)WL_HOME_NAV_UP, wl_home_navigation(&raw, &config));
    raw.remotes[0].stick[0][1] = -50;
    ASSERT_EQ((unsigned)WL_HOME_NAV_DOWN, wl_home_navigation(&raw, &config));
    PASS();
}

TEST the_texts_and_times_are_the_configurations(void)
{
    WlHomeConfig config = wl_home_config_default();
    char text[256];

    config.title = "PAUSED";
    config.reset_label = "Back to the start";
    config.loader_label = "Quit";
    config.confirm_text = "Sure?";
    config.cancel_label = "No";
    config.fade_ms = 100;
    config.swallow_us = 200000u;
    config.notice_us = 1000000u;
    config.no_extension_us = 500000u;
    config.battery_low = 40;
    config.battery_good = 50;
    config.notice_no_extension = "PLAYER {port} NEEDS A STICK";
    config.notice_battery = "{percent} PERCENT LEFT ON {port}";
    start();
    wl_home_menu_init(&rig.home, &config);
    ASSERT_STR_EQ("Back to the start", wl_home_menu_item_label(&rig.home, WL_HOME_ITEM_RESET));
    ASSERT_STR_EQ("Quit", wl_home_menu_item_label(&rig.home, WL_HOME_ITEM_LOADER));
    ASSERT_STR_EQ("Quit", wl_home_menu_choice_label(&rig.home, WL_HOME_CHOICE_LEAVE));
    ASSERT_STR_EQ("No", wl_home_menu_choice_label(&rig.home, WL_HOME_CHOICE_CANCEL));
    ASSERT_STR_EQ("Sure?", wl_home_menu_confirm_text(&rig.home));
    open_menu();
    ASSERT_EQ(100u, fake.fade_ms);
    rig.raw.remotes[0].buttons = WL_WM_B;
    looks(2);
    ASSERT(wl_home_menu_blocks_input(&rig.home));
    rig.now += 200000u;
    looks(1);
    ASSERTm("the program has its controller after the configured hold", !wl_home_menu_blocks_input(&rig.home));
    rig.raw.remotes[0].buttons = 0;

    /* the notices, in its words and times */
    start();
    wl_home_menu_init(&rig.home, &config);
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    rig.raw.remotes[0].battery = 35; /* (low by its levels, not by the default's) */
    looks(30);
    ASSERTm("told after its half second", fake.visible);
    ASSERT(wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof text) > 0);
    ASSERT_STR_EQ("PLAYER 1 NEEDS A STICK\n35 PERCENT LEFT ON 1\n", text);
    looks(40);
    ASSERTm("gone after its second", !fake.visible);

    /* no template: no notice */
    config.notice_battery = NULL;
    config.notice_no_extension = NULL;
    start();
    wl_home_menu_init(&rig.home, &config);
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    rig.raw.remotes[0].battery = 5;
    looks(200);
    ASSERTm("a program that wants no notices gets none", fake.visible_calls == 0);
    PASS();
}

TEST a_text_too_long_for_the_buffer_is_left_out_not_overrun(void)
{
    WlHomeConfig config = wl_home_config_default();
    char text[32];

    config.notice_no_extension = "A NOTICE WITH MORE WORDS THAN THE BUFFER HAS ROOM FOR {port}";
    start();
    wl_home_menu_init(&rig.home, &config);
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(60);
    ASSERT(fake.visible);
    memset(text, 'x', sizeof text);
    ASSERT_EQ(0u, wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof text));
    ASSERT_EQ(0, text[0]);
    ASSERT_EQ(0u, wl_home_menu_notice_text(&rig.home, rig.now, text, 0));
    PASS();
}

TEST the_default_words_are_in_the_font(void)
{
    WlHomeConfig config = wl_home_config_default();

    ASSERT(wl_overlay_text_drawable(config.title));
    ASSERT(wl_overlay_text_drawable(config.resume_label));
    ASSERT(wl_overlay_text_drawable(config.loader_label));
    ASSERT(wl_overlay_text_drawable(config.confirm_text));
    ASSERT(wl_overlay_text_drawable(config.cancel_label));
    ASSERT(wl_overlay_text_drawable(config.batteries_heading));
    ASSERT(wl_overlay_text_drawable(config.hint_menu));
    ASSERT(wl_overlay_text_drawable(config.hint_confirm));
    {
        /* (the templates' braces are put right before a text is drawn) */
        char text[256];

        start();
        rig.raw.remotes[0].extension = WL_EXT_NONE;
        rig.raw.remotes[0].battery = 5;
        looks(100);
        ASSERT(wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof text) > 0);
        ASSERTm("the default notices, with their numbers put in", wl_overlay_text_drawable(text));
        ASSERT(strstr(text, "Battery low (5%)") != NULL && strstr(text, "{") == NULL);
    }
    PASS();
}

/* ---------- the drawing */

static WlHostGxDraw g_draws[64];

static unsigned quads_drawn(void)
{
    unsigned quads = 0;

    for (uint32_t i = 0; i < wl_host_gx_recorded() && i < 64u; i++)
        quads += g_draws[i].count / 4u;
    return quads;
}

static void draw_begin(void)
{
    (void)wl_gx_init();
    memset(g_draws, 0, sizeof g_draws);
    wl_host_gx_record(g_draws, 64);
    wl_overlay_begin();
}

static unsigned draw_end(void)
{
    unsigned quads;

    wl_overlay_end();
    quads = quads_drawn();
    wl_host_gx_record(NULL, 0);
    wl_gx_shutdown();
    return quads;
}

TEST a_closed_menu_and_no_notice_draw_nothing(void)
{
    start();
    looks(5);
    draw_begin();
    wl_home_menu_draw(&rig.home, 640, 480);
    wl_home_menu_draw_notices(&rig.home, 640, rig.now);
    ASSERT_EQ(0u, draw_end());
    PASS();
}

TEST the_menu_and_the_confirmation_and_the_notices_draw_their_words(void)
{
    unsigned menu, confirm, notices;

    start();
    open_menu();
    draw_begin();
    wl_home_menu_draw(&rig.home, 640, 480);
    menu = draw_end();
    ASSERTm("the panel, the heading, three rows, a battery and the hint", menu > 300u);

    press_remote(WL_WM_UP); /* (up from Resume is the loader) */
    press_remote(WL_WM_A);
    ASSERT_EQ(WL_HOME_CONFIRM, rig.home.state);
    draw_begin();
    wl_home_menu_draw(&rig.home, 640, 480);
    confirm = draw_end();
    ASSERTm("the confirmation is drawn too", confirm > 300u && confirm != menu);

    start();
    rig.raw.remotes[0].extension = WL_EXT_NONE;
    looks(60);
    ASSERT(fake.visible);
    draw_begin();
    wl_home_menu_draw(&rig.home, 640, 480);
    ASSERT_EQ(0u, quads_drawn()); /* (no menu is up) */
    wl_home_menu_draw_notices(&rig.home, 640, rig.now);
    notices = draw_end();
    ASSERTm("a notice is a box and its words", notices > 50u);
    PASS();
}

SUITE(home_menu_suite)
{
    RUN_TEST(check_closed_costs_nothing);
    RUN_TEST(check_open_and_close);
    RUN_TEST(check_back);
    RUN_TEST(check_navigation);
    RUN_TEST(check_items);
    RUN_TEST(check_pause_rules);
    RUN_TEST(check_swallow);
    RUN_TEST(check_home_variants);
    RUN_TEST(check_menu_batteries);
    RUN_TEST(check_no_extension_notice);
    RUN_TEST(check_battery_notice);
    RUN_TEST(check_not_polled_every_frame);
    RUN_TEST(a_menu_with_no_reset_item_has_only_resume_and_the_loader);
    RUN_TEST(the_classic_controllers_buttons_go_either_way);
    RUN_TEST(the_stick_threshold_is_the_configurations);
    RUN_TEST(the_texts_and_times_are_the_configurations);
    RUN_TEST(a_text_too_long_for_the_buffer_is_left_out_not_overrun);
    RUN_TEST(the_default_words_are_in_the_font);
    RUN_TEST(a_closed_menu_and_no_notice_draw_nothing);
    RUN_TEST(the_menu_and_the_confirmation_and_the_notices_draw_their_words);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    g_config = wl_home_config_default();
    g_config.reset_label = "Reset to title";
    g_config.classic_swap = true;
    RUN_SUITE(home_menu_suite);
    GREATEST_MAIN_END();
}
