# wl/brstm: NW4R audio readers (BRSTM, BRSAR, RWSD)

Target `wl_brstm` (alias `WiiLibs::brstm`). Portable C11, big-endian safe (every field is read with
`wl_rd_be*`, no struct casts). Links `wl_codec`, `wl_base` and libm; on host builds also `wl_host`, for
`wl_brsar_open(path)`.

| Header | What it reads |
|---|---|
| `brstm.h` | BRSTM streams ("RSTM"): parse, de-interleave a channel, decode, DSP loop state |
| `brsar.h` | BRSAR sound archives ("RSAR"): index of sounds, files, groups, items, banks, players, labels |
| `rwsd.h` | RWSD wave sound data: resolve one wave sound to its DSP-ADPCM frames and playback parameters |
| `dsp.h` | `wl_brstm_dsp_decode`: a wrapper over the shared DSP-ADPCM decoder in `wl/codec/dsp_adpcm.h` (included; integer, the same on every CPU) |

Every input is treated as untrusted: offsets, counts and sizes are range checked before use, and
malformed data gives `WL_ERR_FORMAT` / `WL_ERR_RANGE`, never a crash (the tests fuzz all three
formats). No code from any decomp or tool: the layouts below come from NW4R community notes,
checked against real files and by decoding.

## API overview

```c
WlBrstm b;
wl_brstm_parse(file, len, &b);                  /* header, chunks, block layout, DSP blocks */
wl_brstm_channel_frames(&b, file, len, ch, out); /* WL_BRSTM_FRAMES(b.samples) * 8 bytes */
wl_brstm_decode(&b, file, len, ch, pcm);         /* b.samples int16 samples */
wl_brstm_state(&b, file, len, &state, &notes);   /* init/loop ps and loop history from the audio */

WlBrsar a;
wl_brsar_open_mem(data, size, &a);               /* or wl_brsar_open_cb(read, user, size, &a) */
wl_brsar_open(path, &a);                         /* host builds only */
wl_brsar_find_ext(&a, "sound/x.brstm", &file_id, &sound_id);
wl_brsar_read_file(&a, read, user, file_id, max, &rwsd, &rn, &wave, &wn);
wl_brsar_free(&a);

WlRwsdWave w;                                    /* for a sound of type WL_BRSAR_SOUND_WAVE */
wl_rwsd_wave_sound(rwsd, rn, a.snd[i].info0, wave, wn, &w);
wl_brstm_dsp_decode(w.frames, w.frame_count, w.coef, &h1, &h2, pcm);
uint32_t vol = wl_rwsd_volume(a.snd[i].volume, w.note_volume); /* 256 = 1.0 */
```

## BRSTM layout (big-endian)

`0x00` magic `RSTM`, `0x04 u16` BOM `FEFF`, `0x06 u16` version (major 1), `0x08 u32` file size,
`0x0C u16` header size (0x40), `0x0E u16` chunk count (2+), `0x10`/`0x14` HEAD offset/size,
`0x18`/`0x1C` ADPC offset/size, `0x20`/`0x24` DATA offset/size. Chunks start with their tag and
size and are 32-byte aligned.

HEAD payload (from chunk + 8) starts with three references (8 bytes: marker `0x01000000`, offset
from the payload): stream info, track table, channel table.

Stream info: `+0 u8 codec` (2 = DSP-ADPCM; PCM8/PCM16 are refused), `+1 u8 loop`, `+2 u8 channels`,
`+4 u16 sample_rate`, `+8 u32 loop_start` (sample index; the loop runs to the last sample), `+0x0C
u32 sample_count`, `+0x10 u32` absolute offset of the audio, `+0x14 u32 block_count`, `+0x18 u32
block_size` (typically 0x2000), `+0x1C u32 block_samples` (block_size / 8 * 14), `+0x20 u32
last_block_size` (ADPCM bytes of the last block; a partial frame is cut after its header and the
bytes holding real samples), `+0x24 u32 last_block_samples`, `+0x28 u32 last_block_padded` (bytes
each channel occupies in the last block, 32-byte aligned), `+0x2C u32` seek interval, `+0x30 u32`
seek entry size (4).

