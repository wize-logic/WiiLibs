// SPDX-License-Identifier: MIT
// Wii input backend: WPAD (buttons, IR, accelerometer, Nunchuk, Classic), PAD
// (GameCube ports) and the USB keyboard (libwiikeyboard) polled into WlRawInput once per frame.
// A Wii Remote reports about every 5 ms, so each poll reads every report libogc has queued for it
// (input_reports.h) rather than only the latest. Mapping to actions belongs to the layer above; this
// file only reads devices and never blocks.
// Only this directory may include <ogc/*.h>.
#include <math.h>
#include <ogc/conf.h>
#include <ogc/pad.h>
#include <string.h>
#include <wiikeyboard/keyboard.h>
#include <wiiuse/wpad.h>

#include "wl/platform/input_reports.h"
#include "wl/platform/input_shape.h"
#include "wl/platform/platform.h"

// platform.h button words are libogc's, copied unchanged: prove it at build time.
WL_STATIC_ASSERT(WL_WM_2 == WPAD_BUTTON_2 && WL_WM_1 == WPAD_BUTTON_1 && WL_WM_B == WPAD_BUTTON_B &&
                     WL_WM_A == WPAD_BUTTON_A && WL_WM_MINUS == WPAD_BUTTON_MINUS &&
                     WL_WM_HOME == WPAD_BUTTON_HOME && WL_WM_LEFT == WPAD_BUTTON_LEFT &&
                     WL_WM_RIGHT == WPAD_BUTTON_RIGHT && WL_WM_DOWN == WPAD_BUTTON_DOWN &&
                     WL_WM_UP == WPAD_BUTTON_UP && WL_WM_PLUS == WPAD_BUTTON_PLUS,
                 "remote button bits");
WL_STATIC_ASSERT(WL_NC_Z == WPAD_NUNCHUK_BUTTON_Z && WL_NC_C == WPAD_NUNCHUK_BUTTON_C,
                 "nunchuk button bits");
WL_STATIC_ASSERT(WL_CC_UP == WPAD_CLASSIC_BUTTON_UP && WL_CC_LEFT == WPAD_CLASSIC_BUTTON_LEFT &&
                     WL_CC_ZR == WPAD_CLASSIC_BUTTON_ZR && WL_CC_X == WPAD_CLASSIC_BUTTON_X &&
                     WL_CC_A == WPAD_CLASSIC_BUTTON_A && WL_CC_Y == WPAD_CLASSIC_BUTTON_Y &&
                     WL_CC_B == WPAD_CLASSIC_BUTTON_B && WL_CC_ZL == WPAD_CLASSIC_BUTTON_ZL &&
                     WL_CC_R == WPAD_CLASSIC_BUTTON_FULL_R && WL_CC_PLUS == WPAD_CLASSIC_BUTTON_PLUS &&
                     WL_CC_HOME == WPAD_CLASSIC_BUTTON_HOME &&
                     WL_CC_MINUS == WPAD_CLASSIC_BUTTON_MINUS &&
                     WL_CC_L == WPAD_CLASSIC_BUTTON_FULL_L && WL_CC_DOWN == WPAD_CLASSIC_BUTTON_DOWN &&
                     WL_CC_RIGHT == WPAD_CLASSIC_BUTTON_RIGHT,
                 "classic button bits");
WL_STATIC_ASSERT(WL_PAD_LEFT == PAD_BUTTON_LEFT && WL_PAD_RIGHT == PAD_BUTTON_RIGHT &&
                     WL_PAD_DOWN == PAD_BUTTON_DOWN && WL_PAD_UP == PAD_BUTTON_UP &&
                     WL_PAD_Z == PAD_TRIGGER_Z && WL_PAD_R == PAD_TRIGGER_R &&
                     WL_PAD_L == PAD_TRIGGER_L && WL_PAD_A == PAD_BUTTON_A &&
                     WL_PAD_B == PAD_BUTTON_B && WL_PAD_X == PAD_BUTTON_X &&
                     WL_PAD_Y == PAD_BUTTON_Y && WL_PAD_START == PAD_BUTTON_START,
                 "pad button bits");
