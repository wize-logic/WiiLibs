// SPDX-License-Identifier: MIT
// The wrist strap reminder's state machine (wl/platform/strap_screen.h) against fake controllers and a
// clock. It shows at the first look and only when enabled; it ignores the controllers for the first
// second, and an A held from before (the loader's) or through it does not go on; a fresh press of A
// after the second goes on, whichever controller gives it (a Remote's A, a Classic Controller's
// button, a GameCube controller's A, a script's); other buttons do nothing; the program gets no
// controller while it is up and until the A that went on is let go (or 1.5 s); it shows once; and the
// line for a Nunchuk appears only when one is connected.
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/home_menu.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/overlay.h"
#include "wl/platform/strap_screen.h"

// (a game that swaps the Classic Controller's buttons)
static WlStrapConfig g_config;

#define START_US 5000000u

static WlRawInput none(void)
{
    WlRawInput raw;

    memset(&raw, 0, sizeof(raw));
    return raw;
}

static void remote(WlRawInput *raw, int port, unsigned char extension, uint32_t buttons)
{
    raw->remotes[port].connected = true;
    raw->remotes[port].extension = extension;
    raw->remotes[port].buttons = buttons;
}

TEST check_off(void)
{
    WlStrapScreen strap;
    WlRawInput raw = none();

    wl_strap_screen_init(&strap, &g_config, false);
    remote(&raw, 0, WL_EXT_NONE, WL_WM_A);
    ASSERTm("off: never shows", !wl_strap_screen_step(&strap, &raw, 0, START_US));
    ASSERTm("off: still never", !wl_strap_screen_step(&strap, &raw, 0, START_US + 3000000u));
    ASSERTm("off: the game has its controllers", !wl_strap_screen_blocks_input(&strap));
    PASS();
}

TEST check_timing(void)
{
    WlStrapScreen strap;
    WlRawInput idle = none();
    WlRawInput press = none();

    wl_strap_screen_init(&strap, &g_config, true);
    ASSERTm("before the first look nothing is up", !wl_strap_screen_up(&strap));
    remote(&idle, 0, WL_EXT_NONE, 0);
    remote(&press, 0, WL_EXT_NONE, WL_WM_A);

    ASSERTm("up at the first look", wl_strap_screen_step(&strap, &idle, 0, START_US));
    ASSERTm("the game sees no controller", wl_strap_screen_blocks_input(&strap));
    ASSERTm("not ready at once", !wl_strap_screen_ready(&strap, START_US + 10000u));
    /* a press in the first second does nothing, even a fresh one */
    ASSERTm("a press at 0.2 s: still up", wl_strap_screen_step(&strap, &press, 0, START_US + 200000u));
    ASSERTm("released", wl_strap_screen_step(&strap, &idle, 0, START_US + 400000u));
    ASSERTm("a press just before the second: still up", wl_strap_screen_step(&strap, &press, 0, START_US + g_config.ignore_us - 1u));
    ASSERTm("released after it", wl_strap_screen_step(&strap, &idle, 0, START_US + g_config.ignore_us - 1u + 1000u));
    ASSERTm("ready at the second", wl_strap_screen_ready(&strap, START_US + g_config.ignore_us));
    /* a press after it goes on, and the game is kept from the A until it is let go */
    ASSERTm("a fresh press after the second goes on", !wl_strap_screen_step(&strap, &press, 0, START_US + g_config.ignore_us + 50000u));
    ASSERTm("and the screen is gone", !wl_strap_screen_up(&strap));
    ASSERTm("but the A is kept from the game", wl_strap_screen_blocks_input(&strap));
    ASSERTm("held on", !wl_strap_screen_step(&strap, &press, 0, START_US + g_config.ignore_us + 500000u));
    ASSERTm("still kept", wl_strap_screen_blocks_input(&strap));
    ASSERTm("let go", !wl_strap_screen_step(&strap, &idle, 0, START_US + g_config.ignore_us + 600000u));
    ASSERTm("the game has its controllers again", !wl_strap_screen_blocks_input(&strap));
    /* once */
    ASSERTm("it does not come back", !wl_strap_screen_step(&strap, &press, 0, START_US + 9000000u));
    ASSERTm("and the game keeps its controllers", !wl_strap_screen_blocks_input(&strap));
    PASS();
}

