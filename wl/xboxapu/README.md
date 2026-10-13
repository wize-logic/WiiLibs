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
the stereo result.

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
  `wl/audio`'s IMA step), resamples them by its pitch (2^(p/4096) source samples a sample, linear
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
  takes finished voices out of its lists. The run stops at a trap so the guest's interrupt handler
  answers before the next frame.
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
last-valid-buffer status, stopping at the last valid buffer until a new one is given, and raising
the interrupt line their control bits enable. What the descriptors point at is not read: the APU's
mix is the sound.

## Cost

Measured with a benchmark of the model (a mix of stereo ADPCM at 44.1 kHz, mono ADPCM at 22 kHz and
PCM16 voices): on the host (x86-64, -O2) about 0.3 us a voice a frame; on 32-bit PowerPC (-O2
-mcpu=750, the Wii's code; instructions counted under qemu) about 1200 instructions a frame plus
3500 a voice. Halo's main menu plays three voices (its music, a stereo ADPCM stream at 44.1 kHz,
and two silent loops DirectSound keeps running, which cost little: a silent frame is not mixed):
11 800 instructions a frame, 18 million a second, a few percent of the Wii's 729 MHz.

## Tests

`tests/xboxapu/test_xboxapu.c` (host and ppc-be), DirectSound's method sequences in a synthetic
guest memory: 16-bit PCM through scattered pages reaching its bins exactly, volumes in 1/64 dB,
8/24/32-bit and stereo formats, pitch at half and double rate, a loop's wrap and a one-shot's end
(position, notifier, interrupt), the idle-voice trap and the guest's answer, Xbox ADPCM against
`wl_xbox_adpcm_decode_block` (64 samples a block, blocks across pages), a stream's two segment
lists, its starvation and refill, the amplitude envelope's stages, a multipass voice, the GP's
command word and the fence, a 3D voice's submix bins, and the AC'97 bus master's descriptors,
status and interrupt.