WL_STATIC_ASSERT(WL_INPUT_MAX_REMOTES <= WPAD_MAX_WIIMOTES && WL_INPUT_MAX_PADS <= PAD_CHANMAX,
                 "channel counts");

#define IR_VRES_W 640u
#define IR_VRES_H 480u
// The Wii Remote status report carries the battery as 0..200 (0xC8 = full scale); libogc hands
// the byte on unchanged. Hardware check: log it on a fresh and a weak battery (input suite).
#define BATTERY_FULL_CODE 200u

static bool g_init;
static bool g_kb_init;
static bool g_kb_connected;
static bool g_remote_was[WL_INPUT_MAX_REMOTES];
static uint8_t g_remote_extension[WL_INPUT_MAX_REMOTES];
static uint16_t g_remote_not_ready[WL_INPUT_MAX_REMOTES]; // consecutive polls without data
// How long (in polls) a connected Remote may go without data before it counts as gone.
#define REMOTE_GRACE_POLLS 120u
static bool g_pad_was[WL_INPUT_MAX_PADS];
// A GameCube pad that answers with a momentary error (PAD_ERR_TRANSFER, PAD_ERR_NOT_READY) keeps
// its last good state: zeroing it would release a held trigger for a frame. After
// PAD_ERROR_GRACE_POLLS errors in a row it counts as gone.
#define PAD_ERROR_GRACE_POLLS 10u
static WlRawPad g_pad_last[WL_INPUT_MAX_PADS];
static uint8_t g_pad_errors[WL_INPUT_MAX_PADS];
// The reports of the Remote's frame so far (filled by on_report during the poll).
static WlRemoteReports g_reports[WL_INPUT_MAX_REMOTES];
static bool g_pad_rumble[WL_INPUT_MAX_PADS];
static bool g_remote_rumble[WL_INPUT_MAX_REMOTES]; // what the Remote was last told
static int g_battery_logged[WL_INPUT_MAX_REMOTES] = {-1, -1, -1, -1};

WlResult wl_platform_input_init(void) {
    if (g_init) {
        return WL_OK;
    }
    (void)WPAD_Init(); // idempotent; other code may also call it
    (void)WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
    (void)WPAD_SetVRes(WPAD_CHAN_ALL, IR_VRES_W, IR_VRES_H);
    (void)PAD_Init();
    // The USB keyboard stack starts its own threads. A failure (no USB, IOS without it) only
    // means no keyboard: everything else keeps working.
    g_kb_init = KEYBOARD_Init(NULL) >= 0;
    if (!g_kb_init) {
        WL_LOG_W("input", "USB keyboard stack did not start; keyboard disabled");
    }
    g_init = true;
    return WL_OK;
}

void wl_platform_input_shutdown(void) {
    if (!g_init) {
        return;
    }
    wl_platform_input_stop_rumble();
    if (g_kb_init) {
        (void)KEYBOARD_Deinit();
        g_kb_init = false;
    }
    g_kb_connected = false;
    memset(g_remote_was, 0, sizeof g_remote_was);
    memset(g_pad_was, 0, sizeof g_pad_was);
    g_init = false;
}

static inline int16_t sat16(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)v;
}

// libogc reports a stick as magnitude 0..1 and an angle in degrees from +y, clockwise.
static void js_to_xy(const struct joystick_t *js, int8_t out[2]) {
    float rad = js->ang * (3.14159265f / 180.0f);
    float m = js->mag < 0.0f ? 0.0f : (js->mag > 1.0f ? 1.0f : js->mag);
    out[0] = (int8_t)lroundf(m * sinf(rad) * 127.0f);
    out[1] = (int8_t)lroundf(m * cosf(rad) * 127.0f);
}