TEST check_held_a(void)
{
    WlStrapScreen strap;
    WlRawInput held = none();
    WlRawInput idle = none();

    remote(&held, 0, WL_EXT_NONE, WL_WM_A);
    remote(&idle, 0, WL_EXT_NONE, 0);

    /* an A held from the loader, kept down through the second: it does not go on */
    wl_strap_screen_init(&strap, &g_config, true);
    ASSERTm("A held at the start: up", wl_strap_screen_step(&strap, &held, 0, START_US));
    ASSERTm("and still up with it held past the second", wl_strap_screen_step(&strap, &held, 0, START_US + 1500000u));
    ASSERTm("and long after", wl_strap_screen_step(&strap, &held, 0, START_US + 5000000u));
    ASSERTm("let go: still up", wl_strap_screen_step(&strap, &idle, 0, START_US + 5100000u));
    ASSERTm("then a press goes on", !wl_strap_screen_step(&strap, &held, 0, START_US + 5200000u));

    /* the A that went on, held for good, is kept from the game only for 1.5 s */
    ASSERTm("kept", wl_strap_screen_blocks_input(&strap));
    ASSERTm("held: gone after the limit", !wl_strap_screen_step(&strap, &held, 0, START_US + 5200000u + g_config.swallow_us));
    ASSERTm("the game has its controllers after the limit", !wl_strap_screen_blocks_input(&strap));
    PASS();
}

TEST check_controllers(void)
{
    WlStrapScreen strap;
    WlRawInput idle = none();
    WlRawInput raw;
    int which;

    remote(&idle, 0, WL_EXT_NONE, 0);
    for (which = 0; which < 8; which++)
    {
        bool accepts = true;

        raw = none();
        switch (which)
        {
        case 0:
            remote(&raw, 0, WL_EXT_NONE, WL_WM_A);
            break;
        case 1:
            remote(&raw, 1, WL_EXT_NUNCHUK, WL_WM_A);
            break;
        case 2:
            remote(&raw, 0, WL_EXT_CLASSIC, WL_CC_B);
            break;
        case 3:
            raw.pads[2].connected = true;
            raw.pads[2].buttons = WL_PAD_A;
            break;
        case 4:
            /* the buttons that are not A: B, 1, 2, Z, C, plus, the D-pad, HOME, and a Classic a */
            remote(&raw, 0, WL_EXT_NUNCHUK, WL_WM_B | WL_WM_1 | WL_WM_2 | WL_WM_PLUS | WL_NC_Z | WL_NC_C);
            accepts = false;
            break;
        case 5:
            remote(&raw, 0, WL_EXT_NONE, WL_WM_LEFT | WL_WM_RIGHT | WL_WM_HOME);
            accepts = false;
            break;
        case 6:
            remote(&raw, 0, WL_EXT_CLASSIC, WL_CC_A | WL_CC_X | WL_CC_Y | WL_CC_ZL);
            accepts = false;
            break;
        default:
            raw.pads[0].connected = true;
            raw.pads[0].buttons = WL_PAD_B | WL_PAD_X | WL_PAD_START;
            accepts = false;
            break;
        }
        wl_strap_screen_init(&strap, &g_config, true);
        wl_strap_screen_step(&strap, &idle, 0, START_US);
        wl_strap_screen_step(&strap, &idle, 0, START_US + 1100000u);
        wl_strap_screen_step(&strap, &raw, 0, START_US + 1200000u);
        ASSERTm("controller case : ", wl_strap_screen_up(&strap) == !accepts);
    }
    /* the scripted controller's A */
    wl_strap_screen_init(&strap, &g_config, true);
    wl_strap_screen_step(&strap, &idle, 0, START_US);
    wl_strap_screen_step(&strap, &idle, 0, START_US + 1100000u);
    wl_strap_screen_step(&strap, &idle, WL_HOME_NAV_ACCEPT, START_US + 1200000u);
    ASSERTm("a script's A goes on", !wl_strap_screen_up(&strap));
    PASS();
}

