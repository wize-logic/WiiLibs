// SPDX-License-Identifier: MIT
// The video mode the console's settings ask for (wl/platform/video_mode.h), over every combination
// of the settings: the mode (480i/480p, NTSC, EuRGB60, PAL 50), the line a 16:9 TV scans, the
// display position, the pillarbox's place in the framebuffer, and the retraces a frame stays for a
// rate. A pure function: also run on the big-endian build.
#include <string.h>

#include "greatest.h"
#include "wl/platform/video_mode.h"

static WlVideoChoice choose(WlTvStandard tv, bool prog, bool cable, bool eurgb60, bool wide, int offset, WlWidescreen w) {
    WlVideoSettings s = {tv, prog, cable, eurgb60, wide, offset, w};
    WlVideoChoice c;
    wl_video_choose(&s, &c);
    return c;
}

TEST progressive_needs_the_setting_and_the_cable(void) {
    for (int prog = 0; prog < 2; prog++) {
        for (int cable = 0; cable < 2; cable++) {
            WlVideoChoice c = choose(WL_TV_NTSC, prog, cable, false, false, 0, WL_WIDESCREEN_PILLARBOX);
            ASSERT_EQ(prog && cable, c.progressive);
            ASSERT_EQ(WL_VIDEO_NTSC, c.system);
            ASSERT_EQ(480u, c.xfb_height);
            ASSERT_EQ(59940u, c.refresh_mhz);
        }
    }
    PASS();
}

TEST the_picture_is_always_640_by_480_of_the_efb(void) {
    for (int tv = 0; tv < 3; tv++) {
        for (int e = 0; e < 2; e++) {
            for (int wide = 0; wide < 2; wide++) {
                WlVideoChoice c = choose((WlTvStandard)tv, e, e, e, wide, 0, WL_WIDESCREEN_PILLARBOX);
                ASSERT_EQ(640u, c.fb_width);
                ASSERT_EQ(480u, c.efb_height); // (a PAL 50 mode copies 480 rows, never the scratch rows below)
            }
        }
    }
    PASS();
}