// libogc calls this once for each report it has queued for the Remote, the oldest first, with that
// report's buttons, pointer and acceleration (the data is reused for the next one, so what is wanted
// is taken now, and only the cheap part: the sticks come from the last report, in poll_remote). A
// report with an error holds nothing to read.
static void on_report(s32 chan, const WPADData *d) {
    if (chan < 0 || chan >= (s32)WL_INPUT_MAX_REMOTES || d->err != WPAD_ERR_NONE) {
        return;
    }
    WlRemoteReport r;
    r.buttons = d->btns_h;
    r.ir_valid = d->ir.valid || d->ir.raw_valid;
    r.ir_x = 0;
    r.ir_y = 0;
    if (d->ir.valid) {
        r.ir_x = sat16(d->ir.x);
        r.ir_y = sat16(d->ir.y);
    } else if (d->ir.raw_valid) {
        r.ir_x = sat16(d->ir.ax);
        r.ir_y = sat16(d->ir.ay);
    }
    r.accel[0] = sat16(d->gforce.x * 100.0f);
    r.accel[1] = sat16(d->gforce.y * 100.0f);
    r.accel[2] = sat16(d->gforce.z * 100.0f);
    wl_remote_reports_add(&g_reports[chan], &r);
}

static void poll_remote(uint32_t chan, WlRawRemote *out) {
    memset(out, 0, sizeof *out);
    u32 type = 0;
    s32 res = WPAD_Probe((s32)chan, &type);
    const WPADData *d = res == WPAD_ERR_NONE ? WPAD_Data((int)chan) : NULL;
    if (d != NULL && d->err != WPAD_ERR_NONE) {
        res = d->err;
        d = NULL;
    }
    if (d == NULL) {
        // NOT_READY and TRANSFER are momentary (an extension plugged in, a report lost): a Remote
        // that was connected stays connected with nothing pressed, so that a hot-plug does not
        // read as the controller leaving and returning. A missing Remote, or one without data for
        // REMOTE_GRACE_POLLS polls, clears it.
        if (g_remote_was[chan] && (res == WPAD_ERR_NOT_READY || res == WPAD_ERR_TRANSFER) &&
            ++g_remote_not_ready[chan] <= REMOTE_GRACE_POLLS) {
            out->connected = true;
            out->extension = g_remote_extension[chan];
        } else {
            g_remote_was[chan] = false;
        }
        return;
    }
    g_remote_not_ready[chan] = 0;
    if (!g_remote_was[chan]) {
        // Fresh connection: the data format does not survive a reconnect on every libogc build.
        (void)WPAD_SetDataFormat((s32)chan, WPAD_FMT_BTNS_ACC_IR);
        (void)WPAD_SetVRes((s32)chan, IR_VRES_W, IR_VRES_H);
        g_remote_was[chan] = true;
        g_remote_rumble[chan] = false; // (a Remote that comes back is not rumbling)
    }
    out->connected = true;
    switch (type) {
    case WPAD_EXP_NONE:
        out->extension = WL_EXT_NONE;
        break;
    case WPAD_EXP_NUNCHUK:
        out->extension = WL_EXT_NUNCHUK;
        break;
    case WPAD_EXP_CLASSIC:
        out->extension = WL_EXT_CLASSIC;
        break;
    default:
        out->extension = WL_EXT_OTHER;
        break;
    }
    g_remote_extension[chan] = out->extension;
    if (g_battery_logged[chan] != (int)d->battery_level) { // to settle the scale on real hardware
        g_battery_logged[chan] = (int)d->battery_level;
        WL_LOG_I("input", "remote %u battery code %u (read as 0..%u)", (unsigned)chan,
                 (unsigned)d->battery_level, BATTERY_FULL_CODE);
    }
    unsigned pct = ((unsigned)d->battery_level * 100u) / BATTERY_FULL_CODE;
    out->battery = (uint8_t)(pct > 100u ? 100u : pct);
    out->buttons = d->btns_h;

    // IR: the bounded position when libogc has one, else the unbounded one so the pointer can
    // keep tracking off-screen while the sensor bar is still seen.
    if (d->ir.valid) {
        out->ir_valid = true;
        out->ir_x = sat16(d->ir.x);
        out->ir_y = sat16(d->ir.y);
    } else if (d->ir.raw_valid) {
        out->ir_valid = true;
        out->ir_x = sat16(d->ir.ax);
        out->ir_y = sat16(d->ir.ay);
    }

    struct gforce_t g;
    WPAD_GForce((int)chan, &g);
    out->accel[0] = sat16(g.x * 100.0f);
    out->accel[1] = sat16(g.y * 100.0f);
    out->accel[2] = sat16(g.z * 100.0f);

    if (type == WPAD_EXP_NUNCHUK) {
        js_to_xy(&d->exp.nunchuk.js, out->stick[0]);
    } else if (type == WPAD_EXP_CLASSIC) {
        js_to_xy(&d->exp.classic.ljs, out->stick[0]);
        js_to_xy(&d->exp.classic.rjs, out->stick[1]);
        // The original's analogue triggers are 5 bits; the Pro's (and the Wii U Pro's) read 0 or
        // full, with the digital L/R bits set when pressed.
        out->trigger[0] = wl_input_trigger(d->exp.classic.ls_raw, 0, WL_CLASSIC_TRIGGER_FULL);
        out->trigger[1] = wl_input_trigger(d->exp.classic.rs_raw, 0, WL_CLASSIC_TRIGGER_FULL);
    }
    // The frame's reports over the last one's snapshot: every button pressed in any of them, the
    // pointer's mean, the acceleration's peak. With none (the Remote sent nothing since the last
    // poll) the snapshot stands.
    wl_remote_reports_finish(&g_reports[chan], out);
}

