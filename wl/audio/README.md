# wl/audio

Software audio for Wii homebrew: decoders, a 32-voice mixer, a sample player, a streamed-music
player, a DS sequence (SSEQ/SBNK/SWAR) synthesiser and a small music track engine, over the
`wl/platform` audio output (AESND, ASND fallback on the console; a render-it-yourself stub on the
host).

| Target | Contents | Platforms |
|---|---|---|
| `wl_audio_core` (`WiiLibs::audio_core`) | `adpcm` (IMA here, DSP-ADPCM from `wl_codec`), `mixer`, `fmt`, `synth*`: integer only, no platform calls, bit-identical output on little- and big-endian CPUs | everywhere |
| `wl_audio` (`WiiLibs::audio`) | `audio` (engine), `stream` (PWST player), `music_engine` | wherever `wl_platform` builds |

## Output path

- The Wii's audio interface runs at a fixed 48 kHz and takes PCM only; AESND's DSP resamples each
  voice from its own rate. Everything here is mixed in software (sequences need per-channel
  envelopes, pitch and modulation that hardware voices lack, and one path is easier to test).
- One mixer on the audio thread (LWP priority 100) produces 32 kHz stereo PCM16 in chunks of up to
  256 frames and feeds one AESND stereo stream voice through a ring of three 288-frame blocks.
- AESND copies voice buffers in 1152-byte steps (2 ms at its 144 kHz maximum rate) and pads a short
  buffer with silence: a 256-frame block would play 32 frames of silence every block and run 11 %
  slow. Hence the 288-frame (1152-byte, 9 ms) hardware block, built from two fill calls of 144.
- An AESND stream voice must be started looped, or the engine stops it at the end of its first
  buffer even when the callback queued another. The callback fires when a buffer has been copied
  out; a buffer set inside it plays gaplessly. The callback runs in interrupt context and only
  moves pointers; mixing happens on the audio thread.
- ASND (fallback) runs its voices once per 1024 output samples and takes one queued buffer per call:
  1024-frame blocks, ring of four. `AUTO` starts AESND and falls back to ASND when AESND consumed
  no block within 600 ms.
- Latency: three AESND blocks (27 ms) plus up to one block between a command and its effect.

## Engine (`audio.h`)

- Main-thread API; commands reach the mixer through a 256-entry lock-free SPSC ring (never a mutex
  on the audio thread). Handles are assigned on the main thread, so play calls return at once; a
  full ring drops the command (counted in `cmd_drops`). `wl_audio_sync` waits for the mixer.
- Memory: the mixer and streams from `WL_TAG_AUDIO`, decoded samples from `WL_TAG_AUDIO_BANK`
  (`wl_mem_init` first). Streams read through the IO thread of `wl/platform/fs.h`; call
  `wl_audio_pump` once per frame.

## Mixer and buses (`mixer.h`)

32 voices, 4 buses, integer only. Gains are Q14 and ramp linearly across each block (no zipper
noise); a stereo source at unity on a unity bus is the identity. Mono sources use a constant-power
pan, stereo sources a balance. Pitch is a Q16 step (2^(i/96) table with interpolation), linear
interpolation, up to 4x. Stealing: lowest priority, then oldest, never above the newcomer's
priority. Output saturates (counted in `clipped`).

| Bus | Typical content | Default |
|---|---|---|
| Music | sequences and streams | 80 % |
| Effects | sound effects | 100 % |
| Speech | voices, callouts: while one plays, music ducks by 6 dB (`wl_audio_speech_duck`; attack a quarter per block, release 1/32) | 100 % |
| UI | menu clicks | 70 % |

`wl_audio_duck_music` is an explicit duck (jingles).

## DSP-ADPCM (`adpcm.h`)

`adpcm.h` includes the shared decoder in `wl/codec/dsp_adpcm.h` (`WlDspState`, `wl_dsp_decode`,
`wl_dsp_decode_frame_from`; see `wl/codec/README.md`), so `wl_audio_core` links `wl_codec`. The
GameCube/Wii DSP format (also inside BRSTM, DSP files and the PWST/PWSF containers): 8-byte
frames of 14 samples. Header byte: predictor pair index (high nibble, 0..7) and scale exponent (low
nibble), then 14 signed 4-bit samples, high nibble first.
`s = clamp16((nibble * (2048 << scale) + 1024 + c0 * h1 + c1 * h2) >> 11)` with `h1`, `h2` the
previous two outputs and `(c0, c1)` the pair (8 pairs per channel, units of 1/2048). The decoder
accumulates in 64 bits and supports a partial-frame start (mid-frame loops). Also the DS flavour of
IMA-ADPCM (low nibble first; the caller parses the block header).

## PWST and PWSF containers (`fmt.h`)

Small big-endian wrappers around DSP-ADPCM data. Readers bounds-check every field (files are
untrusted). Byte 0x04 is a u16 version (0 or 1 accepted).