TEST check_words(void)
{
    WlStrapScreen strap;
    WlRawInput raw = none();
    int count;

    wl_strap_screen_init(&strap, &g_config, true);
    remote(&raw, 0, WL_EXT_NONE, 0);
    wl_strap_screen_step(&strap, &raw, 0, START_US);
    ASSERTm("the title names the strap", strstr(wl_strap_screen_title(&strap), "WRIST STRAP") != NULL);
    for (count = 0; wl_strap_screen_line(&strap, count) != NULL; count++) {
    }
    ASSERT_EQ(3, count); /* the strap twice, the room: no Nunchuk line */
    remote(&raw, 0, WL_EXT_NUNCHUK, 0);
    wl_strap_screen_step(&strap, &raw, 0, START_US + 100000u);
    for (count = 0; wl_strap_screen_line(&strap, count) != NULL; count++) {
    }
    ASSERT_EQ(4, count);
    ASSERTm("a line for a Nunchuk when one is connected", strstr(wl_strap_screen_line(&strap, 2), "NUNCHUK") != NULL);
    ASSERTm("and the room after it", strstr(wl_strap_screen_line(&strap, 3), "ROOM") != NULL);
    remote(&raw, 0, WL_EXT_CLASSIC, 0);
    wl_strap_screen_step(&strap, &raw, 0, START_US + 200000u);
    ASSERTm("and not once it is a Classic Controller", strstr(wl_strap_screen_line(&strap, 2), "ROOM") != NULL);
    ASSERTm("nothing past the end", wl_strap_screen_line(&strap, 3) == NULL);
    ASSERTm("the prompt", strstr(wl_strap_screen_prompt(&strap), "CONTINUE") != NULL);
    PASS();
}

TEST the_default_words_are_in_the_font(void)
{
    WlStrapConfig config = wl_strap_config_default();

    ASSERT(wl_overlay_text_drawable(config.title));
    ASSERT(wl_overlay_text_drawable(config.prompt));
    for (int i = 0; i < WL_STRAP_MAX_LINES && config.body[i].text != NULL; i++) {
        ASSERTm(config.body[i].text, wl_overlay_text_drawable(config.body[i].text));
    }
    ASSERT(!wl_overlay_text_drawable("A line with a \x01 box in it"));
    PASS();
}

