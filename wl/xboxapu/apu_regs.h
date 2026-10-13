// SPDX-License-Identifier: MIT
// The MCPX APU's register offsets, front end method numbers and voice record layout.
//
// These are numbers about the hardware, gathered from public descriptions and checked against the
// accesses a title's own DirectSound makes (Halo: Combat Evolved, XDK 3911, logged on the host with
// the MMIO trace of wl/xboxrt):
//   - xboxdevwiki, "APU": the frame (32 samples at 48 kHz), the 256 voices, the 0x80-byte voice
//     record, the mixbins, the pitch law p = 4096 log2(f / 48000), the envelope stages and their
//     1500 Hz counter, the voice lists;
//   - XboxDev's xboxpy (apu_regs.py, apu.py) and JayFoxRox's xbox-tools (inspect_apu_vp.py): the
//     register offsets, the voice record's fields and the volume and bin packing, as numbers;
//   - the DirectSound code itself: which methods it sends, what its interrupt handler reads.
// Names are this project's own.
#pragma once

// ---- Global registers (offsets from 0xFE800000) -----------------------------------------------

#define WL_APU_ISTS 0x1000u        // interrupt status, write one to clear
#define WL_APU_IEN 0x1004u         // interrupt enables (bit 0: the line at all)
#define WL_APU_IRQ_GLOBAL 0x01u    // ISTS bit 0: reads as "something enabled is pending"
#define WL_APU_IRQ_FE_TRAP 0x10u   // the front end trapped (FECTL says why)
#define WL_APU_IRQ_FE_NOTIFY 0x20u // a notifier was written
// a voice notifier was written (DirectSound's DPC checks its voices)
#define WL_APU_IRQ_FE_VOICE 0x40u

#define WL_APU_FECTL 0x1100u          // front end control
#define WL_APU_FECTL_MODE 0x000000E0u // 0 runs, 0x80 halted, 0xE0 trapped
#define WL_APU_FECTL_TRAPPED 0x000000E0u
#define WL_APU_FECTL_REASON 0x00000F00u // why it trapped; 0xF00 = a requested trap
#define WL_APU_FECV 0x1110u             // the current voice (SET_CURRENT_VOICE)
#define WL_APU_FEAV 0x1118u             // the antecedent voice: handle 0..15, list 16..17
#define WL_APU_FENADDR 0x115Cu          // notifiers: guest-physical address
#define WL_APU_FEDECMETH 0x1300u        // the method the front end decoded last (a trap's)
#define WL_APU_FEDECPARAM 0x1304u       // and its parameter
#define WL_APU_FEMEMADDR 0x1324u        // a guest-physical address ...
#define WL_APU_FEMEMDATA 0x1334u        // ... a write here stores its value there (a fence)
#define WL_APU_FETFORCE1 0x1504u        // bit 15: an idle voice in a list traps the front end
#define WL_APU_FETFORCE1_IDLE_VOICE 0x8000u

#define WL_APU_SECTL 0x2000u     // setup engine control
#define WL_APU_SECTL_COUNT 0x18u // the sample counter's mode: 0 stopped (the processor too)
#define WL_APU_XGSCNT 0x200Cu    // the global sample counter
#define WL_APU_VPVADDR 0x202Cu   // the voice records (256 x 0x80 bytes), guest-physical
#define WL_APU_VPSGEADDR 0x2030u // buffer voices' page table: 8 bytes an entry, word 0 the page
#define WL_APU_VPSSLADDR 0x2034u // stream voices' segment table: 8 bytes an entry
#define WL_APU_GPSADDR 0x2040u   // the GP's scratch memory: its page table
#define WL_APU_GPFADDR 0x2044u   // the GP's FIFO memory: its page table
#define WL_APU_TVL2D 0x2054u     // first voice of the 2D list (0xFFFF: empty)
#define WL_APU_CVL2D 0x2058u     // the voice being processed
#define WL_APU_NVL2D 0x205Cu     // the one after it
#define WL_APU_TVL3D 0x2060u     // the same for the 3D list
#define WL_APU_TVLMP 0x206Cu     // and the multipass list
#define WL_APU_LIST_STRIDE 0x0Cu
#define WL_APU_NO_VOICE 0xFFFFu

