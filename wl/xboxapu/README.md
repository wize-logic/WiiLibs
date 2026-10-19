# wl/xboxapu: the Xbox's audio hardware as device models

Target `wl_xboxapu` (alias `WiiLibs::xboxapu`) depends on `wl_audio_core` (the IMA step) and
`wl_base`. The MCPX's audio processing unit (`apu.h`) and AC'97 controller (`ac97.h`) as devices a
recompiled title's own DirectSound programs, so its sound plays without replacing a line of the
title's code. Guest memory through a callback, no platform calls, integer only: the same output on
little- and big-endian hosts (the tests run on ppc-be). `wl/xboxrt` puts them behind 0xFE800000 and
0xFEC00000, ticks them and sends the sound to the platform's output (`wl/xboxrt/README.md`).

A clean implementation, written from public descriptions of the hardware (xboxdevwiki's APU page,
XboxDev's xboxpy register list, Intel's AC'97 bus master interface) and from what a title's own
DirectSound does with it, logged on the host (`WL_XB_AUDIO_TRACE`); `apu_regs.h` lists the facts
and where they come from.

## The APU (`apu.h`)

`wl_xbox_apu_create(&cfg)` (`cfg.phys_ptr` gives the host address of guest-physical memory),
`wl_xbox_apu_read`/`write` for the 512 KB register window, `wl_xbox_apu_irq` for the interrupt
line, and `wl_xbox_apu_run(apu, frames, out)`, which runs frames of 32 samples at 48 kHz and writes
the stereo result (`cfg.on_frame` sees each frame's 32 mixbins and output: checksums, captures).

What the model does:

- **Registers**: interrupt status (write one to clear) and enables, the front end's control
  (running, halted, trapped and why), the setup engine's control and the sample counter (32 a
  frame while it counts), the addresses of the voice records, the buffer page table, the stream
  segment table, the notifiers and the GP/EP DSPs' page tables, the three voice lists. The fence
  (a write to FEMEMDATA stores its value at FEMEMADDR). The method FIFO always has room.
- **Front end methods** (0x20000 + method), run as they are written: the current and antecedent
  voice, every field of the current voice's record (formats, targets, volumes, envelopes, pitch,
  buffer base, loop start, position, end), VOICE_ON (links the voice into a list, starts its
  envelopes), VOICE_OFF, VOICE_RELEASE, VOICE_PAUSE, the buffer page table and the segment table
  entries, a stream voice's segment lists A and B, the HRTF submix bins, the front end's notifier.
- **The voice processor**, once a frame: walks the 2D, 3D and multipass lists in guest memory;
  for each active voice steps its two envelopes (delay, linear attack, hold, decay at the law
  xboxdevwiki gives, sustain, an exponential release to -60 dB; counts of 16 frames a unit),
  fetches samples (8-bit unsigned, 16-, 24- and 32-bit PCM through the 4 KB buffer pages, or a
  stream's segments; Xbox ADPCM, 36 bytes a channel and 64 samples a block, decoded with
  `wl/audio`'s IMA step, its difference kept in 16 bits), resamples them by its pitch (2^(p/4096) source samples a sample, linear
  interpolation; the pitch envelope adds its share), and adds them to its eight target bins at
  their volumes (12 bits of 1/64 dB; ramped over the frame). Stereo voices send even targets the
  left and odd ones the right; a 3D-list voice's first four targets go to the HRTF submix bins; a
  multipass voice plays a bin of the frame. Loops wrap at the end to the loop start; a buffer
  without one ends, its voice goes off. A stream plays list A's segments, then B's, notifying each
  list as it is done; with no segments left a persistent stream waits in silence, another ends.
  The play position goes back into the record (DirectSound's GetCurrentPosition reads it).
- **Notifiers and interrupts**: a voice's notifier (16 bytes at the notifier address, 4 a voice
  from index 2: status 1 in byte 15) when it ends or a segment list is done, with the voice and
  notify interrupt bits; an idle voice found in a list traps the front end (FECTL trapped,
  FEDECMETH 0x8000, FEDECPARAM the voice) when FETFORCE1 asks for it, which is how DirectSound
  takes finished voices out of its lists. The run stops after a frame that trapped or played out a
  stream's segment list, so the guest's interrupt handler answers (and gives the stream its next
  list) before the next frame; the caller runs the frames left after the interrupt.
- **The DSPs**: their memories and registers keep what is written (a title reads effect
  parameters back). Their programs do not run. DirectSound's effects program takes commands from a
  word in the GP's scratch memory (byte 0x810 of the pages GPSADDR maps) and DirectSound waits for
  it to be cleared (Halo hangs in its init otherwise); the model clears it once a frame while the
  GP runs (`cfg.gp_command_word`, 0x810 for the XDK's DirectSound; Wreckless, another XDK, polls the
  same offset).
- **The output**: the GP and EP programs would mix the 32 bins into speakers, add the effects and
  encode for the AC'97. In their place a fixed stereo mix (`cfg.mix_left`/`mix_right`, Q15 weights):
  front left and right at 1.0, centre at 0.707 and LFE at 0.5 to both, rear left and right at 0.707
  to their side, the four 3D crosstalk bins (6..9) to theirs.

What it does not do:

- No effects: the GP's program (I3DL2 reverb, chorus, the effect chain DirectSound's image sets
  up, the effect sends in bins 10..31) and the EP's (Dolby Digital encoding, S/PDIF) are not run,
  so environmental reverb and every effect a title routes through the sends are missing; 3D voices
  get their volumes' panning but no HRTF filtering (the HRIR methods are ignored).
