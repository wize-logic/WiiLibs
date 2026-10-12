// SPDX-License-Identifier: MIT
// Wii text console and stop screen (stop.h). The console is libogc's, set up in the video's back
// buffer; the stop waits for HOME on a Remote or START on a GameCube controller. Only this
// directory may include <ogc/*.h>.
#include <gccore.h>
#include <stdio.h>

#include "wl/platform/platform.h"
#include "wl/platform/stop.h"
#include "wl/platform/stop_backend.h"

// the console's framebuffer: one of the video's, which the program's frames cover once they start
static void *g_console_fb;

WlResult wl_platform_console_start(void) {
    if (g_console_fb != NULL) {
        return WL_OK;
    }
    WlResult video = wl_platform_video_init();
    if (video != WL_OK) {
        return video;
    }
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    void *fb = wl_platform_video_back_buffer();
    if (mode == NULL || fb == NULL) {
        return WL_ERR_STATE;
    }
    console_init(fb, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    g_console_fb = fb;
    VIDEO_SetNextFramebuffer(fb);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    return WL_OK;
}

void *wl_platform_console_framebuffer(void) {
    return g_console_fb;
}

static void wait_a_frame(void) {
    if (g_console_fb != NULL) {
        VIDEO_WaitVSync();
    } else {
        wl_platform_thread_sleep_ms(16);
    }
}

void wl_platform_stop_wait(const char *message, bool again) {
    (void)message;
    if (again) {
        for (;;) {
            wait_a_frame();
        }
    }
    fputs("Press HOME (or START) to return to the loader.\n", stdout);
    // the console's framebuffer on the screen again, if the program's frames had taken its place:
    // after the last frame presented, which the retrace interrupt would otherwise put on the
    // screen over it
    if (g_console_fb != NULL) {
        wl_platform_video_wait_shown();
        VIDEO_SetNextFramebuffer(g_console_fb);
        VIDEO_Flush();
    }
    for (;;) {
        WlRawInput input;
        bool leave = false;
        wl_platform_input_poll(&input);
        for (uint32_t i = 0; i < WL_INPUT_MAX_REMOTES; i++) {
            leave |= input.remotes[i].connected && (input.remotes[i].buttons & WL_WM_HOME) != 0u;
        }
        for (uint32_t i = 0; i < WL_INPUT_MAX_PADS; i++) {
            leave |= input.pads[i].connected && (input.pads[i].buttons & WL_PAD_START) != 0u;
        }
        if (leave || wl_platform_system_events() != 0u) {
            wl_platform_exit(WL_EXIT_LOADER);
        }
        wait_a_frame();
    }
}
