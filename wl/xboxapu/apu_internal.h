// SPDX-License-Identifier: MIT
// The APU model's state, shared by apu.c (registers, front end, output) and apu_vp.c (the voice
// processor).
#pragma once

#include "wl/audio/adpcm.h"
#include "wl/base/endian.h"
#include "wl/xboxapu/apu.h"
#include "wl/xboxapu/apu_regs.h"

#define WL_APU_GLOBAL_LO 0x1000u // the global registers kept: 0x1000..0x4FFF
#define WL_APU_GLOBAL_HI 0x5000u
#define WL_APU_DSP_SLOTS 1024u // GP/EP memory words kept (a title touches few through the window)
#define WL_APU_SRC_MAX (WL_XBOX_APU_FRAME * 16u + 2u) // source samples a frame takes (step <= 16)
#define WL_APU_PEND_FRAMES 32u // voice channels' frames held before they are added to their bins
#define WL_APU_PEND_ADDS 32u   // additions a bin holds before they are made

// What the hardware keeps of a voice outside its record: the decoder's and resampler's state.
typedef struct {
    // the read cursor: the next sample to fetch (buffer: sample index; stream: index in the
    // segment)
    uint32_t rd;
    uint32_t seg;       // stream: segment within the list in use
    uint8_t ssl;        // stream: the list in use (0 = A, 1 = B)
    bool primed;        // s0 and s1 hold the samples at pos and pos1
    bool ending;        // s1 lies past a buffer's end: the voice ends when s0 gets there
    bool starving;      // a stream had no data for s1
    bool stalled;       // ... and has played up to there: silence until a segment list comes
    bool locked;        // VOICE_LOCK (kept; the processor runs under the caller's lock anyway)
    uint32_t pos, pos1; // the samples s0 and s1 are (pos is what CBO says)
    uint32_t pos_prev;  // the sample fetched before the last one (fill)
    uint32_t frac;      // Q16 position between s0 and s1
    int32_t s0[2], s1[2];
    uint32_t cbo_written; // CBO as last written: anything else is the guest moving the voice
    // ADPCM: the block decoded last, its 64 samples (interleaved by channel)
    uint32_t blk_addr; // buffer: byte offset of the block; stream: its guest-physical address
    uint32_t blk_next; // 0xFF: no block decoded
    int16_t blk_pcm[2u * WL_XBOX_ADPCM_BLOCK_SAMPLES];
    // the buffer page the last samples came from
    const uint8_t *page; // host address of its first byte, NULL: none
    uint32_t page_key;   // its index in the buffer's space
    uint32_t page_gen;   // apu->sge_gen when it was looked up (a page table write invalidates it)
    // a run of samples after the last one fetched that the next fetches read straight: 16-bit
    // little-endian PCM in a page or a segment, or the decoded ADPCM block (host order)
    const uint8_t *run;
    uint32_t run_left; // samples in it
    bool run_host;     // host-order int16 (the ADPCM block) rather than little-endian bytes
    // the stream segment being played: its table entry as last read
    uint32_t seg_entry; // table index (UINT32_MAX: none)
    uint32_t seg_gen;
    uint32_t seg_addr, seg_len, seg_container;
    const uint8_t *seg_data; // PCM: the whole segment's host address (NULL: look again)
    // segment lists as the methods gave them
    uint32_t ssl_base[2];
    uint8_t ssl_count[2];
    // pitch: the step for the last pitch seen
    int32_t pitch_seen;
    uint32_t step;    // source samples an output sample, Q16
    uint16_t gain[8]; // last frame's gains per target, Q15 (volume ramps start here)
    // the gains (Q15) worked out last from the volume words and the amplitude envelope's value
    // (vol_key: VOLA, VOLB, VOLC, the envelope; reused while they stay the same)
    bool vol_valid;
    uint8_t vol_act; // the targets whose gain is not 0 (bit t)
    uint32_t vol_key[4];
    uint16_t vol_gain[8];
} WlApuVoice;

