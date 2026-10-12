// SPDX-License-Identifier: MIT
// The NV2A's register and method numbers (nv2a.h, nv2a_kelvin.h): the MMIO registers the Xbox's
// Direct3D and kernel touch, the DMA pusher's command words, the puller's own methods and the
// methods of the Kelvin 3D class (NV097), with the field layouts and values the device model and the
// state tracker read. The names are WiiLibs' own; the numbers are facts, each group cited to the
// public source it was taken from:
//
//   [envy-pusher]  envytools, "PFIFO DMA pusher" (docs/hw/fifo/dma-pusher.rst), MIT
//   [envy-puller]  envytools, "PFIFO puller" (docs/hw/fifo/puller.rst), MIT
//   [envy-3d]      envytools rnndb graph/nv10_3d.xml, graph/nv20_3d.xml, graph/nv_3ddefs.xml, MIT
//   [envy-pmc]     envytools rnndb bus/pmc.xml, bus/ptimer.xml, display/nv3_pcrtc.xml, MIT
//   [pbkit]        nxdk lib/pbkit/outer.h and pbkit.c (the Xbox's register map as nxdk drives it), MIT
//   [nxdk-regs]    nxdk lib/pbkit/nv_regs.h (NV097 method numbers and fields of the NV2A itself)
//   [nouveau]      Linux nouveau, nvkm/engine/fifo/nv04.c, nvkm/core/ramht.c,
//                  nvkm/engine/dma/usernv04.c (RAMHT entries, DMA objects), MIT
//
// tests/xbox/test_xbox_nv2a_regs.c compares these numbers with another public table when one is
// checked out next to WiiLibs (a test-only cross-check; nothing of that table is copied).
#pragma once

// ---- the MMIO blocks, offsets from 0xFD000000 [pbkit] [envy-pmc] -----------------------------
enum {
    WL_NV2A_PMC = 0x000000,
    WL_NV2A_PBUS = 0x001000,
    WL_NV2A_PFIFO = 0x002000,
    WL_NV2A_PTIMER = 0x009000,
    WL_NV2A_PFB = 0x100000,
    WL_NV2A_PGRAPH = 0x400000,
    WL_NV2A_PCRTC = 0x600000,
    WL_NV2A_PRMCIO = 0x601000,
    WL_NV2A_PRAMDAC = 0x680000,
    WL_NV2A_PRMDIO = 0x681000,
    // instance memory: the last megabyte of RAM, seen through this window [envy-pusher, NV4:NV40]
    WL_NV2A_PRAMIN = 0x700000,
    WL_NV2A_PRAMIN_SIZE = 0x100000,
    // the channels' control areas: 0x800000 + chid * 0x10000 [envy-pusher]
    WL_NV2A_USER = 0x800000,
    WL_NV2A_USER_STRIDE = 0x10000,
    WL_NV2A_CHANNELS = 32,
    WL_NV2A_MMIO_SIZE = 0x1000000
};

// ---- PMC [pbkit] ---------------------------------------------------------------------------
enum {
    WL_NV2A_PMC_BOOT_0 = 0x000000,
    WL_NV2A_PMC_INTR_0 = 0x000100,
    WL_NV2A_PMC_INTR_EN_0 = 0x000140,
    WL_NV2A_PMC_ENABLE = 0x000200,
    // PMC_INTR_0's bits: the units with an interrupt pending
    WL_NV2A_PMC_INTR_PFIFO = 1u << 8,
    WL_NV2A_PMC_INTR_PGRAPH = 1u << 12,
    WL_NV2A_PMC_INTR_PVIDEO = 1u << 16,
    WL_NV2A_PMC_INTR_PTIMER = 1u << 20,
    WL_NV2A_PMC_INTR_PCRTC = 1u << 24,
    // PMC_INTR_EN_0: 1 the hardware line, 2 software
    WL_NV2A_PMC_INTR_EN_HARDWARE = 1u,
    WL_NV2A_PMC_INTR_EN_SOFTWARE = 2u
};

// ---- PBUS (the PCI configuration's mirror) [pbkit] -------------------------------------------
enum {
    WL_NV2A_PBUS_PCI_NV_0 = 0x001800, // vendor and device id
    WL_NV2A_PBUS_PCI_NV_1 = 0x001804,
    WL_NV2A_PBUS_PCI_NV_2 = 0x001808, // revision
    // the NV2A's PCI id: NVIDIA (0x10DE), device 0x02A0
    WL_NV2A_PCI_ID = 0x02A010DEu
};

// ---- PFIFO [pbkit] ---------------------------------------------------------------------------
enum {
    WL_NV2A_PFIFO_INTR_0 = 0x002100,
    WL_NV2A_PFIFO_INTR_EN_0 = 0x002140,
    WL_NV2A_PFIFO_INTR_CACHE_ERROR = 1u << 0,
    WL_NV2A_PFIFO_INTR_RUNOUT = 1u << 4,
    WL_NV2A_PFIFO_INTR_DMA_PUSHER = 1u << 12,
    WL_NV2A_PFIFO_INTR_SEMAPHORE = 1u << 20,
    WL_NV2A_PFIFO_RAMHT = 0x002210, // base (bits 8:4, << 8 bytes), size (bits 17:16: 4 KiB << n)
    WL_NV2A_PFIFO_RAMFC = 0x002214,
    WL_NV2A_PFIFO_RAMRO = 0x002218,
    WL_NV2A_PFIFO_RUNOUT_STATUS = 0x002400,
    WL_NV2A_PFIFO_RUNOUT_LOW_MARK_EMPTY = 1u << 4,
    WL_NV2A_PFIFO_CACHES = 0x002500,
    WL_NV2A_PFIFO_MODE = 0x002504,
    WL_NV2A_PFIFO_DMA = 0x002508,
    WL_NV2A_PFIFO_CACHE1_PUSH0 = 0x003200,
    WL_NV2A_PFIFO_CACHE1_PUSH1 = 0x003204, // the channel id in bits 4:0
    WL_NV2A_PFIFO_CACHE1_PUT = 0x003210,
    WL_NV2A_PFIFO_CACHE1_STATUS = 0x003214,
    WL_NV2A_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY = 1u << 4,
    WL_NV2A_PFIFO_CACHE1_DMA_PUSH = 0x003220,
    WL_NV2A_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY = 1u << 4,
    WL_NV2A_PFIFO_CACHE1_DMA_STATE = 0x003228,
    WL_NV2A_PFIFO_CACHE1_DMA_INSTANCE = 0x00322C,
    WL_NV2A_PFIFO_CACHE1_DMA_PUT = 0x003240,
    WL_NV2A_PFIFO_CACHE1_DMA_GET = 0x003244,
    WL_NV2A_PFIFO_CACHE1_REF = 0x003248,
    WL_NV2A_PFIFO_CACHE1_DMA_SUBROUTINE = 0x00324C, // the return offset, bit 0 active
    WL_NV2A_PFIFO_CACHE1_PULL0 = 0x003250,
    WL_NV2A_PFIFO_CACHE1_PULL0_HASH_STATE_BUSY = 1u << 12,
    WL_NV2A_PFIFO_CACHE1_SEMAPHORE = 0x00326C,
    WL_NV2A_PFIFO_CACHE1_GET = 0x003270
};