#define WL_APU_GP_BASE 0x30000u // the GP DSP's memories and registers
#define WL_APU_GPRST 0x3FFFCu   // bit 0: out of reset, bit 1: the DSP core runs
#define WL_APU_EP_BASE 0x50000u // the EP DSP's
#define WL_APU_DSP_SIZE 0x10000u

// ---- Front end methods (written at 0x20000 + method) -------------------------------------------

#define WL_APU_PIO_BASE 0x20000u
#define WL_APU_PIO_FREE 0x010u         // read: free slots in the method FIFO
#define WL_APU_M_NOTIFY 0x104u         // write the front end's notifier (index 0)
#define WL_APU_M_SET_ANTECEDENT 0x120u // handle 0..15, list 16..17 (0 = after the handle)
#define WL_APU_M_VOICE_ON 0x124u       // handle 0..15, EF start stage 24..27, EA 28..31
#define WL_APU_M_VOICE_OFF 0x128u
#define WL_APU_M_VOICE_RELEASE 0x12Cu
#define WL_APU_M_VOICE_PAUSE 0x140u     // handle 0..15, bit 18: pause
#define WL_APU_M_SET_SSL 0x190u         // the segment table entry 0x600.. write to (8-byte units)
#define WL_APU_M_SUBMIX_HEADROOM 0x200u // 32 words, one a bin
#define WL_APU_M_HRTF_HEADROOM 0x280u
#define WL_APU_M_HRTF_SUBMIXES 0x2C0u // the bins 3D voices' first four targets go to, a byte each
#define WL_APU_M_SET_CURRENT_VOICE 0x2F8u
#define WL_APU_M_VOICE_LOCK 0x2FCu
#define WL_APU_M_VOICE_SSL_A 0x320u // count 0..7, first segment 8..31
#define WL_APU_M_VOICE_SSL_B 0x35Cu
#define WL_APU_M_VOICE_PITCH 0x37Cu    // pitch in bits 16..31
#define WL_APU_M_VOICE_BUF_BASE 0x3A0u // byte offset of the buffer in the page table's space
#define WL_APU_M_VOICE_BUF_LBO 0x3A4u  // loop start, samples
#define WL_APU_M_VOICE_BUF_CBO 0x3D8u  // play position, samples
#define WL_APU_M_VOICE_BUF_EBO 0x3DCu  // last sample
#define WL_APU_M_SSL_SEGMENTS 0x600u   // 0x600..0x7FF: segment table words
#define WL_APU_M_SSL_SEGMENTS_END 0x800u
#define WL_APU_M_INBUF_SGE 0x804u      // page table entry to write
#define WL_APU_M_INBUF_SGE_ADDR 0x808u // its page address (bits 12..31)
#define WL_APU_M_OUTBUF_SGE 0x1800u
#define WL_APU_M_OUTBUF_SGE_ADDR 0x1808u
#define WL_APU_M_IDLE_VOICE 0x8000u // what the voice processor reports for an idle voice

// ---- The voice record (0x80 bytes at VPVADDR + handle * 0x80, little-endian words) -------------

