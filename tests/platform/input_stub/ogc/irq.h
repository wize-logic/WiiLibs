// SPDX-License-Identifier: MIT
// A stand-in for libogc's <ogc/irq.h> (test only): interrupts are counted, not switched.
#pragma once

#include <stdint.h>

#ifndef OGC_STUB_TYPES
#define OGC_STUB_TYPES
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
#endif

u32 IRQ_Disable(void);
void IRQ_Restore(u32 level);