// ---- PTIMER [pbkit] [envy-pmc] ----------------------------------------------------------------
enum {
    WL_NV2A_PTIMER_INTR_0 = 0x009100,
    WL_NV2A_PTIMER_INTR_EN_0 = 0x009140,
    WL_NV2A_PTIMER_NUMERATOR = 0x009200,
    WL_NV2A_PTIMER_DENOMINATOR = 0x009210,
    WL_NV2A_PTIMER_TIME_0 = 0x009400, // nanoseconds, bits 31:5 (the low 27 bits of time >> 5)
    WL_NV2A_PTIMER_TIME_1 = 0x009410, // the high bits (time >> 32)
    WL_NV2A_PTIMER_ALARM_0 = 0x009420
};

// ---- PFB [pbkit] [nouveau: the RAM size in 0x10020C on NV10 and later] -------------------------
enum {
    WL_NV2A_PFB_CFG0 = 0x100200,
    WL_NV2A_PFB_CFG1 = 0x100204,
    WL_NV2A_PFB_CSTATUS = 0x10020C, // the RAM's size in bytes
    WL_NV2A_PFB_NVM = 0x100214,
    // the write-combining buffer's flush: written with bit 16 set, polled until it clears
    WL_NV2A_PFB_WBC = 0x100410,
    WL_NV2A_PFB_WBC_FLUSH = 1u << 16
};

// ---- PGRAPH [pbkit] [envy-3d nv4_pgraph] -------------------------------------------------------
enum {
    WL_NV2A_PGRAPH_INTR = 0x400100,
    WL_NV2A_PGRAPH_NSOURCE = 0x400108,
    WL_NV2A_PGRAPH_INTR_EN = 0x400140,
    WL_NV2A_PGRAPH_INTR_NOTIFY = 1u << 0,
    WL_NV2A_PGRAPH_INTR_MISSING_HW = 1u << 4,
    WL_NV2A_PGRAPH_INTR_ERROR = 1u << 20,
    WL_NV2A_PGRAPH_STATUS = 0x400700, // 0: idle
    WL_NV2A_PGRAPH_FIFO = 0x400720,
    WL_NV2A_PGRAPH_CHANNEL_CTX_POINTER = 0x400784,
    WL_NV2A_PGRAPH_CHANNEL_CTX_STATUS = 0x400788
};

// ---- PCRTC, PRMCIO [pbkit] [envy-pmc nv3_pcrtc] -------------------------------------------------
enum {
    WL_NV2A_PCRTC_INTR_0 = 0x600100,
    WL_NV2A_PCRTC_INTR_EN_0 = 0x600140,
    WL_NV2A_PCRTC_INTR_VBLANK = 1u << 0,
    WL_NV2A_PCRTC_START = 0x600800, // the scanned-out frame buffer's address: a write is a flip
    WL_NV2A_PCRTC_RASTER = 0x600808,
    WL_NV2A_PRMCIO_CRX = 0x6013D4, // the CRTC's index (a byte)
    WL_NV2A_PRMCIO_CR = 0x6013D5,  // its data
    WL_NV2A_PRMCIO_INP0 = 0x6013DA // input status 1: bit 3 vertical retrace, bit 0 not displaying
};

// ---- the channel control area, offsets in a channel's 64 KiB [envy-pusher] ---------------------
enum {
    WL_NV2A_USER_DMA_PUT = 0x40,
    WL_NV2A_USER_DMA_GET = 0x44,
    WL_NV2A_USER_REF = 0x48
};

// ---- the DMA pusher's command words (pre-GF100, NV4-style) [envy-pusher] ------------------------
//   increasing methods   000CCCCCCCCCCC00SSSMMMMMMMMMMM00   (NV4+)
//   old jump             001JJJJJJJJJJJJJJJJJJJJJJJJJJJ00   (NV4+: target bits 28:2)
//   non-increasing       010CCCCCCCCCCC00SSSMMMMMMMMMMM00   (NV10+)
//   jump                 JJJJJJJJJJJJJJJJJJJJJJJJJJJJJJ01   (NV1A+: target bits 31:2)
//   call                 JJJJJJJJJJJJJJJJJJJJJJJJJJJJJJ10   (NV1A+: one level)
//   return               00000000000000100000000000000000   (NV1A+)
enum {
    WL_NV2A_CMD_METHOD_MASK = 0x00001FFCu,
    WL_NV2A_CMD_SUBCH_SHIFT = 13,
    WL_NV2A_CMD_COUNT_SHIFT = 18,
    WL_NV2A_CMD_COUNT_MASK = 0x7FFu,
    WL_NV2A_CMD_TYPE_MASK = 0xE0030003u, // the bits that tell a method header from the others
    WL_NV2A_CMD_INCREASING = 0x00000000u,
    WL_NV2A_CMD_NON_INCREASING = 0x40000000u,
    WL_NV2A_CMD_OLD_JUMP_MASK = 0xE0000003u,
    WL_NV2A_CMD_OLD_JUMP = 0x20000000u,
    WL_NV2A_CMD_OLD_JUMP_TARGET = 0x1FFFFFFCu,
    WL_NV2A_CMD_JUMP = 1u, // in bits 1:0
    WL_NV2A_CMD_CALL = 2u,
    WL_NV2A_CMD_RETURN = 0x00020000u
};