struct WlXboxApu {
    WlXboxApuConfig cfg;
    uint32_t regs[(WL_APU_GLOBAL_HI - WL_APU_GLOBAL_LO) / 4u];
    uint32_t dsp_key[WL_APU_DSP_SLOTS]; // GP/EP window words: offset | 1, 0 = free
    uint32_t dsp_val[WL_APU_DSP_SLOTS];
    uint32_t ssl_page; // SET_SSL: the segment table entry 0x600.. writes land on
    uint32_t inbuf_sge, outbuf_sge;
    uint8_t hrtf_submix[4];
    bool trap_pending; // a trap raised in this run: stop after the frame
    bool list_notified; // a stream's segment list played out in this frame: stop after it
    uint32_t sge_gen;  // counts page table and segment table writes (voices' page caches)
    WlApuVoice voice[WL_XBOX_APU_VOICES];
    int32_t mix[WL_XBOX_APU_BINS][WL_XBOX_APU_FRAME];
    uint32_t mix_used; // bins written in the last frame (the others are 0)
    uint32_t mix_live; // apu_vp.c, in a frame: bins holding this frame's sum (the others: 0)
    uint32_t out_left, out_right;   // bins with a weight in the stereo mix's left, right side
    int32_t src[2][WL_APU_SRC_MAX]; // a voice's source samples for the frame (apu_vp.c)
    // Voices' frames waiting to be added to their bins (apu_vp.c): a bin's additions go in at
    // once, every frame added to it per sample (integer sums: the order does not matter). The
    // frames (a voice channel's 32 samples each, x 4) and each bin's additions (frame, gain << 15).
    int32_t pend_frame[WL_APU_PEND_FRAMES][WL_XBOX_APU_FRAME];
    uint32_t pend_frames;
    uint32_t pend_bins; // bins with additions waiting
    uint8_t pend_n[WL_XBOX_APU_BINS];
    struct WlApuPend {
        const int32_t *frame;
        int32_t gain;
    } pend[WL_XBOX_APU_BINS][WL_APU_PEND_ADDS];
    WlXboxApuStats stats;
};

// Empties the bins the last frame wrote (the others are 0 already).
static inline void wl_apu_clear_mix(WlXboxApu *apu) {
    for (uint32_t b = 0; apu->mix_used != 0; b++, apu->mix_used >>= 1) {
        if (apu->mix_used & 1u) {
            for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++) {
                apu->mix[b][i] = 0;
            }
        }
    }
}

// Global registers
static inline uint32_t wl_apu_reg(const WlXboxApu *apu, uint32_t off) {
    return apu->regs[(off - WL_APU_GLOBAL_LO) >> 2];
}
static inline void wl_apu_set_reg(WlXboxApu *apu, uint32_t off, uint32_t v) {
    apu->regs[(off - WL_APU_GLOBAL_LO) >> 2] = v;
}

// Guest-physical memory (NULL outside RAM, counted)
uint8_t *wl_apu_mem(WlXboxApu *apu, uint32_t phys, uint32_t bytes);
uint32_t wl_apu_rd32(WlXboxApu *apu, uint32_t phys);
void wl_apu_wr32(WlXboxApu *apu, uint32_t phys, uint32_t v);
// A voice's record (NULL when VPVADDR is not set up)
uint8_t *wl_apu_voice_rec(WlXboxApu *apu, uint32_t v);

// apu.c: a voice's notifier, the voice ending (VOICE_OFF), the idle-voice report
void wl_apu_notify_voice(WlXboxApu *apu, uint32_t v, uint32_t which);
void wl_apu_voice_off(WlXboxApu *apu, uint32_t v);
void wl_apu_idle_voice(WlXboxApu *apu, uint32_t v);
void wl_apu_voice_reset(WlApuVoice *vs);

// apu_vp.c: one frame of the voice processor into apu->mix
void wl_apu_vp_frame(WlXboxApu *apu);
// apu_vp.c: its tables (once, before the first frame; wl_xbox_apu_create calls it)
void wl_apu_vp_init(void);