static void poll_pads(WlRawPad out[WL_INPUT_MAX_PADS]) {
    PADStatus st[PAD_CHANMAX];
    memset(st, 0, sizeof st);
    (void)PAD_Read(st);
    for (uint32_t i = 0; i < WL_INPUT_MAX_PADS; i++) {
        memset(&out[i], 0, sizeof out[i]);
        if (st[i].err == PAD_ERR_NONE) {
            g_pad_was[i] = true;
            g_pad_errors[i] = 0;
            out[i].connected = true;
            out[i].buttons = st[i].button;
            wl_input_stick_pair(st[i].stickX, st[i].stickY, WL_PAD_MAIN_STICK_FULL, out[i].stick[0]);
            wl_input_stick_pair(st[i].substickX, st[i].substickY, WL_PAD_C_STICK_FULL, out[i].stick[1]);
            out[i].trigger[0] = wl_input_trigger(st[i].triggerL, WL_PAD_TRIGGER_FIRST, WL_PAD_TRIGGER_FULL);
            out[i].trigger[1] = wl_input_trigger(st[i].triggerR, WL_PAD_TRIGGER_FIRST, WL_PAD_TRIGGER_FULL);
            g_pad_last[i] = out[i];
        } else if (st[i].err == PAD_ERR_NO_CONTROLLER) {
            g_pad_was[i] = false;
            g_pad_errors[i] = 0;
        } else if (g_pad_was[i]) {
            // NOT_READY and TRANSFER are momentary (libogc hands back the pad with nothing pressed):
            // the pad stays, in the state it was last read in, until it has failed so many polls in
            // a row that it is taken to be gone.
            if (g_pad_errors[i] < PAD_ERROR_GRACE_POLLS) {
                g_pad_errors[i]++;
            }
            if (g_pad_errors[i] < PAD_ERROR_GRACE_POLLS) {
                out[i] = g_pad_last[i];
            } else {
                g_pad_was[i] = false;
            }
        }
    }
}

static uint8_t kmods_from(uint16_t m) {
    uint8_t r = 0;
    if (MOD_ONESET(m, MOD_ANYSHIFT)) {
        r |= WL_KMOD_SHIFT;
    }
    if (MOD_ONESET(m, MOD_ANYCONTROL)) {
        r |= WL_KMOD_CTRL;
    }
    if (MOD_ONESET(m, MOD_ANYMETA)) {
        r |= WL_KMOD_ALT;
    }
    return r;
}