// ---- the puller's own methods, below 0x100 on any subchannel [envy-puller] -----------------------
enum {
    WL_NV2A_PULLER_OBJECT = 0x0000,           // binds the object of a handle (RAMHT) to the subchannel
    WL_NV2A_PULLER_REF_CNT = 0x0050,          // NV10+: the channel's reference counter
    WL_NV2A_PULLER_DMA_SEMAPHORE = 0x0060,    // NV1A+: the semaphores' DMA object (a handle)
    WL_NV2A_PULLER_SEMAPHORE_OFFSET = 0x0064, // its offset (12 bits)
    WL_NV2A_PULLER_SEMAPHORE_ACQUIRE = 0x0068, // waits for the word to equal the parameter
    WL_NV2A_PULLER_SEMAPHORE_RELEASE = 0x006C, // writes the parameter to the word
    // methods 0x180-0x1FC of an engine take a handle, which the puller looks up in RAMHT and
    // hands down as the object's instance address
    WL_NV2A_PULLER_HANDLE_FIRST = 0x0180,
    WL_NV2A_PULLER_HANDLE_LAST = 0x01FC
};

// ---- RAMHT entries and DMA objects in instance memory [nouveau] ---------------------------------
//   RAMHT entry: word 0 the handle; word 1 the instance address >> 4 (bits 15:0), the engine
//   (bits 17:16: 0 software, 1 graphics), the channel (bits 28:24), valid (bit 31).
//   DMA object: word 0 the class (bits 11:0) and the base's low 12 bits (bits 31:20, "adjust"),
//   word 1 the limit, word 2 the base's page (bits 31:12) and access bits.
//   A graphics object: word 0 holds its class (bits 11:0).
enum {
    WL_NV2A_RAMHT_INSTANCE_MASK = 0x0000FFFFu,
    WL_NV2A_RAMHT_ENGINE_SHIFT = 16,
    WL_NV2A_RAMHT_CHID_SHIFT = 24,
    WL_NV2A_RAMHT_VALID = 0x80000000u,
    WL_NV2A_DMA_CLASS_MASK = 0x00000FFFu,
    WL_NV2A_DMA_ADJUST_SHIFT = 20,
    WL_NV2A_DMA_PAGE_MASK = 0xFFFFF000u
};

// object classes the Xbox's Direct3D creates [envy-3d] [nxdk-regs]
enum {
    WL_NV2A_CLASS_DMA_FROM_MEMORY = 0x02,
    WL_NV2A_CLASS_DMA_TO_MEMORY = 0x03,
    WL_NV2A_CLASS_DMA_IN_MEMORY = 0x3D,
    WL_NV2A_CLASS_M2MF = 0x39,
    WL_NV2A_CLASS_SURFACES_2D = 0x62,
    WL_NV2A_CLASS_IMAGE_BLIT = 0x9F,
    WL_NV2A_CLASS_KELVIN = 0x97
};

