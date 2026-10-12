// SPDX-License-Identifier: MIT
// The libogc calls input_wii.c makes, answering from g_input_stub (input_stub.h).
#include <string.h>

#include "input_stub.h"
#include <ogc/conf.h>
#include <wiikeyboard/keyboard.h>

InputStub g_input_stub;

s32 WPAD_Init(void) {
    return 0;
}

s32 WPAD_SetDataFormat(s32 chan, s32 fmt) {
    (void)fmt;
    if (chan >= 0) {
        g_input_stub.data_format_calls++;
    }
    return 0;
}

s32 WPAD_SetVRes(s32 chan, u32 xres, u32 yres) {
    (void)chan;
    (void)xres;
    (void)yres;
    return 0;
}

s32 WPAD_Probe(s32 chan, u32 *type) {
    *type = g_input_stub.probe_type[chan];
    return g_input_stub.probe[chan];
}

s32 WPAD_ScanPads(void) {
    return 0;
}

s32 WPAD_Rumble(s32 chan, int status) {
    g_input_stub.rumble[chan] = status;
    g_input_stub.rumble_calls++;
    return 0;
}

WPADData *WPAD_Data(int chan) {
    return &g_input_stub.data[chan];
}

void WPAD_GForce(int chan, struct gforce_t *gforce) {
    *gforce = g_input_stub.data[chan].gforce;
}

u32 PAD_Init(void) {
    return 1;
}

u32 PAD_Read(PADStatus *status) {
    memcpy(status, g_input_stub.pad, sizeof g_input_stub.pad);
    return 0;
}

void PAD_ControlMotor(s32 chan, u32 cmd) {
    g_input_stub.motor[chan] = (int)cmd;
    g_input_stub.motor_calls++;
}

s32 CONF_Init(void) {
    return 0;
}

s32 CONF_GetPadMotorMode(void) {
    return g_input_stub.motor_mode;
}

s32 CONF_GetSoundMode(void) {
    return g_input_stub.sound_mode;
}

int32_t KEYBOARD_Init(keyPressCallback keypress_cb) {
    (void)keypress_cb;
    return -1; // no USB: input_wii.c carries on without a keyboard
}

int32_t KEYBOARD_Deinit(void) {
    return 0;
}

int32_t KEYBOARD_GetEvent(keyboard_event *event) {
    (void)event;
    return 0;
}