static void poll_keyboard(WlRawKeyboard *out) {
    memset(out, 0, sizeof *out);
    if (!g_kb_init) {
        return;
    }
    keyboard_event ev;
    // At most WL_INPUT_MAX_KEYS events per poll; the rest wait in libwiikeyboard's queue.
    while (out->count < WL_INPUT_MAX_KEYS && KEYBOARD_GetEvent(&ev) > 0) {
        switch (ev.type) {
        case KEYBOARD_CONNECTED:
            g_kb_connected = true;
            break;
        case KEYBOARD_DISCONNECTED:
            g_kb_connected = false;
            break;
        case KEYBOARD_PRESSED:
        case KEYBOARD_RELEASED:
            out->keys[out->count].code = ev.keycode;
            out->keys[out->count].modifiers = kmods_from(ev.modifiers);
            out->keys[out->count].down = ev.type == KEYBOARD_PRESSED;
            out->count++;
            break;
        }
    }
    out->connected = g_kb_connected;
}

void wl_platform_input_poll(WlRawInput *out) {
    memset(out, 0, sizeof *out);
    if (!g_init && wl_platform_input_init() != WL_OK) {
        return;
    }
    // (what WPAD_ScanPads does for every channel, with a look at each report on the way)
    for (uint32_t i = 0; i < WL_INPUT_MAX_REMOTES; i++) {
        wl_remote_reports_begin(&g_reports[i]);
        (void)WPAD_ReadPending((s32)i, on_report);
    }
    for (uint32_t i = 0; i < WL_INPUT_MAX_REMOTES; i++) {
        poll_remote(i, &out->remotes[i]);
    }
    poll_pads(out->pads);
    poll_keyboard(&out->keyboard);
}

bool wl_platform_rumble_enabled(void) {
    // SYSCONF's rumble setting ("BT.MOT": 1 on, 0 off; negative when it cannot be read, which
    // allows rumble). Read at each call (a lookup in the copy libogc holds in memory), so a
    // change is followed; the log says when it was found to change.
    static int g_logged = -1;
    (void)CONF_Init(); // idempotent
    int setting = CONF_GetPadMotorMode() == 0 ? 0 : 1;
    if (setting != g_logged) {
        g_logged = setting;
        WL_LOG_I("input", "the console's rumble setting: %s", setting != 0 ? "on" : "off");
    }
    return setting != 0;
}

WlSoundMode wl_platform_sound_mode(void) {
    // SYSCONF's "IPL.SND": 0 mono, 1 stereo, 2 surround; negative when it cannot be read.
    (void)CONF_Init(); // idempotent
    switch (CONF_GetSoundMode()) {
    case 0:
        return WL_SOUND_MONO;
    case 2:
        return WL_SOUND_SURROUND;
    default:
        return WL_SOUND_STEREO;
    }
}

void wl_platform_input_stop_rumble(void) {
    if (!g_init) {
        return; // (WPAD and PAD are not started: no motor can be running)
    }
    for (uint8_t i = 0; i < WL_INPUT_MAX_REMOTES; i++) {
        (void)WPAD_Rumble(i, 0);
        g_remote_rumble[i] = false;
    }
    for (uint8_t i = 0; i < WL_INPUT_MAX_PADS; i++) {
        PAD_ControlMotor(i, PAD_MOTOR_STOP);
        g_pad_rumble[i] = false;
    }
}

void wl_platform_rumble(uint8_t remote, bool on) {
    if (remote >= WL_INPUT_MAX_REMOTES) {
        return;
    }
    on = on && wl_platform_rumble_enabled();
    // (each call to libogc is a report to the Remote: only a change is sent)
    if (g_remote_rumble[remote] != on) {
        (void)WPAD_Rumble(remote, on ? 1 : 0);
        g_remote_rumble[remote] = on;
    }
}

void wl_platform_pad_rumble(uint8_t pad, bool on) {
    if (pad >= WL_INPUT_MAX_PADS) {
        return;
    }
    on = on && wl_platform_rumble_enabled();
    if (g_pad_rumble[pad] != on) {
        PAD_ControlMotor(pad, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
        g_pad_rumble[pad] = on;
    }
}