TEST the_configuration_sets_the_words_the_times_and_the_buttons(void)
{
    WlStrapConfig config = wl_strap_config_default();
    WlStrapScreen strap;
    WlRawInput idle = none();
    WlRawInput classic_a = none();
    WlRawInput classic_b = none();
    int count;

    /* the Wii's own Classic Controller: a accepts */
    remote(&idle, 0, WL_EXT_CLASSIC, 0);
    remote(&classic_a, 0, WL_EXT_CLASSIC, WL_CC_A);
    remote(&classic_b, 0, WL_EXT_CLASSIC, WL_CC_B);
    config.classic_swap = false;
    wl_strap_screen_init(&strap, &config, true);
    wl_strap_screen_step(&strap, &idle, 0, START_US);
    wl_strap_screen_step(&strap, &classic_b, 0, START_US + 1100000u);
    ASSERTm("without the swap the Classic Controller's b does not go on", wl_strap_screen_up(&strap));
    wl_strap_screen_step(&strap, &idle, 0, START_US + 1150000u);
    wl_strap_screen_step(&strap, &classic_a, 0, START_US + 1200000u);
    ASSERTm("and its a does", !wl_strap_screen_up(&strap));

    /* two lines of its own, a shorter wait and a shorter hold */
    memset(&config, 0, sizeof config);
    config.title = "STRAP";
    config.body[0].text = "ONE";
    config.body[1].text = "TWO";
    config.prompt = "GO";
    config.ignore_us = 200000u;
    config.swallow_us = 300000u;
    wl_strap_screen_init(&strap, &config, true);
    wl_strap_screen_step(&strap, &idle, 0, START_US);
    ASSERTm("not ready before its own wait", !wl_strap_screen_ready(&strap, START_US + 199999u));
    ASSERTm("ready after it", wl_strap_screen_ready(&strap, START_US + 200000u));
    ASSERT_STR_EQ("STRAP", wl_strap_screen_title(&strap));
    ASSERT_STR_EQ("GO", wl_strap_screen_prompt(&strap));
    for (count = 0; wl_strap_screen_line(&strap, count) != NULL; count++) {
    }
    ASSERT_EQ(2, count);
    wl_strap_screen_step(&strap, &classic_a, 0, START_US + 250000u);
    ASSERTm("goes on at its own wait", !wl_strap_screen_up(&strap));
    wl_strap_screen_step(&strap, &classic_a, 0, START_US + 250000u + 299999u);
    ASSERTm("kept from the program for its own hold", wl_strap_screen_blocks_input(&strap));
    wl_strap_screen_step(&strap, &classic_a, 0, START_US + 250000u + 300000u);
    ASSERTm("and no longer", !wl_strap_screen_blocks_input(&strap));

    /* no title or prompt given: the defaults */
    memset(&config, 0, sizeof config);
    wl_strap_screen_init(&strap, &config, true);
    ASSERT(strstr(wl_strap_screen_title(&strap), "WRIST STRAP") != NULL);
    ASSERT(wl_strap_screen_line(&strap, 0) == NULL);
    PASS();
}

static unsigned g_quads;

TEST it_draws_its_words_and_lights_the_prompt_when_ready(void)
{
    WlStrapScreen strap;
    WlRawInput raw = none();
    WlHostGxDraw draws[64];

    wl_strap_screen_init(&strap, &g_config, true);
    remote(&raw, 0, WL_EXT_NONE, 0);
    wl_strap_screen_step(&strap, &raw, 0, START_US);
    (void)wl_gx_init();
    memset(draws, 0, sizeof draws);
    wl_host_gx_record(draws, 64);
    wl_overlay_begin();
    wl_strap_screen_draw(&strap, 640, 480, START_US + 10000u);
    wl_overlay_end();
    g_quads = 0;
    for (uint32_t i = 0; i < wl_host_gx_recorded() && i < 64u; i++) {
        g_quads += draws[i].count / 4u;
    }
    ASSERTm("the black and the words come out as quads", g_quads > 100u);
    {
        unsigned before = g_quads;

        memset(draws, 0, sizeof draws);
        wl_host_gx_record(draws, 64);
        wl_overlay_begin();
        wl_strap_screen_draw(&strap, 640, 480, START_US + 2000000u);
        wl_overlay_end();
        g_quads = 0;
        for (uint32_t i = 0; i < wl_host_gx_recorded() && i < 64u; i++) {
            g_quads += draws[i].count / 4u;
        }
        ASSERT_EQ(before, g_quads); /* (the same words in another colour) */
    }
    wl_host_gx_record(NULL, 0);
    wl_gx_shutdown();
    PASS();
}

static void setup(void *arg)
{
    (void)arg;
    g_config = wl_strap_config_default();
    g_config.classic_swap = true;
}

SUITE(strap_screen_suite)
{
    GREATEST_SET_SETUP_CB(setup, NULL);
    RUN_TEST(check_off);
    RUN_TEST(check_timing);
    RUN_TEST(check_held_a);
    RUN_TEST(check_controllers);
    RUN_TEST(check_words);
    RUN_TEST(the_default_words_are_in_the_font);
    RUN_TEST(the_configuration_sets_the_words_the_times_and_the_buttons);
    RUN_TEST(it_draws_its_words_and_lights_the_prompt_when_ready);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(strap_screen_suite);
    GREATEST_MAIN_END();
}
