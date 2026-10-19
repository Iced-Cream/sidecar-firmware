# Sample RAM in PSRAM and DSP SAMPLE's playback modes (2026-10-09)

(Plugin code in PSRAM, the second code arena, and how PSRAM is shared out: [psram-testing.md](psram-testing.md).)

Groundwork for PLAN item 5a: the project's pool samples held whole in the
Teensy 4.1's PSRAM (2 x 8 MB APS6404L on the bottom pads), so DSP SAMPLE can
read them anywhere - slices, grains, a stretch, a wavetable scan - next to
today's streaming from the card, which stays as it is. Written and tested in
the simulator and compiled for the Teensy; nothing ran on PSRAM hardware yet
(the chips go on Monday: the checklist at the end).

## What is where

| | |
|---|---|
| `sim/sampler/sample_modes.h` | the seven playback modes on an int16 mono sample in RAM; shared (sim and Teensy) |
| `sim/sampler/sample_ram.h` | the sample RAM's allocator: blocks, LRU eviction, pins; shared |
| `sim/sampler/sampler_c.cc`, `build.sh` | libsampler for the simulator (with read counts and a model of the Teensy's data cache), libsampler_fast (without, for timing) |
| `sim/samplemodes.py` | the Python wrapper: `ModeVoice`, `SampleRam`, `choose_path` |
| `sim/obhost.py --psram` | the simulated host playing DSP SAMPLE from RAM in MOD's mode |
| `sim/samplebench.py` | the CPU and PSRAM estimate per mode, 8 voices |
| `sim/tests/` | 28 tests (`cd sim && sampler/build.sh && uv run python -m unittest discover tests`) |
| `teensy/setup_sampler_lib.sh` | the two shared headers as libraries/Sampler (run_sketch.sh runs it) |
| `core/pool/sample_ram_pool.*` | the loader (`SampleRamPool`): card -> the sample RAM (the Teensy's PSRAM), always built |
| `teensy/psram_test` | the memory test and bandwidth sketch for Monday |

## The switch, and the fallback

The sample RAM (`SampleRamPool`, core/pool/sample_ram_pool.h) is always
built; there is no longer a `-DRYTM_PSRAM=0` switch: without a region from
the board it stays off. At boot the core sets `external_psram_size` (MB; 0
on today's Teensy). (The "switch off" column below is history: the build
without it, measured when the switch existed.)

- **No PSRAM** (0): `SampleRamPool::create` returns null; no pool object, no
  engines, no Hann table are made; every new branch in voiceloop is behind
  `if (psram)`. DSP SAMPLE streams exactly as before and MOD (word 20) is
  ignored, as FLD was. The only difference is one boot log line ("PSRAM:
  none ..."). Memory against the build before (170817f):

  | | before | switch on | switch off |
  |---|---|---|---|
  | RAM1 free for locals | 121 824 | 121 728 (-96 B) | 121 824 |
  | RAM2 free for malloc | 41 248 | 41 248 | 41 248 |
  | ITCM code (5 x 32 KB blocks) | 147 272 | 157 608 (same blocks, 6.2 KB spare) | 147 272 |
  | flash code | 174 184 | 188 384 | 174 184 |

  Switch off: every symbol the same size as before and `render_block`'s
  code identical (only the linker's veneer order differs) [measured:
  arm-none-eabi-nm/objdump on both elf files].
- **PSRAM** (16 MB): the pool object, its bounce buffer and the voices' PLUCK
  lines live in PSRAM; 8 engines (3 KB) and the Hann table (4 KB) come from
  the heap (RAM2). A trig plays (`rytm::ChoosePath`): from RAM when the
  sample is resident; otherwise ONE and LOOP stream as today (LOOP looping
  forward) and GRAIN, SLICE, STRCH, PLUCK, WAVE stay silent while the sample
  loads (logged "waits for its sample in PSRAM").

The simulator mirrors it: `obhost.py` without `--psram` is today's voice
sample for sample (`test_fallback_ignores_mod`); with it, every pool sample
counts as resident.

## Format: int16 mono at the file's rate

Every sample is converted while it loads to int16, mono (the first two
channels averaged, as the streamed voice does), at the file's rate (the
engine resamples, as the stream does); 24-bit keeps its top 16 bits (the
voices' output to the Rytm is int16 anyway). 32-bit files are not loaded
(the card index keeps no format tag, and 32-bit WAVs are often float): they
stream as before, in ONE and LOOP only. One extra frame repeats
the last, so interpolation reads k + 1 without a check.

Why not float, why not stereo - PSRAM bandwidth. From the core (Teensyduino
1.62, cores/teensy4/startup.c, read): FlexSPI2 runs the PSRAM at 105.6 MHz
(CCM_CBCMR FLEXSPI2_PODF 4, CLK_SEL 3: 528 / 5), QPI SDR, read command 0xEB
with 6 dummy clocks; the MPU maps 0x7000_0000 write-back write-allocate
cached; two AHB RX buffers prefetch. Computed from that: the bus moves 4 bits
a clock, 52.8 MB/s at best; filling one 32-byte cache line costs 2 (command)
+ 6 (address) + 6 (dummy) + 64 (data) = 78 clocks = 0.74 us, ~0.8 us with
chip select and the AHB [inferred] - about 480 CPU cycles the core waits
(T_MISS). The cost of random access is these stalls, not the bus's MB/s:

- 8 voices x 4 grains (the grain budget, below) at the recorded pitch read
  32 streams x 48 000 frames/s x 2 B = 3.1 MB/s, well under the bus; but a
  32-byte line holds 16 int16 frames, so the streams cross 96 000 lines a
  second: ~7.7 % of the CPU stalled if each is a miss. At +12 semitones
  twice that.
- float32 frames: 8 a line, twice the misses (~15 %); stereo int16 the same.
  int16 mono is the cheapest per frame of audio, and holds 16 MB = 87 s at
  48 kHz (stereo or float: 44 s).
- What the engines actually miss depends on the 32 KB, 4-way data cache:
  `samplebench.py` runs every read through a model of it (all 8 voices
  interleaved block by block, nothing else competing: a lower bound).
  Streams of one-shots and grains miss about once per line; WAVE's cycles
  are re-read every period, so they stay cached if 8 voices x 2 cycles fit:
  600- and 1024-frame cycles do (1-21 k misses/s), 2048-frame cycles do not
  (580 k misses/s, ~47 % stalled for 8 voices).
- Block placement matters for the cache: 4 KB-aligned samples played at the
  same position map onto the same cache sets (the model showed WAVE 600
  thrashing until the samples were staggered); the allocator packs blocks
  on 32-byte lines instead.

## The allocator (sample_ram.h)

One contiguous block per resident sample (a voice reads a plain array; no
page table in the render loop). Blocks are kept in address order; a new one
takes the smallest free hole that fits; if none does, the window of
neighbouring blocks whose most recent use is oldest is evicted (fewer bytes
on a tie), never a pinned block (one a voice plays, or the one loading).
Fragmentation costs an extra eviction, never a compaction: moving megabytes
of PSRAM (~20-40 MB/s) would block the loop for 100s of ms. Up to 160
blocks (with 160, a want evicts the oldest even if a hole fits); prefetches
only take free holes. Tests: holes, LRU order, pins,
fragmentation (8 MB free in 1 MB holes, a 3 MB load evicts the oldest
single block between two holes and moves nothing), 3000 random operations
with the invariants checked after each.

Budget: 16 MB - 4 MB kept for later uses (recording buffers, the looper:
`kReserve`) - 72 KB (the bounce buffer, 8 PLUCK lines) - the pool object
= ~11.9 MB for samples, ~130 s of 48 kHz mono. A sample longer than 4 M
frames (87 s at 48 kHz) keeps its first 4 M frames (`kMaxFrames`; logged
"cut"): the random-access modes play that much of it, ONE and LOOP stream it
whole.

## The loader (core/pool/sample_ram_pool.*): scheduling

The same discipline as the stream refills (PLAN 3c): nothing touches the
card inside a render, and loop() does one bounded piece of card work a pass,
only while no request is waiting (`!Serial.available()`), after
`prepare_pending` (a sample change's head), and after any stream within 4096
frames (85 ms) of running dry: then one load step - an open, or one 8 KB read
ending on a sector, converted into PSRAM (~0.75 ms, as a refill) - else the
streams' ordinary top-ups. Loads are queued: a voice's want (its SMP slot in
a report) goes first and may evict, and a prefetch loading meanwhile gives
way (re-queued); at a project change the pool's slots are queued as
prefetches (free space only, the first 32), the old project's dropped. A
sample that cannot load (no room past the pinned ones, its format, the open)
is not retried before the next project. A sample longer than the arena
loads its first part. A voice holds a pin on the sample it plays from RAM
until it plays another, streams, changes machine, or the project changes
while it is silent. The 2-second log adds
`psram <used> of <size> KB, <n> loads, <n> evictions, max step <us>`.

Without PSRAM, loop() is exactly the old `refill_neediest(0xffffffff)`.

## The modes and the knobs (SD SAMPLE page)

DSP SAMPLE's second page (P9..P16, sample words 17..24) was TUN FIN SMP FLD
STA END LOP LEV with FLD unused. FLD becomes **MOD**, seven zones (the ENG
knobs' rule, `(v * 7) >> 7`): ONE LOOP SLICE GRAIN STRCH PLUCK WAVE. MOD 0
(ONE) is today's voice knob for knob, so existing sounds play the same.
MOD is taken at a trig; the rest are read every block (p-locks, the LFO).

| knob | ONE | LOOP | SLICE | GRAIN | STRCH | PLUCK | WAVE |
|---|---|---|---|---|---|---|---|
| TUN, FIN | pitch | pitch | pitch | grain pitch | pitch (not time) | pitch | pitch |
| note | transposes | transposes | **picks the slice** (C4 = 1st) | transposes | transposes | transposes | transposes |
| SMP | pool slot | | | | | | |
| MOD | 0..18 | 19..36 | 37..54 | 55..73 | 74..91 | 92..109 | 110..127 |
| STA | start | loop start | region start | **position** | start | burst start | **scan position** |
| END | end (< STA: reverse) | loop end (< STA: reverse) | region end (< STA: slices reversed) | grain size 10 ms..0.5 s | end (< STA: reverse) | decay 50 ms..10 s | - |
| LOP | >= 1 loops | < 64 forward, >= 64 ping-pong | slices 2 4 6 8 12 16 32 64 | density 2..256 grains/s | speed 0 (freeze)..64 (1x)..127 (2x) | brightness | cycle 256 512 600 1024 2048 4096 |
| LEV | level | | | | | | |
| note-off | plays out | fades 10 ms | plays out | fades | fades | rings out | fades |

Details: SLICE fades 2 ms in and out against clicks. GRAIN sprays a quarter
of a grain; its output is normalised by the average overlap. STRCH is a
WSOLA stretch: 43 ms Hann grains at 50 % overlap, each placed within +-192
frames where it best continues the sounding grain (coarse to fine; plain
overlap-add shifted a 440 Hz tone to 454 Hz at half speed). PLUCK is
Karplus-Strong with its loop filter's delay taken out of the line length,
so it is in tune (+-1 %, tested at 110/220/440 Hz). WAVE plays C4 = 261.6 Hz
at note 60, crossfading the two cycles around STA. The pitch reference
everywhere is the trig's note against 60, as the streamed voice.

The Rytm side (rytm-mods 0021/0023) needs: word 20's label MOD and its value
text (the seven names in seven zones), ideally the per-mode labels of STA END
LOP above. Until then the knob says FLD and its value is the mode.

## CPU (estimated; to measure on hardware)

`sim/samplebench.py`, 8 voices, the costliest settings. "Teensy" is host
time scaled by the measured ratio of two voices already on the Teensy (8
JUNO 27.3 %, 8 PLAITS WAVETABLE 49.0 %: x58..x152 - code that vectorizes
on the Mac, as JUNO, gives the high factor; scalar table code as the grains
the low one); "stall" the model cache's misses at 0.8 us each. SLICE and
ONE +12 end within the run, so their averages are low: while sounding they
cost as ONE.

| 8 voices | Teensy compute | PSRAM misses/s | stall | total |
|---|---|---|---|---|
| ONE | 4.9..12.9 % | 26 k | 2.1 % | 7..15 % |
| LOOP ping-pong | 4.8..12.7 % | 26 k | 2.1 % | 7..15 % |
| GRAIN, all 32 grains busy | 15.9..41.8 % | 40 k | 3.2 % | 19..45 % |
| same, +12 semitones | 15.7..41.3 % | 144 k | 11.5 % | 27..53 % |
| STRCH 1x | 16.0..42.1 % | 24 k | 1.9 % | 18..44 % |
| STRCH 0.5x, +12 | 16.0..42.1 % | 107 k | 8.5 % | 25..51 % |
| PLUCK | 2.6..6.8 % | 0 | 0 | 3..7 % |
| WAVE 600 / 1024 | 9..24 % | 1 k / 21 k | 0.1 / 1.7 % | 9..26 % |
| WAVE 2048 | 9.3..24.4 % | 581 k | 46.5 % | 56..71 % |

Plus the resonator when MIX > 0 (DSP SAMPLE's STRING 3.4 %, MODAL ~7 % a
voice, measured 2026-10-09). What keeps it in check:
- **grain budget**: 32 grains for all voices together (`kGrainBudget`; up to
  8 a voice): 8 dense clouds cost what 32 grains cost. Grains run as frame +
  fraction with a fixed-point window phase into a 1024-point Hann table (no
  double math, no interpolation of the window: 19.7 -> 7.2 ns a voice's
  sample on the host).
- STRCH's alignment search coarse to fine: under 2 multiplies a sample.
- WAVE: cycles up to 1024 frames for 8 voices; 2048/4096 for a few voices.

Target "8 voices < ~50 %": met by every mode but WAVE 2048 and dense GRAIN /
STRCH pitched up, which sit at the limit by these estimates.

## Monday checklist (PSRAM soldered)

1. Continuity and shorts under a loupe; 3.3 V on both chips' VCC.
2. `teensy/run_sketch.sh psram_test 120` and keep the output:
   - "PSRAM: 16 MB" (8: the second chip not seen - its CS pin (EMC_22) or
     its own joints);
   - the memory test passes on all 8 patterns (errors list the address and
     the bits: one chip's range, or one data line's bit everywhere);
   - bandwidth: sequential read / write / memcpy MB/s; **random line read
     ns** -> put it in `samplebench.py`'s T_MISS (0.8 us assumed) and rerun
     the bench;
   - access patterns: the PSRAM-minus-RAM "stall" per case against the
     estimates above (GRAIN 4 grains x8, WAVE 600/1024/2048).
3. Several cold boots (power off/on): size and the memory test each time.
4. Optional: the FlexSPI2 clock up (startup.c's commented settings, 132 MHz:
   PODF 3, SEL 3) only if the memory test passes at it several times; else
   keep 105.6 MHz.
5. `teensy/run_sketch.sh voiceloop` with a project whose pool fits: the boot
   log's "PSRAM: 16 MB, ~12 160 KB for samples", then "PSRAM: <name> in" per
   sample (load ms: ~11 MB/s expected from the refills' 8 KB in 0.75 ms) and
   the 2 s "psram" line (max step under ~1 ms).
6. `sim/stress.py` scenarios c, c2, d, dl with PSRAM: no dropouts, the cpu
   line as before (ONE from RAM should cost no more than streaming); then
   MOD per mode with 8 voices (needs the Rytm side's MOD, or FLD set by
   hand): the "load dsps" share against the CPU table above.
7. Compare the load's step time and the "max request" figure against 3c's
   (the loader shares the gaps with the refills).

## Open questions

- MOD on the Rytm: label and value text in rytm-mods (and per-mode labels for
  STA END LOP, or leave them fixed and document).
- Note-offs: LOOP, GRAIN, STRCH and WAVE fade on the gate closing; the
  Rytm's AMP envelope shapes the voice anyway - keep, or let them run until
  the next trig (CPU while silent)?
- Should a stream fallback play for GRAIN/SLICE while the sample loads (as
  ONE), rather than silence?
- Prefetch policy: all of a project's slots (up to 32 queued) at a change,
  or only those the pattern uses (needs the Rytm's kit/pattern)?
- The 4 MB reserve and kMaxFrames: right for the looper's buffers?
- Cache: the voices' own state, the resonators and the other machines share
  the 32 KB D-cache with the samples - the model ignores them; Monday's
  numbers decide whether WAVE 1024 holds up in a mixed set.
- A sample's rate: kept, resampled on play as the stream; converting to 48
  kHz at load would save nothing on the read side.