#define WL_APU_VOICE_SIZE 0x80u
#define WL_APU_V_VBIN 0x00u                     // targets 0..5: bins at bits 0, 5, 10, 16, 21, 26
#define WL_APU_V_FMT 0x04u                      // targets 6, 7 at bits 0, 5; format below
#define WL_APU_FMT_SPB(f) (((f) >> 16) & 0x1Fu) // also the multipass voice's source bin
#define WL_APU_FMT_MULTIPASS 0x00200000u
#define WL_APU_FMT_PERSIST 0x00800000u // a stream voice waits for data instead of ending
#define WL_APU_FMT_STREAM 0x01000000u  // data type: segments (1) or a buffer (0)
#define WL_APU_FMT_LOOP 0x02000000u
#define WL_APU_FMT_CLEAR_MIX 0x04000000u // a multipass voice empties its source bin
#define WL_APU_FMT_STEREO 0x08000000u
#define WL_APU_FMT_SAMPLE(f) (((f) >> 28) & 3u)    // U8, S16, S24, S32
#define WL_APU_FMT_CONTAINER(f) (((f) >> 30) & 3u) // 8-bit, 16-bit, ADPCM, 32-bit
#define WL_APU_CONTAINER_ADPCM 2u
#define WL_APU_V_ENV0 0x08u // EA attack 0..11, EA delay 12..23, EF pitch scale 24..31 (signed)
#define WL_APU_V_ENVA 0x0Cu // EA decay 0..11, EA hold 12..23, EA sustain 24..31
#define WL_APU_V_ENV1 0x10u // EF attack, delay, filter scale
#define WL_APU_V_ENVF 0x14u // EF decay, hold, sustain
#define WL_APU_V_MISC 0x18u // EF release 0..11
#define WL_APU_V_HRTF 0x1Cu
#define WL_APU_V_BASE 0x20u  // buffer: byte offset 0..23; stream: segment list A
#define WL_APU_V_LBO 0x24u   // buffer: loop start 0..23
#define WL_APU_V_ECNT 0x34u  // EA count 0..15, EF count 16..31
#define WL_APU_V_STATE 0x54u // paused 18, new 20, active 21, EF stage 24..27, EA stage 28..31
#define WL_APU_STATE_PAUSED 0x00040000u
#define WL_APU_STATE_NEW 0x00100000u
#define WL_APU_STATE_ACTIVE 0x00200000u
#define WL_APU_V_CBO 0x58u  // play position 0..23, EA level 24..31
#define WL_APU_V_EBO 0x5Cu  // buffer: last sample 0..23 (stream: segment list B); EF level 24..31
#define WL_APU_V_VOLA 0x60u // target volumes 0 and 1 at bits 4 and 20, 12 bits each
#define WL_APU_V_VOLB 0x64u // 2 and 3; volumes 6 and 7 are the low nibbles of A, B, C
#define WL_APU_V_VOLC 0x68u // 4 and 5
#define WL_APU_V_LFO_ENV 0x6Cu    // EA release 0..11
#define WL_APU_V_PITCH_LINK 0x7Cu // next voice of the list 0..15, pitch 16..31 (signed)

// Envelope stages (EA: amplitude; EF: pitch and filter)
#define WL_APU_ENV_OFF 0u
#define WL_APU_ENV_DELAY 1u
#define WL_APU_ENV_ATTACK 2u
#define WL_APU_ENV_HOLD 3u
#define WL_APU_ENV_DECAY 4u
#define WL_APU_ENV_SUSTAIN 5u
#define WL_APU_ENV_RELEASE 6u
#define WL_APU_ENV_FORCE_RELEASE 7u

// Notifiers: 16 bytes each at FENADDR; the front end's own first, then 4 a voice from index 2
// (0: segment list A done or the voice ended, 1: segment list B done). Byte 15 is the status.
#define WL_APU_NOTIFIER_SIZE 16u
#define WL_APU_NOTIFIER_VOICE_BASE 2u
#define WL_APU_NOTIFIERS_PER_VOICE 4u
#define WL_APU_NOTIFY_DONE 0x01u

// Stream segment table entry: word 0 the guest-physical address, word 1 samples 0..15, container
// 16..17
#define WL_APU_SEG_SAMPLES(w) ((w) & 0xFFFFu)
#define WL_APU_SEG_CONTAINER(w) (((w) >> 16) & 3u)
