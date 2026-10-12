// SPDX-License-Identifier: MIT
// What the libogc stand-ins (this directory) answer, set by the test.
#pragma once

#include <ogc/conf.h>
#include <ogc/pad.h>
#include <wiiuse/wpad.h>

typedef struct {
    s32 probe[4];       // WPAD_Probe's result per channel
    u32 probe_type[4];  // and the expansion type it reports
    WPADData data[4];   // WPAD_Data's
    PADStatus pad[4];   // what PAD_Read reports
    int rumble[4];      // the last WPAD_Rumble per channel
    int motor[4];       // the last PAD_ControlMotor per channel
    unsigned rumble_calls;      // WPAD_Rumble calls, all channels
    unsigned motor_calls;       // PAD_ControlMotor calls, all channels
    s32 motor_mode;     // what CONF_GetPadMotorMode reports: 1 on, 0 off, negative unreadable
    unsigned data_format_calls; // WPAD_SetDataFormat on one channel (a new connection)
} InputStub;

extern InputStub g_input_stub;