Track table: not needed (one track per channel pair). Channel table: `u8 count` and a reference per
channel to a reference to the channel's 0x30-byte DSP block: `s16 coef[16]`, `u16 gain` (0), `u16
initial_ps`, `s16 hist1`, `s16 hist2`, `u16 loop_ps`, `s16 loop_hist1`, `s16 loop_hist2`, 2 pad.

Audio: `block_count - 1` blocks of `block_size` bytes per channel, channel 0 then channel 1 in
turn, then the last block with `last_block_padded` bytes per channel. Each channel is one plain
DSP-ADPCM frame stream; the predictor history runs on across blocks. The ADPC chunk is a seek table
(`u32` entries per channel per interval); it is range checked and not used.

`wl_brstm_state` takes the loop state (header byte of the loop frame, the two samples before the
loop point) from the audio, because that is what a DSP voice needs at the loop; `notes` reports
where the header's own copy disagrees (in well-formed files it never does).

## BRSAR layout (big-endian)

Header 0x40: `RSAR`, BOM, `u16` version (e.g. `0x0103`), `u32` size, `u16` header size (0x40),
`u16` chunks (3), then offset/size of SYMB (`0x10`), INFO (`0x18`) and FILE (`0x20`).

SYMB: tag, size, `u32` offset (from +8) of the string table = `u32 count` and one `u32` offset (from
+8) per NUL-terminated label (sound, player, group and bank names; the four lookup trees after it
are not used). Labels are kept in `WL_BRSAR_STR_MAX`-byte rows; longer ones are cut and counted in
`str_truncated`.

INFO payload (from chunk + 8) starts with six references (marker `0x01000000` + offset from the
payload): sounds, banks, players, files, groups and the sound-archive player info (not indexed). A
table is `u32 count` followed by references to the entries.

- Sound: `+0 u32 string id`, `+4 u32 file id`, `+8 u32 player id`, `+0x0C` reference to the 3D
  parameters (not indexed), `+0x14 u8 volume`, `u8 priority`, `u8 type` (1 sequence, 2 stream,
  3 wave), `u8 remote filter`, `+0x18` reference to the type's info (marker `0x010T0000`, T =
  type; a mismatch is counted). Info: sequence `u32 data id, u32 bank id, u32 track allocation, u8
  channel priority, u8 release priority`; stream `u32 start position, u16, u16`; wave `u32 wave
  sound index (in the RWSD), u32 track allocation, u8 channel priority, u8 release priority`.
- File: `+0 u32 size`, `+4 u32 wave data size`, `+8 u32`, `+0x0C` reference to an external path
  (null for files inside the archive; stream sounds name external `.brstm` files), `+0x14`
  reference to the list of groups holding it (`u32 count`, references to `(u32 group id, u32 item
  index)`).
- Group: `+0 u32 string id`, `+4 u32`, `+8` reference to an external path, `+0x10 u32 offset` and
  `+0x14 u32 size` (absolute), `+0x18 u32 wave data offset` and `+0x1C u32 wave data size`
  (absolute), `+0x20` reference to the item list (`u32 count`, references to items). Item: `u32
  file id, u32 offset, u32 size, u32 wave data offset, u32 wave data size, u32` (offsets relative
  to the group's).
- Bank: `u32 string id, u32 file id, u32`. Player: `u32 string id, u8 playable sound count, 3 pad,
  u32 heap size`.

The first four bytes of each file inside the archive (group offset + item offset) give its kind
(`RWSD`, `RSEQ`, `RBNK`, `RWAR`); the index reads just those four bytes per item. Wave data usually
lives in the groups' wave data areas rather than in `RWAR` files.

## RWSD layout (big-endian)

A wave sound reaches its audio through the BRSAR index: `info0` is the index of a wave sound entry
in the RWSD, `file_id` names that RWSD, the first group listing the file gives the item, the RWSD
bytes are at `group offset + item offset` and the audio at `group wave offset + item wave offset`
(`wl_brsar_read_file` does this).

Header 0x20: `RWSD`, BOM, `u16` version (`0x0102`), `u32` size, `u16` header size (0x20), `u16`
chunk count (2), `+0x10` DATA offset and size, `+0x18` WAVE offset and size. A reference is 8
bytes (first byte 1, `u32` offset in the second word).

DATA (offsets from chunk + 8): `u32 count`, one reference per wave sound entry. Entry: `+0`
reference to the info, `+8` to the track table (not used), `+0x10` to the note table. Info: `f32
pitch`, `u8 pan` (64 = centre), then surround, effect sends and null references (not used). Note
table: `u32 count` and a reference to the first note info: `s32 wave index`, `u8` attack, decay,
sustain, release, hold, pad, `u8 original key`, `u8 volume`, `u8 pan`, `u8 surround`, `f32 pitch`.

WAVE (offsets from the chunk start, not from +8): `u32 count`, one `u32` offset per wave to its
wave info: `u8 encoding` (2 = DSP-ADPCM; PCM8/PCM16 give `WL_ERR_UNSUPPORTED`), `u8 loop`, `u8
channels` (1; more is unsupported), `u8 rate high byte`, `u16 rate low`, `u8 location type` (0 =
offset into the wave data), pad, `u32 loop start`, `u32 end`, `u32 offset of the channel table`,
`u32 data location` (bytes from the start of the item's wave data). Loop start and end are DSP
nibble addresses (`frame * 16 + 2 + sample in frame`, the end inclusive), so the sample count is
`end_frame * 14 + end_nibble - 1` and the data is `end_frame + 1` frames. The channel table is one
`u32` per channel (offset from the wave info) to a channel info (`u32 0`, `u32` offset of the ADPCM
info, ...); the ADPCM info is the 0x30-byte DSP block described under BRSTM.

Derived parameters: `pitch = info pitch * note pitch` (`pitch_cents = round(1200 * log2(pitch))`),
`pan = round(((info_pan - 64) + (note_pan - 64)) * 127 / 64)` clamped to +-127, and
`wl_rwsd_volume(sound_volume, note_volume) = round(256 * sound_volume / 127 * note_volume / 127)`.

## Limits

- BRSTM: DSP-ADPCM only, one or two channels, 8 to 96 kHz, fewer than 2^28 samples per channel.
- BRSAR: index only; RSEQ sequences and RBNK banks are located but not parsed. Tables are capped at
  2^20 entries and 2^22 group items; SYMB and INFO at 64 MiB each.
- RWSD: mono DSP-ADPCM waves stored at an offset; the first note of an entry only; envelopes,
  surround and effect sends are not reported. RWSD files are capped at 1 MiB, waves at 2^24
  samples.