// ---- the Kelvin class (NV097): methods [envy-3d], NV2A-only ones [nxdk-regs] ------------------
enum {
    WL_NV2A_K_NO_OPERATION = 0x0100,
    WL_NV2A_K_NOTIFY = 0x0104, // 0: write the notifier; 1: write it and raise PGRAPH NOTIFY
    WL_NV2A_K_WAIT_FOR_IDLE = 0x0110,
    WL_NV2A_K_SET_FLIP_READ = 0x0120,
    WL_NV2A_K_SET_FLIP_WRITE = 0x0124,
    WL_NV2A_K_SET_FLIP_MODULO = 0x0128,
    WL_NV2A_K_FLIP_INCREMENT_WRITE = 0x012C,
    WL_NV2A_K_FLIP_STALL = 0x0130,
    // the context DMAs (handles, resolved by the puller) [envy-3d: DMA_NOTIFY .. DMA_QUERY]
    WL_NV2A_K_SET_CONTEXT_DMA_NOTIFIES = 0x0180,
    WL_NV2A_K_SET_CONTEXT_DMA_A = 0x0184,
    WL_NV2A_K_SET_CONTEXT_DMA_B = 0x0188,
    WL_NV2A_K_SET_CONTEXT_DMA_STATE = 0x0190,
    WL_NV2A_K_SET_CONTEXT_DMA_COLOR = 0x0194,
    WL_NV2A_K_SET_CONTEXT_DMA_ZETA = 0x0198,
    WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_A = 0x019C,
    WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_B = 0x01A0,
    WL_NV2A_K_SET_CONTEXT_DMA_SEMAPHORE = 0x01A4,
    WL_NV2A_K_SET_CONTEXT_DMA_REPORT = 0x01A8,
    // the surfaces [envy-3d: RT_HORIZ .. ZETA_OFFSET]
    WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL = 0x0200, // x bits 15:0, width 31:16
    WL_NV2A_K_SET_SURFACE_CLIP_VERTICAL = 0x0204,   // y, height
    WL_NV2A_K_SET_SURFACE_FORMAT = 0x0208,
    WL_NV2A_K_SET_SURFACE_PITCH = 0x020C, // colour bits 15:0, zeta 31:16
    WL_NV2A_K_SET_SURFACE_COLOR_OFFSET = 0x0210,
    WL_NV2A_K_SET_SURFACE_ZETA_OFFSET = 0x0214,
    // the combiners [envy-3d: RC_IN_ALPHA .. RC_ENABLE]
    WL_NV2A_K_SET_COMBINER_ALPHA_ICW = 0x0260, // eight
    WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW0 = 0x0288,
    WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW1 = 0x028C,
    WL_NV2A_K_SET_CONTROL0 = 0x0290,
    WL_NV2A_K_SET_LIGHT_CONTROL = 0x0294,
    WL_NV2A_K_SET_COLOR_MATERIAL = 0x0298,
    // fog [envy-3d]
    WL_NV2A_K_SET_FOG_MODE = 0x029C,
    WL_NV2A_K_SET_FOG_GEN_MODE = 0x02A0,
    WL_NV2A_K_SET_FOG_ENABLE = 0x02A4,
    WL_NV2A_K_SET_FOG_COLOR = 0x02A8, // ABGR: red in bits 7:0
    WL_NV2A_K_SET_WINDOW_CLIP_TYPE = 0x02B4,
    WL_NV2A_K_SET_WINDOW_CLIP_HORIZONTAL = 0x02C0, // eight
    WL_NV2A_K_SET_WINDOW_CLIP_VERTICAL = 0x02E0,   // eight
    // the raster state [envy-3d: ALPHA_FUNC_ENABLE .. FRONT_FACE]
    WL_NV2A_K_SET_ALPHA_TEST_ENABLE = 0x0300,
    WL_NV2A_K_SET_BLEND_ENABLE = 0x0304,
    WL_NV2A_K_SET_CULL_FACE_ENABLE = 0x0308,
    WL_NV2A_K_SET_DEPTH_TEST_ENABLE = 0x030C,
    WL_NV2A_K_SET_DITHER_ENABLE = 0x0310,
    WL_NV2A_K_SET_LIGHTING_ENABLE = 0x0314,
    WL_NV2A_K_SET_POINT_PARAMS_ENABLE = 0x0318,
    WL_NV2A_K_SET_POINT_SMOOTH_ENABLE = 0x031C,
    WL_NV2A_K_SET_LINE_SMOOTH_ENABLE = 0x0320,
    WL_NV2A_K_SET_POLY_SMOOTH_ENABLE = 0x0324,
    WL_NV2A_K_SET_SKIN_MODE = 0x0328,
    WL_NV2A_K_SET_STENCIL_TEST_ENABLE = 0x032C,
    WL_NV2A_K_SET_POLY_OFFSET_POINT_ENABLE = 0x0330,
    WL_NV2A_K_SET_POLY_OFFSET_LINE_ENABLE = 0x0334,
    WL_NV2A_K_SET_POLY_OFFSET_FILL_ENABLE = 0x0338,
    WL_NV2A_K_SET_ALPHA_FUNC = 0x033C,
    WL_NV2A_K_SET_ALPHA_REF = 0x0340,
    WL_NV2A_K_SET_BLEND_FUNC_SFACTOR = 0x0344,
    WL_NV2A_K_SET_BLEND_FUNC_DFACTOR = 0x0348,
    WL_NV2A_K_SET_BLEND_COLOR = 0x034C,
    WL_NV2A_K_SET_BLEND_EQUATION = 0x0350,
    WL_NV2A_K_SET_DEPTH_FUNC = 0x0354,
    WL_NV2A_K_SET_COLOR_MASK = 0x0358, // blue bit 0, green 8, red 16, alpha 24
    WL_NV2A_K_SET_DEPTH_MASK = 0x035C,
    WL_NV2A_K_SET_STENCIL_MASK = 0x0360,
    WL_NV2A_K_SET_STENCIL_FUNC = 0x0364,
    WL_NV2A_K_SET_STENCIL_FUNC_REF = 0x0368,
    WL_NV2A_K_SET_STENCIL_FUNC_MASK = 0x036C,
    WL_NV2A_K_SET_STENCIL_OP_FAIL = 0x0370,
    WL_NV2A_K_SET_STENCIL_OP_ZFAIL = 0x0374,
    WL_NV2A_K_SET_STENCIL_OP_ZPASS = 0x0378,
    WL_NV2A_K_SET_SHADE_MODEL = 0x037C,
    WL_NV2A_K_SET_LINE_WIDTH = 0x0380,
    WL_NV2A_K_SET_POLYGON_OFFSET_SCALE_FACTOR = 0x0384,
    WL_NV2A_K_SET_POLYGON_OFFSET_BIAS = 0x0388,
    WL_NV2A_K_SET_FRONT_POLYGON_MODE = 0x038C,
    WL_NV2A_K_SET_BACK_POLYGON_MODE = 0x0390,
    WL_NV2A_K_SET_CLIP_MIN = 0x0394,
    WL_NV2A_K_SET_CLIP_MAX = 0x0398,
    WL_NV2A_K_SET_CULL_FACE = 0x039C,
    WL_NV2A_K_SET_FRONT_FACE = 0x03A0,
    WL_NV2A_K_SET_NORMALIZATION_ENABLE = 0x03A4,
    WL_NV2A_K_SET_SPECULAR_ENABLE = 0x03B8,
    WL_NV2A_K_SET_LIGHT_ENABLE_MASK = 0x03BC,
    WL_NV2A_K_SET_TEXGEN_S = 0x03C0, // 16 bytes a stage: S, T, R, Q
    WL_NV2A_K_SET_TEXTURE_MATRIX_ENABLE = 0x0420, // four
    WL_NV2A_K_SET_POINT_SIZE = 0x043C,
    // the fixed function's matrices [nxdk-regs]
    WL_NV2A_K_SET_PROJECTION_MATRIX = 0x0440,
    WL_NV2A_K_SET_MODEL_VIEW_MATRIX = 0x0480,         // four of 16 floats
    WL_NV2A_K_SET_INVERSE_MODEL_VIEW_MATRIX = 0x0580, // four of 16 floats
    WL_NV2A_K_SET_COMPOSITE_MATRIX = 0x0680,
    WL_NV2A_K_SET_TEXTURE_MATRIX = 0x06C0, // four of 16 floats
    WL_NV2A_K_SET_FOG_PARAMS = 0x09C0,     // three floats
    WL_NV2A_K_SET_FOG_PLANE = 0x09D0,      // four floats
    WL_NV2A_K_SET_FLAT_SHADE_OP = 0x09FC,
    WL_NV2A_K_SET_SCENE_AMBIENT_COLOR = 0x0A10,
    WL_NV2A_K_SET_VIEWPORT_OFFSET = 0x0A20, // four floats [envy-3d: VIEWPORT_TRANSLATE]
    WL_NV2A_K_SET_POINT_PARAMS = 0x0A30,    // eight floats
    WL_NV2A_K_SET_EYE_POSITION = 0x0A50,
    WL_NV2A_K_SET_COMBINER_FACTOR0 = 0x0A60, // eight, ARGB
    WL_NV2A_K_SET_COMBINER_FACTOR1 = 0x0A80,
    WL_NV2A_K_SET_COMBINER_ALPHA_OCW = 0x0AA0,
    WL_NV2A_K_SET_COMBINER_COLOR_ICW = 0x0AC0,
    WL_NV2A_K_SET_COLOR_KEY_COLOR = 0x0AE0, // four
    WL_NV2A_K_SET_VIEWPORT_SCALE = 0x0AF0,  // four floats
    // the vertex program's upload windows: 32 words each [envy-3d: VP_UPLOAD_INST, VP_UPLOAD_CONST]
    WL_NV2A_K_SET_TRANSFORM_PROGRAM = 0x0B00,
    WL_NV2A_K_SET_TRANSFORM_CONSTANT = 0x0B80,
    WL_NV2A_K_SET_BACK_LIGHT = 0x0C00,
    WL_NV2A_K_SET_LIGHT = 0x1000, // eight of 128 bytes
    WL_NV2A_K_SET_STIPPLE_ENABLE = 0x147C,
    WL_NV2A_K_SET_STIPPLE_PATTERN = 0x1480,
    // immediate vertex attributes [envy-3d: VERTEX_POS_3F .. VERTEX_FOG_1F] [nxdk-regs]
    WL_NV2A_K_SET_VERTEX3F = 0x1500,
    WL_NV2A_K_SET_VERTEX4F = 0x1518,
    WL_NV2A_K_SET_VERTEX4S = 0x1528,
    WL_NV2A_K_SET_NORMAL3F = 0x1530,
    WL_NV2A_K_SET_NORMAL3S = 0x1540,
    WL_NV2A_K_SET_DIFFUSE_COLOR4F = 0x1550,
    WL_NV2A_K_SET_DIFFUSE_COLOR3F = 0x1560,
    WL_NV2A_K_SET_DIFFUSE_COLOR4I = 0x156C,
    WL_NV2A_K_SET_SPECULAR_COLOR4F = 0x1570,
    WL_NV2A_K_SET_SPECULAR_COLOR3F = 0x1580,
    WL_NV2A_K_SET_SPECULAR_COLOR4I = 0x158C,
    WL_NV2A_K_SET_TEXCOORD0_2F = 0x1590,
    WL_NV2A_K_SET_TEXCOORD0_2S = 0x1598,
    WL_NV2A_K_SET_TEXCOORD0_4F = 0x15A0,
    WL_NV2A_K_SET_TEXCOORD0_4S = 0x15B0,
    WL_NV2A_K_SET_TEXCOORD1_2F = 0x15B8,
    WL_NV2A_K_SET_TEXCOORD1_2S = 0x15C0,
    WL_NV2A_K_SET_TEXCOORD1_4F = 0x15C8,
    WL_NV2A_K_SET_TEXCOORD1_4S = 0x15D8,
    WL_NV2A_K_SET_TEXCOORD2_2F = 0x15E0,
    WL_NV2A_K_SET_TEXCOORD2_2S = 0x15E8,
    WL_NV2A_K_SET_TEXCOORD2_4F = 0x15F0,
    WL_NV2A_K_SET_TEXCOORD2_4S = 0x1600,
    WL_NV2A_K_SET_TEXCOORD3_2F = 0x1608,
    WL_NV2A_K_SET_TEXCOORD3_2S = 0x1610,
    WL_NV2A_K_SET_TEXCOORD3_4F = 0x1620,
    WL_NV2A_K_SET_TEXCOORD3_4S = 0x1630,
    WL_NV2A_K_SET_FOG_COORD = 0x1698,
    WL_NV2A_K_SET_WEIGHT1F = 0x169C,
    WL_NV2A_K_SET_EDGE_FLAG = 0x16BC,
    WL_NV2A_K_SET_WEIGHT4F = 0x16C0,
    // vertex arrays [envy-3d: VTXBUF_OFFSET, VTXBUF_FMT .. VTXBUF_DATA]
    WL_NV2A_K_BREAK_VERTEX_BUFFER_CACHE = 0x1710,
    WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET = 0x1720, // sixteen: offset bits 27:0, bit 31 DMA B
    WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT = 0x1760, // sixteen: type 3:0, size 7:4, stride 31:8
    WL_NV2A_K_SET_BACK_MATERIAL_ALPHA = 0x17AC,
    WL_NV2A_K_SET_LOGIC_OP_ENABLE = 0x17BC,
    WL_NV2A_K_SET_LOGIC_OP = 0x17C0,
    WL_NV2A_K_SET_LIGHT_TWO_SIDE_ENABLE = 0x17C4,
    // reports [nxdk-regs]
    WL_NV2A_K_CLEAR_REPORT_VALUE = 0x17C8, // 1: the pixel count
    WL_NV2A_K_SET_ZPASS_PIXEL_COUNT_ENABLE = 0x17CC,
    WL_NV2A_K_GET_REPORT = 0x17D0, // offset bits 23:0, type 31:24 (1: pixel count)
    WL_NV2A_K_SET_EYE_DIRECTION = 0x17E0,
    WL_NV2A_K_SET_SHADER_CLIP_PLANE_MODE = 0x17F8,
    WL_NV2A_K_SET_BEGIN_END = 0x17FC,
    WL_NV2A_K_ARRAY_ELEMENT16 = 0x1800, // two 16-bit indices, the first in bits 15:0
    WL_NV2A_K_ARRAY_ELEMENT32 = 0x1808,
    WL_NV2A_K_DRAW_ARRAYS = 0x1810, // first bits 23:0, count - 1 in 31:24
    WL_NV2A_K_INLINE_ARRAY = 0x1818,
    WL_NV2A_K_SET_EYE_VECTOR = 0x181C,
    WL_NV2A_K_SET_VERTEX_DATA2F_M = 0x1880, // sixteen attributes of two floats
    WL_NV2A_K_SET_VERTEX_DATA2S = 0x1900,   // sixteen of one word (two shorts)
    WL_NV2A_K_SET_VERTEX_DATA4UB = 0x1940,  // sixteen of one word (four bytes)
    WL_NV2A_K_SET_VERTEX_DATA4S_M = 0x1980, // sixteen of two words (four shorts)
    WL_NV2A_K_SET_VERTEX_DATA4F_M = 0x1A00, // sixteen of four floats [envy-3d: VERTEX_ATTR_4F]
    // the texture stages: four of 64 bytes [envy-3d: TEX] [nxdk-regs]
    WL_NV2A_K_SET_TEXTURE_OFFSET = 0x1B00,
    WL_NV2A_K_SET_TEXTURE_FORMAT = 0x1B04,
    WL_NV2A_K_SET_TEXTURE_ADDRESS = 0x1B08,
    WL_NV2A_K_SET_TEXTURE_CONTROL0 = 0x1B0C,
    WL_NV2A_K_SET_TEXTURE_CONTROL1 = 0x1B10, // the linear pitch in bits 31:16
    WL_NV2A_K_SET_TEXTURE_FILTER = 0x1B14,
    WL_NV2A_K_SET_TEXTURE_IMAGE_RECT = 0x1B1C, // width bits 31:16, height 15:0
    WL_NV2A_K_SET_TEXTURE_PALETTE = 0x1B20,    // DMA bit 0, length 3:2, offset 31:6
    WL_NV2A_K_SET_TEXTURE_BORDER_COLOR = 0x1B24,
    WL_NV2A_K_SET_TEXTURE_BUMP_ENV_MAT = 0x1B28, // four floats
    WL_NV2A_K_SET_TEXTURE_BUMP_ENV_SCALE = 0x1B38,
    WL_NV2A_K_SET_TEXTURE_BUMP_ENV_OFFSET = 0x1B3C,
    WL_NV2A_K_TEXTURE_STRIDE = 0x40,
    // semaphores, clears [nxdk-regs] [envy-3d: CLEAR_DEPTH_VALUE .. CLEAR_BUFFERS]
    WL_NV2A_K_SET_SEMAPHORE_OFFSET = 0x1D6C,
    WL_NV2A_K_BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1D70,
    WL_NV2A_K_SET_ZMIN_MAX_CONTROL = 0x1D78,
    WL_NV2A_K_SET_ANTI_ALIASING_CONTROL = 0x1D7C,
    WL_NV2A_K_SET_COMPRESS_ZBUFFER_EN = 0x1D80,
    WL_NV2A_K_SET_OCCLUDE_ZSTENCIL_EN = 0x1D84,
    WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE = 0x1D8C,
    WL_NV2A_K_SET_COLOR_CLEAR_VALUE = 0x1D90,
    WL_NV2A_K_CLEAR_SURFACE = 0x1D94, // z bit 0, stencil 1, red 4, green 5, blue 6, alpha 7
    WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL = 0x1D98, // x0 bits 15:0, x1 31:16 (inclusive)
    WL_NV2A_K_SET_CLEAR_RECT_VERTICAL = 0x1D9C,
    WL_NV2A_K_SET_SPECULAR_FOG_FACTOR = 0x1E20, // two: the final combiner's constants
    WL_NV2A_K_SET_COMBINER_COLOR_OCW = 0x1E40,  // eight
    WL_NV2A_K_SET_COMBINER_CONTROL = 0x1E60,
    WL_NV2A_K_SET_SHADOW_ZSLOPE_THRESHOLD = 0x1E68,
    WL_NV2A_K_SET_SHADOW_COMPARE_FUNC = 0x1E6C,
    WL_NV2A_K_SET_SHADER_STAGE_PROGRAM = 0x1E70, // five bits a stage
    WL_NV2A_K_SET_DOT_RGBMAPPING = 0x1E74,
    WL_NV2A_K_SET_SHADER_OTHER_STAGE_INPUT = 0x1E78,
    WL_NV2A_K_SET_TRANSFORM_DATA = 0x1E80,
    WL_NV2A_K_LAUNCH_TRANSFORM_PROGRAM = 0x1E90,
    WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE = 0x1E94, // bits 1:0: 0 fixed, 2 program
    WL_NV2A_K_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN = 0x1E98,
    WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD = 0x1E9C,
    WL_NV2A_K_SET_TRANSFORM_PROGRAM_START = 0x1EA0,
    WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD = 0x1EA4,
    WL_NV2A_K_METHODS = 0x2000
};