**PWST** (stream, magic `PWST`). Header 0x30 bytes: `0x06 u16 flags` (bit 0 loops; bit 1 the loop
starts mid-frame), `0x08 u8 channels` (1 or 2), `0x09 u8 codec` (0 = DSP-ADPCM), `0x0C u32 rate`,
`0x10 u32 samples` (per channel, a multiple of 14), `0x14 u32 loop_start`, `0x18 u32 loop_end`
(inclusive; 0 without a loop; loop_start is a multiple of 14 unless flag bit 1), `0x1C u32
data_off`, `0x20 u32 data_size` (all channels, stored one after the other). Then one 48-byte channel
block per channel: `s16 coef[16]`, `u16 gain` (0), `u16 initial_ps`, `s16 hist1`, `s16 hist2`,
`u16 loop_ps`, `s16 loop_hist1`, `s16 loop_hist2`, 2 reserved. With a mid-frame loop, `loop_ps` is
the header of the frame holding `loop_start` and the loop history is the one before it, as the DSP
hardware resumes at a nibble address.

**PWSF** (sample, magic `PWSF`). Header 0x40 bytes: `0x06 u16 flags` (as PWST), `0x08 u8 channels`
(1), `0x09 u8 codec` (0 DSP-ADPCM, 1 PCM16 big-endian), `0x0A u8 bus` (0 music, 1 effects, 2
speech, 3 UI), `0x0B u8 priority` (64 default), `0x0C u32 rate`, `0x10 u32 samples`, `0x14 u32
loop_start`, `0x18 u32 loop_end` (inclusive), `0x1C u32 data_off` (0x80), `0x20 u32 data_size`,
`0x24 s16 pitch` (cents), `0x26 u16 volume` (256 = 1.0), `0x28 s8 pan`, `0x2A u16 play_ms` (0 =
whole sample), `0x2C u32 duration_ms` (informational). At 0x40 one channel block as in PWST (zero
for PCM16). Data at 0x80, padded to 32 bytes. Samples are decoded to PCM16 at load.

## Stream player (`stream.c`)

A `WlAudioSpan` (file path, offset, size) names the PWST bytes: a whole file or an uncompressed
archive entry. 32 KiB reads per channel go to the IO thread into a ring of three blocks per channel
(192 KiB per stereo stream; at most three streams: two for a crossfade plus a jingle). The ring
holds compressed frames; the mixer thread decodes on demand. Playback starts with about one second
buffered. The producer reads the "virtual stream" (up to the frame holding `loop_end`, then from the
frame holding `loop_start`, forever); at the jump the decoder takes `loop_ps` and the loop history
and resumes at `loop_start % 14`, giving sample-exact intro-to-loop and mid-frame loops. Loops
shorter than 2048 bytes of ADPCM are refused (use the sample player). Late data plays silence
(`starved_frames`) and resumes exactly where it stopped. Crossfade = a new stream with
`fade_in_ms` plus `wl_audio_stop(old, ms)`.

## SSEQ synthesiser (`synth.h`)

A re-implementation of the Nintendo DS sequence player written from community format notes. One
`WlSynth` (about 9 KiB) plays one sequence on its own 32 voices; `wl_audio_seq_play` runs one as a
mixer pull voice (at most 6). The song is the SDAT sub-files as stored (little-endian SSEQ, SBNK and
up to four SWAR), read through byte accessors and kept alive by the caller until the voice is gone.

- **Time**: 192 Hz engine frames (166.67 samples at 32 kHz, Bresenham-integral). A tick is 1/48
  quarter note: the tempo is added per frame and a tick fires per 240 (tempo 120 = 96 ticks/s).
  Envelopes, LFO, pitch and gains update per frame and ramp across it.
- **Commands**: variable-length numbers (7 bits per byte, most significant first); prefixes A0
  (random operand), A1 (variable operand), A2 (conditional). Notes 00-7F, rest 80, program 81,
  open track 93, jump/call/return 94/95/FD, loops D4/FC, end FF, pan C0, volume C1/D5/C2,
  transpose/bend/range C3/C4/C5, priority C6, note wait C7, tie C8, portamento C9/CE/CF, LFO
  CA-CD/E0, ADSR overrides D0-D3, tempo E1, sweep E3, variables B0-B6, compares B8-BD. Unknown
  commands end the track (counted); a track running 4096 commands in one tick is cut.
- **Instruments** (SBNK): single PCM/ADPCM, PSG square (duty) and noise (15-bit LFSR), drum sets, key
  splits. **Waves** (SWAR/SWAV): PCM8, PCM16, IMA-ADPCM with loops (decoder state at the loop start
  is restored at every wrap, as the hardware does).
- **Interpretation** (not verified against hardware): levels are 400·log10(v/127) tenths of dB per
  volume stage, summed with the envelope; pan is the hardware's linear law; attack/decay/release
  rates follow the DS tables; pitch in 1/64 semitone; vibrato a quarter sine cycle per 8 frames at
  speed 16; PSG key 69 = 440 Hz.
- Voice stealing: releasing voices first, then lowest priority, then oldest.

## Music track engine (`music_engine.h`)

Caches tracks (SSEQ songs or PWST spans) delivered by an application `WlMusicLoader`, plays them by
role (BGM, jingle, SFX, UI) and frees a track only after its voice left the mixer. It exposes a
`WlMusicBackend` function table for the application's own music logic (what plays when).

## Tests

`tests/audio`: decoders against reference implementations from the format text, the mixer
(including a fixed-scenario digest), the SSEQ checks (also on big-endian ppc-be, label `be`; the
2-second piece digest must not change unintentionally), and the engine on the host stub (the test
renders blocks itself, so loops, fades and starvation are exact). All content is synthetic.