TEST pal_is_eurgb60_when_the_setting_allows_else_50_hertz(void) {
    WlVideoChoice c = choose(WL_TV_PAL, false, false, true, false, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT_EQ(WL_VIDEO_EURGB60, c.system);
    ASSERT_EQ(480u, c.xfb_height);
    ASSERT_EQ(59940u, c.refresh_mhz);
    c = choose(WL_TV_PAL, false, false, false, false, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT_EQ(WL_VIDEO_PAL50, c.system);
    ASSERT_EQ(576u, c.xfb_height); // 480 EFB rows scaled to the 576 lines
    ASSERT_EQ(50000u, c.refresh_mhz);
    c = choose(WL_TV_PAL, true, true, false, false, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT_EQ(WL_VIDEO_PAL50, c.system);
    ASSERT(c.progressive); // 576p at 50 Hz
    c = choose(WL_TV_MPAL, false, false, true, false, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT_EQ(WL_VIDEO_MPAL, c.system); // (the EuRGB60 setting is a PAL console's)
    ASSERT_EQ(59940u, c.refresh_mhz);
    PASS();
}

TEST a_wide_tv_scans_704_of_the_line_and_a_4_3_tv_640(void) {
    WlVideoChoice c = choose(WL_TV_NTSC, false, false, false, false, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT_EQ(640u, c.vi_width);
    ASSERT_EQ(40u, c.vi_x_origin);
    ASSERT(!c.pillarbox);
    ASSERT_EQ(0u, c.picture_x);
    ASSERT_EQ(640u, c.picture_width);
    c = choose(WL_TV_NTSC, false, false, false, true, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT_EQ(704u, c.vi_width);
    ASSERT_EQ(8u, c.vi_x_origin);
    PASS();
}

TEST pillarbox_is_the_middle_three_quarters_for_a_wide_setting_only(void) {
    WlVideoChoice c = choose(WL_TV_NTSC, false, false, false, true, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT(c.pillarbox);
    ASSERT_EQ(480u, c.picture_width); // 4:3 of the 16:9 line: 3/4 of its width
    ASSERT_EQ(80u, c.picture_x);
    ASSERT_EQ(c.fb_width, 2u * c.picture_x + c.picture_width); // centred
    ASSERT_EQ(0u, c.picture_x % 2u);
    ASSERT_EQ(0u, c.picture_width % 2u);
    // the 4:3 picture's width over the line's, as the TV shows it, is 3/4 of the screen
    ASSERT_EQ(3u * c.fb_width, 4u * c.picture_width);

    c = choose(WL_TV_NTSC, false, false, false, true, 0, WL_WIDESCREEN_STRETCH);
    ASSERT(!c.pillarbox); // stretched as before
    ASSERT_EQ(640u, c.picture_width);
    ASSERT_EQ(704u, c.vi_width);
    c = choose(WL_TV_NTSC, false, false, false, false, 0, WL_WIDESCREEN_PILLARBOX);
    ASSERT(!c.pillarbox); // a 4:3 setting needs none
    PASS();
}

TEST the_display_position_is_kept_on_the_line(void) {
    // 4:3: the origin 40 may move 40 either way; a 16:9 line of 704, 8
    ASSERT_EQ(3, choose(WL_TV_NTSC, false, false, false, false, 3, WL_WIDESCREEN_PILLARBOX).offset_h);
    ASSERT_EQ(-5, choose(WL_TV_NTSC, false, false, false, false, -5, WL_WIDESCREEN_PILLARBOX).offset_h);
    ASSERT_EQ(40, choose(WL_TV_NTSC, false, false, false, false, 99, WL_WIDESCREEN_PILLARBOX).offset_h);
    ASSERT_EQ(-40, choose(WL_TV_NTSC, false, false, false, false, -99, WL_WIDESCREEN_PILLARBOX).offset_h);
    ASSERT_EQ(8, choose(WL_TV_PAL, false, false, false, true, 99, WL_WIDESCREEN_PILLARBOX).offset_h);
    ASSERT_EQ(-8, choose(WL_TV_PAL, false, false, false, true, -99, WL_WIDESCREEN_PILLARBOX).offset_h);
    ASSERT_EQ(6, choose(WL_TV_PAL, false, false, false, true, 6, WL_WIDESCREEN_STRETCH).offset_h);
    for (int offset = -60; offset <= 60; offset++) {
        for (int wide = 0; wide < 2; wide++) {
            WlVideoChoice c = choose(WL_TV_NTSC, false, false, false, wide, offset, WL_WIDESCREEN_PILLARBOX);
            int pos = (int)c.vi_x_origin + c.offset_h;
            ASSERT(pos >= 0 && pos + (int)c.vi_width <= 720);
        }
    }
    PASS();
}

TEST the_rate_is_a_ratio_of_retraces_to_a_frame(void) {
    uint32_t n, d;
    wl_video_rate_ratio(59940, 30, &n, &d);
    ASSERT(n == 2u && d == 1u);
    wl_video_rate_ratio(50000, 30, &n, &d);
    ASSERT(n == 5u && d == 3u); // two retraces, two, one: 30 frames in 50 retraces
    wl_video_rate_ratio(50000, 25, &n, &d);
    ASSERT(n == 2u && d == 1u);
    wl_video_rate_ratio(59940, 60, &n, &d);
    ASSERT(n == 1u && d == 1u);
    wl_video_rate_ratio(50000, 60, &n, &d);
    ASSERT(n == 1u && d == 1u); // (not faster than the display)
    wl_video_rate_ratio(59940, 0, &n, &d);
    ASSERT(n == 1u && d == 1u);
    wl_video_rate_ratio(59940, 24, &n, &d);
    ASSERT(n == 5u && d == 2u);
    PASS();
}

TEST the_description_says_the_mode(void) {
    char text[200];
    WlVideoChoice c = choose(WL_TV_NTSC, true, true, false, true, 0, WL_WIDESCREEN_PILLARBOX);
    wl_video_describe(&c, text, sizeof text);
    ASSERT(strstr(text, "480p NTSC 59.94 Hz") != NULL);
    ASSERT(strstr(text, "16:9") != NULL);
    ASSERT(strstr(text, "80..560") != NULL);
    c = choose(WL_TV_PAL, false, false, false, false, 0, WL_WIDESCREEN_PILLARBOX);
    wl_video_describe(&c, text, sizeof text);
    ASSERT(strstr(text, "576i PAL 50 50.00 Hz") != NULL);
    ASSERT(strstr(text, "EFB 640x480") != NULL);
    char tiny[8];
    wl_video_describe(&c, tiny, sizeof tiny); // (cut, terminated)
    ASSERT_EQ(7u, strlen(tiny));
    PASS();
}

SUITE(video_mode_suite) {
    RUN_TEST(progressive_needs_the_setting_and_the_cable);
    RUN_TEST(the_picture_is_always_640_by_480_of_the_efb);
    RUN_TEST(pal_is_eurgb60_when_the_setting_allows_else_50_hertz);
    RUN_TEST(a_wide_tv_scans_704_of_the_line_and_a_4_3_tv_640);
    RUN_TEST(pillarbox_is_the_middle_three_quarters_for_a_wide_setting_only);
    RUN_TEST(the_display_position_is_kept_on_the_line);
    RUN_TEST(the_rate_is_a_ratio_of_retraces_to_a_frame);
    RUN_TEST(the_description_says_the_mode);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(video_mode_suite);
    GREATEST_MAIN_END();
}