// ---- values -----------------------------------------------------------------------------------
// compare functions, blend factors and equations, cull faces, windings, polygon and stencil ops
// are OpenGL's numbers [envy-3d nv_3ddefs: gl_comparison_op, gl_blend_factor, gl_blend_equation,
// gl_cull_face, gl_front_face]
enum {
    WL_NV2A_FUNC_NEVER = 0x200,
    WL_NV2A_FUNC_LESS,
    WL_NV2A_FUNC_EQUAL,
    WL_NV2A_FUNC_LEQUAL,
    WL_NV2A_FUNC_GREATER,
    WL_NV2A_FUNC_NOTEQUAL,
    WL_NV2A_FUNC_GEQUAL,
    WL_NV2A_FUNC_ALWAYS
};
enum {
    WL_NV2A_BLEND_ZERO = 0x0000,
    WL_NV2A_BLEND_ONE = 0x0001,
    WL_NV2A_BLEND_SRC_COLOR = 0x0300,
    WL_NV2A_BLEND_ONE_MINUS_SRC_COLOR = 0x0301,
    WL_NV2A_BLEND_SRC_ALPHA = 0x0302,
    WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA = 0x0303,
    WL_NV2A_BLEND_DST_ALPHA = 0x0304,
    WL_NV2A_BLEND_ONE_MINUS_DST_ALPHA = 0x0305,
    WL_NV2A_BLEND_DST_COLOR = 0x0306,
    WL_NV2A_BLEND_ONE_MINUS_DST_COLOR = 0x0307,
    WL_NV2A_BLEND_SRC_ALPHA_SATURATE = 0x0308,
    WL_NV2A_BLEND_CONSTANT_COLOR = 0x8001,
    WL_NV2A_BLEND_ONE_MINUS_CONSTANT_COLOR = 0x8002,
    WL_NV2A_BLEND_CONSTANT_ALPHA = 0x8003,
    WL_NV2A_BLEND_ONE_MINUS_CONSTANT_ALPHA = 0x8004
};
enum {
    WL_NV2A_BLEND_EQ_ADD = 0x8006,
    WL_NV2A_BLEND_EQ_MIN = 0x8007,
    WL_NV2A_BLEND_EQ_MAX = 0x8008,
    WL_NV2A_BLEND_EQ_SUBTRACT = 0x800A,
    WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT = 0x800B,
    // the NV2A's signed forms [nxdk-regs]
    WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT_SIGNED = 0xF005,
    WL_NV2A_BLEND_EQ_ADD_SIGNED = 0xF006
};
enum { WL_NV2A_CULL_FRONT = 0x404, WL_NV2A_CULL_BACK = 0x405, WL_NV2A_CULL_FRONT_AND_BACK = 0x408 };
enum { WL_NV2A_FRONT_CW = 0x900, WL_NV2A_FRONT_CCW = 0x901 };
enum { WL_NV2A_SHADE_FLAT = 0x1D00, WL_NV2A_SHADE_SMOOTH = 0x1D01 };
// fog modes [nxdk-regs] [envy-3d nv20_fog_mode]
enum {
    WL_NV2A_FOG_EXP = 0x800,
    WL_NV2A_FOG_EXP2 = 0x801,
    WL_NV2A_FOG_EXP_ABS = 0x802,
    WL_NV2A_FOG_EXP2_ABS = 0x803,
    WL_NV2A_FOG_LINEAR_ABS = 0x804,
    WL_NV2A_FOG_LINEAR = 0x2601
};
// begin/end primitives [envy-3d nv10_begin_end]
enum {
    WL_NV2A_PRIM_END = 0,
    WL_NV2A_PRIM_POINTS,
    WL_NV2A_PRIM_LINES,
    WL_NV2A_PRIM_LINE_LOOP,
    WL_NV2A_PRIM_LINE_STRIP,
    WL_NV2A_PRIM_TRIANGLES,
    WL_NV2A_PRIM_TRIANGLE_STRIP,
    WL_NV2A_PRIM_TRIANGLE_FAN,
    WL_NV2A_PRIM_QUADS,
    WL_NV2A_PRIM_QUAD_STRIP,
    WL_NV2A_PRIM_POLYGON
};
// vertex array types [nxdk-regs] [envy-3d nv20_vtxbuf_type]: the low byte of an array format is
// also the Xbox Direct3D's D3DVSDT_* value (type | size << 4)
enum {
    WL_NV2A_VTX_UB_D3D = 0, // four bytes B, G, R, A (a D3DCOLOR) to r, g, b, a
    WL_NV2A_VTX_S1 = 1,     // signed shorts, normalized (/ 32767)
    WL_NV2A_VTX_F = 2,      // floats (size 7: x, y and w, z 0)
    WL_NV2A_VTX_UB_OGL = 4, // bytes in order, normalized (/ 255)
    WL_NV2A_VTX_S32K = 5,   // signed shorts as they are
    WL_NV2A_VTX_CMP = 6     // a word of 11, 11 and 10 signed bits, normalized
};
// surface formats [nxdk-regs]
enum {
    WL_NV2A_SURFACE_COLOR_X1R5G5B5_Z = 0x01,
    WL_NV2A_SURFACE_COLOR_X1R5G5B5_O = 0x02,
    WL_NV2A_SURFACE_COLOR_R5G6B5 = 0x03,
    WL_NV2A_SURFACE_COLOR_X8R8G8B8_Z = 0x04,
    WL_NV2A_SURFACE_COLOR_X8R8G8B8_O = 0x05,
    WL_NV2A_SURFACE_COLOR_X1A7R8G8B8_Z = 0x06,
    WL_NV2A_SURFACE_COLOR_X1A7R8G8B8_O = 0x07,
    WL_NV2A_SURFACE_COLOR_A8R8G8B8 = 0x08,
    WL_NV2A_SURFACE_COLOR_B8 = 0x09,
    WL_NV2A_SURFACE_COLOR_G8B8 = 0x0A,
    WL_NV2A_SURFACE_ZETA_Z16 = 1,
    WL_NV2A_SURFACE_ZETA_Z24S8 = 2,
    WL_NV2A_SURFACE_TYPE_PITCH = 1,
    WL_NV2A_SURFACE_TYPE_SWIZZLE = 2,
    // the format word's fields
    WL_NV2A_SURFACE_COLOR_MASK = 0x0000000Fu,
    WL_NV2A_SURFACE_ZETA_SHIFT = 4,
    WL_NV2A_SURFACE_TYPE_SHIFT = 8,
    WL_NV2A_SURFACE_AA_SHIFT = 12, // 0 one sample, 1 two across, 2 four (two by two)
    WL_NV2A_SURFACE_WIDTH_SHIFT = 16, // log2 of a swizzled surface's width
    WL_NV2A_SURFACE_HEIGHT_SHIFT = 24
};
// clear flags
enum {
    WL_NV2A_CLEAR_Z = 1u << 0,
    WL_NV2A_CLEAR_STENCIL = 1u << 1,
    WL_NV2A_CLEAR_R = 1u << 4,
    WL_NV2A_CLEAR_G = 1u << 5,
    WL_NV2A_CLEAR_B = 1u << 6,
    WL_NV2A_CLEAR_A = 1u << 7,
    WL_NV2A_CLEAR_COLOR = 0xF0u
};
// texture formats (SET_TEXTURE_FORMAT bits 15:8) [nxdk-regs] [envy-3d nv10_tex_format]: SZ
// swizzled, LU linear, L the DXT blocks
enum {
    WL_NV2A_TEX_SZ_Y8 = 0x00,
    WL_NV2A_TEX_SZ_AY8 = 0x01,
    WL_NV2A_TEX_SZ_A1R5G5B5 = 0x02,
    WL_NV2A_TEX_SZ_X1R5G5B5 = 0x03,
    WL_NV2A_TEX_SZ_A4R4G4B4 = 0x04,
    WL_NV2A_TEX_SZ_R5G6B5 = 0x05,
    WL_NV2A_TEX_SZ_A8R8G8B8 = 0x06,
    WL_NV2A_TEX_SZ_X8R8G8B8 = 0x07,
    WL_NV2A_TEX_SZ_I8_A8R8G8B8 = 0x0B, // indexed (P8)
    WL_NV2A_TEX_L_DXT1 = 0x0C,
    WL_NV2A_TEX_L_DXT23 = 0x0E,
    WL_NV2A_TEX_L_DXT45 = 0x0F,
    WL_NV2A_TEX_LU_A1R5G5B5 = 0x10,
    WL_NV2A_TEX_LU_R5G6B5 = 0x11,
    WL_NV2A_TEX_LU_A8R8G8B8 = 0x12,
    WL_NV2A_TEX_LU_Y8 = 0x13,
    WL_NV2A_TEX_LU_R8B8 = 0x16,
    WL_NV2A_TEX_LU_G8B8 = 0x17,
    WL_NV2A_TEX_SZ_A8 = 0x19,
    WL_NV2A_TEX_SZ_A8Y8 = 0x1A,
    WL_NV2A_TEX_LU_AY8 = 0x1B,
    WL_NV2A_TEX_LU_X1R5G5B5 = 0x1C,
    WL_NV2A_TEX_LU_A4R4G4B4 = 0x1D,
    WL_NV2A_TEX_LU_X8R8G8B8 = 0x1E,
    WL_NV2A_TEX_LU_A8 = 0x1F,
    WL_NV2A_TEX_LU_A8Y8 = 0x20,
    WL_NV2A_TEX_LC_CR8YB8CB8YA8 = 0x24, // YUY2
    WL_NV2A_TEX_LC_YB8CR8YA8CB8 = 0x25, // UYVY
    WL_NV2A_TEX_SZ_R6G5B5 = 0x27,
    WL_NV2A_TEX_SZ_G8B8 = 0x28,
    WL_NV2A_TEX_SZ_R8B8 = 0x29,
    WL_NV2A_TEX_SZ_DEPTH_X8_Y24_FIXED = 0x2A,
    WL_NV2A_TEX_SZ_DEPTH_X8_Y24_FLOAT = 0x2B,
    WL_NV2A_TEX_SZ_DEPTH_Y16_FIXED = 0x2C,
    WL_NV2A_TEX_SZ_DEPTH_Y16_FLOAT = 0x2D,
    WL_NV2A_TEX_LU_DEPTH_X8_Y24_FIXED = 0x2E,
    WL_NV2A_TEX_LU_DEPTH_Y16_FIXED = 0x30,
    WL_NV2A_TEX_LU_DEPTH_Y16_FLOAT = 0x31,
    WL_NV2A_TEX_SZ_Y16 = 0x32,
    WL_NV2A_TEX_SZ_A8B8G8R8 = 0x3A,
    WL_NV2A_TEX_SZ_B8G8R8A8 = 0x3B,
    WL_NV2A_TEX_SZ_R8G8B8A8 = 0x3C,
    WL_NV2A_TEX_LU_A8B8G8R8 = 0x3F,
    WL_NV2A_TEX_LU_B8G8R8A8 = 0x40,
    WL_NV2A_TEX_LU_R8G8B8A8 = 0x41,
    // SET_TEXTURE_FORMAT's fields
    WL_NV2A_TEXFMT_DMA_MASK = 0x3u, // 1: DMA A, 2: DMA B
    WL_NV2A_TEXFMT_CUBEMAP = 1u << 2,
    WL_NV2A_TEXFMT_BORDER_COLOR = 1u << 3,
    WL_NV2A_TEXFMT_DIMENSIONS_SHIFT = 4,
    WL_NV2A_TEXFMT_COLOR_SHIFT = 8,
    WL_NV2A_TEXFMT_LEVELS_SHIFT = 16,
    WL_NV2A_TEXFMT_SIZE_U_SHIFT = 20,
    WL_NV2A_TEXFMT_SIZE_V_SHIFT = 24,
    WL_NV2A_TEXFMT_SIZE_P_SHIFT = 28
};
// texture addressing (per coordinate, four bits) and filters [envy-3d nv4_tex_wrap,
// nv4_tex_min_filter, nv4_tex_mag_filter] [nxdk-regs: the NV2A's field positions]
enum {
    WL_NV2A_WRAP_REPEAT = 1,
    WL_NV2A_WRAP_MIRROR = 2,
    WL_NV2A_WRAP_CLAMP_TO_EDGE = 3,
    WL_NV2A_WRAP_BORDER = 4,
    WL_NV2A_WRAP_CLAMP = 5
};
enum {
    WL_NV2A_FILTER_NEAREST = 1,
    WL_NV2A_FILTER_LINEAR = 2,
    WL_NV2A_FILTER_NEAREST_MIP_NEAREST = 3,
    WL_NV2A_FILTER_LINEAR_MIP_NEAREST = 4,
    WL_NV2A_FILTER_NEAREST_MIP_LINEAR = 5,
    WL_NV2A_FILTER_LINEAR_MIP_LINEAR = 6,
    WL_NV2A_TEXFILTER_MIN_SHIFT = 16, // bits 23:16
    WL_NV2A_TEXFILTER_MAG_SHIFT = 24  // bits 27:24
};
// the texture shader's stage programs (five bits a stage) [nxdk-regs] [envy-3d nv20_tex_shader_op]
enum {
    WL_NV2A_STAGE_NONE = 0x00,
    WL_NV2A_STAGE_2D_PROJECTIVE = 0x01,
    WL_NV2A_STAGE_3D_PROJECTIVE = 0x02,
    WL_NV2A_STAGE_CUBE_MAP = 0x03,
    WL_NV2A_STAGE_PASS_THROUGH = 0x04,
    WL_NV2A_STAGE_CLIP_PLANE = 0x05,
    WL_NV2A_STAGE_BUMPENVMAP = 0x06,
    WL_NV2A_STAGE_BUMPENVMAP_LUMINANCE = 0x07,
    WL_NV2A_STAGE_BRDF = 0x08,
    WL_NV2A_STAGE_DOT_ST = 0x09,
    WL_NV2A_STAGE_DOT_ZW = 0x0A,
    WL_NV2A_STAGE_DOT_REFLECT_DIFFUSE = 0x0B,
    WL_NV2A_STAGE_DOT_REFLECT_SPECULAR = 0x0C,
    WL_NV2A_STAGE_DOT_STR_3D = 0x0D,
    WL_NV2A_STAGE_DOT_STR_CUBE = 0x0E,
    WL_NV2A_STAGE_DEPENDENT_AR = 0x0F,
    WL_NV2A_STAGE_DEPENDENT_GB = 0x10,
    WL_NV2A_STAGE_DOT_PRODUCT = 0x11,
    WL_NV2A_STAGE_DOT_REFLECT_SPECULAR_CONST = 0x12
};
// the transform's execution modes [nxdk-regs]
enum { WL_NV2A_XF_FIXED = 0, WL_NV2A_XF_PROGRAM = 2 };
// GET_REPORT / CLEAR_REPORT_VALUE types [nxdk-regs]
enum { WL_NV2A_REPORT_ZPASS_PIXEL_COUNT = 1 };