- No per-voice filters (DLS2 low-pass, parametric EQ), no LFOs, no voice or submix headroom (the
  GP's program would scale it back), no multipass output buffers.
- The voice processor runs only while the front end runs freely (as on the console while
  DirectSound edits its lists); the sample counter and the frame timing follow the caller's ticks.

## The AC'97 controller (`ac97.h`)

The primary codec's registers (ready: the powerdown register's low bits stay set), the global
control and status (the primary codec ready bit; `cfg.codec_ready`), the codec access semaphore
(always free), and the bus masters: PCM out (0x110) and S/PDIF out (0x170) run through their
buffer descriptor lists at 48 kHz stereo (`wl_xbox_ac97_advance`), setting buffer-complete and
last-valid-buffer status, stopping at the last valid buffer until a new one is given (an LVI write
that names the current buffer again leaves it halted), and raising
the interrupt line their control bits enable. What the descriptors point at is not read: the APU's
mix is the sound.

## Cost

`tests/xboxapu/bench_xboxapu.c` plays a scene shaped like what Halo plays in a level: 26 voices a
frame, most of them mono ADPCM streams near 22 kHz sent to three bins, stereo ADPCM loops at
44.1 kHz, 16-bit PCM voices with five or six targets, a few silent, some moving, and one of each
rarer case. On 32-bit PowerPC (-O2 -mcpu=750, the Wii's code; instructions counted under qemu with
`tools/qemu_icount.py`) it costs about 50 000 instructions a frame, 1900 a voice (90 000 before
the voice processor was made for the Wii: ADPCM a block at a time through one table, gains worked
out only when they change, the bins' additions made together up to eight voices a pass, a
multiply-high for each), 75 million a second at 1500 frames; on the host (x86-64, -O2) about
4 us a frame. Its checksum over 3000 frames (a ctest) pins the bins and the output bit for bit.
`BENCH_VOICES=mute` plays the same voices with every target silent and `BENCH_VOICES=pause` pauses
them: 49 400 instructions a frame playing, 24 200 silent (930 a voice: a silent voice is not
interpolated or mixed, but its samples are still fetched and its ADPCM decoded, to keep its
position, its stream's notifications and the two samples it resumes from exact) and 2 200 paused
(85 a voice). In the game (Dolphin, the profile's log) 12 to 17 voices a frame cost 20 to 29 us,
3 to 4% of the processor at 1500 frames a second. Since 2026-10-18 (ppc-be -O3): 47 600 playing,
22 900 silent, 1 600 paused (from 49 250, 24 000, 2 200): ADPCM's table holds each entry's next row
as a pointer and its clamp is one compare, the stereo mix sums two bins a pass, and a voice's held
gains are queued without the ramp checks. Since 2026-10-19: 45 800 playing, 22 300 silent, 1 540
paused: a voice whose volume, envelope, target and format words are the last frame's (and whose
gains held then) queues the additions kept from that frame (its plan) without working out its
targets and gains again; the waiting additions of every bin are made in one call (their loops'
registers saved once); the lists' current/next voice registers are written once a list. Later
on 2026-10-19 (44 600 playing; Halo's shapes a10 37 800 -> 36 200): a paused voice is passed over in
the list walk; a frame whose samples all lie in the voice's run (a decoded block, a page or a
segment of 16-bit PCM) takes them without fill's loop; both envelopes off is checked once; a
stream's buffer words are not read; the plan's words are compared in place. All of it is off with
`WlXboxApuConfig.plain`. `WlXboxApuConfig.plain` (the runtime: a file `apuplain` in
the title folder, `WL_XB_APU_PLAIN=1` on the host; the bench: `BENCH_PLAIN=1`) runs the earlier
paths, for an A/B: the same bins and output, bit for bit (`bench_xboxapu_plain`).

In Halo's levels (deterministic host runs, 2026-10-19: a10's bridge, a30 and d40 from their checkpoints,
60-90 s of play each) a frame has 17-22 playing voices and 13-24 paused ones in the lists: 70-83%
mono ADPCM streams near 22 kHz into two 3D submixes and the effect send (3 additions each), 8-20%
stereo ADPCM streams at 44.1 kHz to front left and right, 10% silent 16-bit loops; no envelopes; 98% of
the voice frames repeat the last frame's additions (plan), 1% move a gain; 65-69% of the frames'
samples lie in one run; 40-55 additions a frame over 5 bins (the two submixes and the effect send
11-17 each). The bench's `BENCH_VOICES=a10`, `a30` and `d40` play those shapes.

In Halo (a deterministic host run of a10's menu and opening, 100 s of sound) a frame has 8.7 voices:
77% ADPCM, 70% streams, a fifth with a silent source; 1.8 ADPCM blocks are decoded a frame, and a
block's bytes come again in 9% of decodes within 64 blocks, 15% within 1024: too few for a decode
cache to pay for its lookups.

## Tests

`tests/xboxapu/test_xboxapu.c` (host and ppc-be), DirectSound's method sequences in a synthetic
guest memory: 16-bit PCM through scattered pages reaching its bins exactly, volumes in 1/64 dB,
8/24/32-bit and stereo formats, pitch at half and double rate, a loop's wrap and a one-shot's end
(position, notifier, interrupt), the idle-voice trap and the guest's answer, Xbox ADPCM against
`wl_xbox_adpcm_decode_block` (64 samples a block, blocks across pages), a stream's two segment
lists (each run ending with the list it played out), its starvation and refill, the amplitude envelope's stages, a multipass voice, the GP's
command word and the fence, a 3D voice's submix bins, and the AC'97 bus master's descriptors,
status and interrupt, and its halt at the last valid buffer. `bench_xboxapu_checksum` (host and ppc-be): the benchmark's scene for 3000
frames, its checksum of every frame's bins and output against the one the voice processor made
before it was reworked for speed.
