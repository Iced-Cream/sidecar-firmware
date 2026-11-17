# WAVETABLE: a band-limited wavetable synth machine for the Sidecar (Pi)

## In short

- A polyphonic wavetable machine on the Pi engine (`core/wavetable/`, Pi and Mac only: `CORE_MSAMP` builds; the Teensy
  is frozen and has none). Machine flag **0x28** (`core/machines.h` `kWaveMachine`); until the Rytm's 0021 has the
  machine, `wt=voice:V` plays it on voice V whatever built-in machine the Rytm reports.
- Tables: TBL is a row of the project's **WAVETABLES** POOL list (docs/POOLS.md): six built-ins generated here (BASIC
  PWM VOCAL ORGAN FM SYNC, 64 frames each, nothing third-party; the template puts them in A001..A006), or any WAV /
  AIFF on the card (by its hash) in the open format Serum, Vital and others write: N single-cycle frames of 2048
  samples, or the frame size its `clm ` chunk names, or 1024 / 512 / 256, or one cycle of any length (AKWF's 600), or
  any longer WAV cut into 2048-sample frames.
- Every frame is held as 10 mip levels an octave apart (harmonics 512 >> k): a note reads the level whose top harmonic
  stays under 0.6 fs, so nothing aliases audibly at any pitch [measured: a C7 saw's aliases -67 dB under its
  harmonics, against -14 dB reading level 0; C2..C8 all under -67 dB up to 18 kHz].
- Per note: up to 7 unison oscillators (stereo summed to the voice's mono return), **two tables** (A and B, each with
  its own frames and POS, played at once in every lane), a warp (nine: SYNC PWM FM FORMANT QUANT BEND SQUEEZE RM), a sub
  sine, a filter (the DJ-style SVF, low-pass below CUT 64 and high-pass above, or LP / BP / HP at 12 and 24 dB, or a
  ladder) with drive, an amp ADSR and a mod envelope on both POS. A trig is one note: **chords come from a Sidecar poly
  track** (its sequencer notes, the live pads, the keyboards), not from the machine; trigs, the sequencer and the live
  pads / keys all play it.
- Derived in part from Vital by Matt Tytel (GPL-3.0-or-later): the new warps and filters (section 9: the Pi binaries carry
  its licence).
- CPU [measured: Mac; inferred: Pi]: the boot bench's note (3 oscillators, SYNC) ~1.0 % of a Pi 4 core; a 6-note
  chord of 7 oscillators each with the filter ~7.5 %, with table B, BEND and the ladder ~CPU_CHORD_B %; Pi 5 about 2.5x less.

Reference feel: the Elektron Tonverk's wavetable machine (public information only: a wavetable oscillator with
position morphing, warps and unison into a filter); nothing of its code, tables or exact parameter set is used.

## 1. Tables

| source | frames | notes |
|---|---|---|
| built-in BASIC | 64 | sine -> triangle -> saw -> square (their series crossfaded) |
| built-in PWM | 64 | a pulse from 50 % to 4 % |
| built-in VOCAL | 64 | a 110 Hz voice's harmonics under three formants, a -> e -> i -> o -> u |
| built-in ORGAN | 64 | harmonics 1..48 coming in one by one, drawbar-like levels |
| built-in FM | 64 | a sine phase-modulated by its 2nd harmonic, index 0 .. 6 |
| built-in SYNC | 64 | a saw hard-synced at 1..8x |
| a sample row of the WAVETABLES list | up to 256 | any WAV / AIFF the reader takes (below), anywhere on the card; more frames are thinned evenly (first and last kept) |

The WAV reader (`core/wavetable/wav_read.h`, also GRANULAR's): RIFF/WAVE with `fmt ` / `data` / `clm ` anywhere; PCM
8/16/24/32-bit, IEEE float 32/64-bit, `WAVE_FORMAT_EXTENSIBLE`; any channel count (the first two summed); any rate
(a table's frames are cycles: the rate does not matter). A file is at most 8 MB.

Frame size (`tables.h detect_frame_size`, in this order): the `clm ` chunk's `<!>N` when N divides the length; 2048
when it divides; 1024, 512, 256 when one does; a file of at most 8192 samples is one cycle; else 2048 (the rest left
out). A 2048-sample frame keeps its first 512 harmonics (they reach 20 kHz down to 39 Hz).

Each frame's harmonics come from an FFT (any power of two) or a DFT (other lengths); DC is removed; the table is scaled
so its loudest frame peaks at 0.95. Loading runs between blocks (`wt::step()`): one 32 KB card read or one frame's
mip levels a call, so a 256-frame table is in after ~260 steps. A trig while its table loads waits for it (up to
250 ms, else it plays at once on the table the voice had).

### The list (the pools build, 2026-10-22)

TBL (the 14-bit word) is `code = bank * 128 + row` of the WAVETABLES list: 8 banks x 128 rows, row 000 OFF in every
bank, an empty or MISSING row silent. TBL's default is 1 since rui1043 (A001 = BASIC in the template). Rows are filled
from the Rytm (FUNC + YES in every bank: `@BUILT-IN/` and the card's folders; the POOL page), the touchscreen (the
POOL view, FILES, FILL), the web (Files: **+WT**, Fill bank; the Pool tab) or the console (`pool=wt:...`). Card tables
load on use into 4 card slots (least recently used replaced, never one a voice reads); their memory is set aside at
boot and shown apart in the pool RAM figure ("tables"). See docs/POOLS.md.

## 2. Band limiting

| level k | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| top harmonic | 512 | 256 | 128 | 64 | 32 | 16 | 8 | 4 | 2 | 1 |
| samples | 2048 | 1024 | 512 | 512 | 512 | 512 | 512 | 512 | 512 | 512 |

- A note (each unison oscillator's highest rate, the warp's top rate folded in: SYNC's ratio, PWM's steepest slope,
  FM's 1 + index) reads the first level whose top harmonic x f stays under **0.6 fs** (28.8 kHz): whatever lies over
  Nyquist then aliases above 0.4 fs (19.2 kHz), out of hearing. A note too high even for level 9 is silent.
- Reads are 4-point Hermite with wrap guards stored around each level (no modulo in the loop). Levels 3..9 keep 512
  samples, so the high notes' strong low harmonics are read at 8 samples a harmonic or more and the read's images sit
  near its nulls [measured: with 256-sample levels key 60 aliased at -51 dB, with 512 every key is under -67 dB].
- Memory: 7200 floats (28 KB) a frame; a card slot holds 256 frames (7.4 MB), four slots, the built-ins 11 MB; all
  from the board's high heap (`msamp_ram_alloc`), taken once at boot.

## 3. Parameters

P1..P16 are sound words 9..16 and 17..24 (page 1 A..H, page 2 A..H), each 0..127 with its 1/128 fraction.

| P | name | what | range |
|---|---|---|---|
| 1 | TBL | table A: a row of the WAVETABLES list (A001..A006 the built-ins, then the card's by path) | 14-bit code, OFF = row 000 |
| 2 | POS | the frame position of A (smoothed 10 ms); neighbouring frames crossfaded | 0 first .. 127 last |
| 3 | WARP | the warp, 9 zones (value x 9 >> 7) | OFF SYNC PWM FM FORMANT QUANT BEND SQUEEZE RM |
| 4 | AMT | the warp's amount | per warp, below |
| 5 | UNI | unison: 0 one oscillator; up, pairs fade in (3, 5, 7 oscillators) while the detune opens (both tables) | 0 .. 60 cents (x^1.5) |
| 6 | SUB | a sine an octave down | level 0..0.8 |
| 7 | CUT | DJ: below 64 low-pass, 64 open, above high-pass; 50 % key tracking. The other types: a plain cutoff | DJ: LP 20 Hz .. 20 kHz, HP 20 Hz .. 8 kHz; others 8 .. 136 semitones above C-1 (13 Hz .. 21 kHz) |
| 8 | RES | resonance, and the drive into the filter with it | DJ: 0 .. near self-oscillation, drive 1..4x; others: Vital's laws |
| 9 | ATK | attack (amp and mod envelope) | 1 ms .. 10 s, exponential |
| 10 | DEC | decay to SUS; also the release (a Minimoog's DEC/REL) | 1 ms .. 10 s (-60 dB) |
| 11 | SUS | sustain | 0 .. 1 |
| 12 | ENV | the mod envelope (attack ATK, decay DEC, to 0) onto POS **and POS B** | 64 none, 127 +the whole table, 0 -the whole |
| 13 | TBL B | table B: the same list; row 000 (OFF, the default) is no B | 14-bit code |
| 14 | POS B | the frame position of B | 0 first .. 127 last |
| 15 | MIX | A against B, equal power (cos / sin of 0..90 degrees); with no B, nothing | 0 only A .. 127 only B |
| 16 | FTYPE | the filter, 8 zones (value x 8 >> 7) | DJ LP12 LP24 BP12 BP24 HP12 HP24 LADDER |

The warps (AMT laws; SYNC, PWM and FM keep this engine's, the others are Vital's):

| WARP | AMT 0 .. 127 |
|---|---|
| OFF | none |
| SYNC | the phase read 1x .. 8x faster, restarting each cycle (hard sync) |
| PWM | the cycle's first half stretched: pulse width 50 % .. 4 % |
| FM | phase modulation, index 0 .. 5 rad (AMT squared); the modulator is **table B when B is on**, a sine at the note's rate otherwise |
| FORMANT | sync ratio 1/16 .. 16x (2^((a - 0.5) x 8)), under a half-sine window (a formant, not a sweep) |
| QUANT | the phase quantised into steps: many (AMT 0: none) .. few (AMT 127: the staircase collapses) |
| BEND | the phase bent by a cubic: AMT 64 is the straight phase, below / above push it either way |
| SQUEEZE | the cycle squeezed toward its start (AMT 0) or its middle (AMT 127); AMT 64 is straight |
| RM | the table multiplied in by a window: 1 + (m - 1) x AMT with m the modulator as in FM (table B, or a sine) |

With table B on, the warp acts on A and on B, except FM and RM: there B is the modulator of A, and B is heard unwarped
under MIX. Reading B when MIX is 0 under FM or RM still happens (it modulates), so MIX 0 plus FM is "A modulated by B,
B not heard". B shares the note's unison lanes, rates and pitch, and its own random start phases. Old projects' WARP
values land in other zones now (nine zones, not four: an old SYNC at 40 is PWM now); P13..P16 of an old project were
the chord (CHD INV SPR VOI): a CHD value reads as a TBL B row now (CHD 1 = A001 = BASIC as B, MIX 0: nothing audible
until MIX moves), INV / SPR are POS B / MIX, VOI an FTYPE zone: they are all zero on a project without a chord, and
those sound as before.

The filters (FTYPE): DJ is the engine's own (CUT as before: a project with FTYPE 0 sounds as before, sample for sample
with B off). The others are Vital's: a digital state-variable filter (12 dB, or two stages for 24 dB: LP, BP, HP by blend;
resonance 0.5 .. 16 Q by RES cubed, drive compensated) and its ladder (4 stages, LP). CUT is semitones: 8 + CUT x 128/127
above C-1, plus 50 % key tracking (0.5 semitone a semitone from C4) plus the **filter envelope** (the mod envelope x
FENV x 64 semitones). The oscillators play into them at half scale (Vital's levels: hardTanh's linear range), the level
made up after, so the passband keeps the DJ filter's. The 24 dB HP's first stage is Vital's blend through a resonance-1
stage, so it falls 18 dB / octave below its cutoff (its 24 dB shows above and in the cascade).

Decisions: page 1 is the oscillator and filter (TBL POS WARP AMT UNI SUB CUT RES, as asked); page 2 is the amp envelope
ATK DEC SUS (DEC also the release), the mod envelope's depth ENV onto both POS, and TBL B, POS B, MIX, FTYPE where the
chord was (owner, 2026-11-17: "chords we can already do from a poly track"). The LFO is the Sidecar's own (`core/lfo.h`):
any of P1..P12 can be its target, POS above all (P13..P16 are not LFO targets). Velocity: 0.25 + 0.75 v^2.

**The filter envelope's depth has no knob and no store.** The brief put it on the touchscreen's WAVETABLE page, but the
project keeps no per-track, per-machine store for a touch-only setting (the stores are the LFO / INSERT / XY / scene
files, each its own kind; none takes a machine's number). So it is the engine's `fenv` per voice, 0 at boot, set from
the console (`wt=fenv:V:PERCENT`, -100..100, 100 % = +64 semitones at the envelope's peak) and **not saved**; with 0 the
new filters have no envelope. A store for it (a line in a kit's record) is a decision for a later task.

## 4. Voice

```
note -> 7 phase accumulators (unison detune, own random phases; the centre one starts at 0), once for table A and
        once for table B (B only when it is on and MIX or the warp needs it)
     -> warp of the phase (SYNC / PWM / FM / FORMANT / QUANT / BEND / SQUEEZE / RM; FM and RM modulate A by B's lane,
        or by a sine at the note's rate while B is off)
     -> Hermite read of frames A and B' at the level, each table's frame pair crossfaded by its POS (+ ENV x the mod env)
     -> A x cos(MIX) + B x sin(MIX) -> sum x 1/sqrt(sum w^2) -> DC blocker (4 Hz)
     -> + SUB -> DJ: drive -> SVF (LP / HP)     |  the others: x 0.5 -> SVF 12 / 24 dB or ladder -> x 2
     -> x amp envelope x velocity x 0.5
voice: the notes summed x the headroom gain 1/sqrt(sum of envelopes) (as MULTISAMPLE's, smoothed 3 / 15 ms)
     -> voices.cpp's soft limiter (soft_out) -> the voice's Overbridge return
```

- Notes: 8 a voice, 32 in all; steal: a free note, then the voice's oldest released, then its oldest. A trig releases
  the voice's earlier trig notes (their tails ring) and starts the chord; 0x11 releases them; live pad notes (a bound
  MIDI channel) and the sequencer's (channel 16) are held until their own note-off.
- Cores: the control core posts a note's start / release / kill through a sequence-locked mailbox (as msamp's); the
  render core reads the voice's tables A and B through a hazard slot each (`sel` / `use`, `selb` / `useb`), so a table a
  render core reads (either one) is never reloaded under it, and a card slot a voice's B holds is not given to another
  table. A B that is not in yet is silent (A plays alone) until it loads. No allocation after `begin()`.
- Chords: the live pads' chord detection writes the Rytm track's chord words (a 28 01) for 6-OP FM and the sample
  machines; for WAVETABLE it does not (P13..P16 would be overwritten): the pads' notes play as notes. The sequencer's
  chord steps, the pads and the keyboards reach the voice as `live_on` notes (`poly::seq_on`, `key_on`).

## 5. CPU

`./core/build/render_wavegrain --bench` runs the Pi's boot bench (`pi/engine/bridge.cpp bench`: voice 0, note 48, a
retrigger on 55, 1 s of audio) on the Mac; the Pi 4's measured numbers for the other machines (`docs/pi-engine.md`
"Boot bench") give the ratio, 8.1..8.4x (5.9..12.3x across machines) [measured: Mac 2026-10-20; inferred: Pi].

| case | Mac (% of a core) | Pi 4 1.5 GHz (estimate) | Pi 5 (estimate, ~2.5x) |
|---|---|---|---|
| bench note (`notes.cpp` wt: 3 osc, SYNC, sub, LP) | 0.12 % | ~1.0 % | ~0.4 % |
| 1 note, 1 osc | 0.04 % | ~0.3 % | ~0.15 % |
| 1 note, 7 osc + FM warp + filter | 0.16 % | ~1.3 % | ~0.5 % |
| 6-note chord, 7 osc each + filter | 0.90 % | ~7.5 % (5..11 %) | ~3 % |

The Pi's bench now runs `wt` and `gr` too (`pi/engine/cores.cpp`, `pi_run.py bench`): its boot log has the measured
numbers once a kernel with this branch boots. Boot: the built-ins take 24 ms on the Mac (~0.2 s on a Pi 4).

## 6. Console and the Rytm side

- `wt` stats; `wt=voice:V` / `wt=voice:V:off` (the override); `wt=list` (the tables in the log); `wt=rescan` (failed
  tables tried again); `wt=pick:V:ROW` (writes the Rytm's TBL through 48). The list itself: `pool=wt:...`
  (docs/POOLS.md).
- Rytm mod (rytm-mods 0021 + 0026, not in this branch): a machine slot reported with **0x28** in the voice byte (one
  of 0021's free hidden machines: `us_flag_tab` entries that are 0 now, machines 43 / 46..50; e.g. 46 -> 0x28, and
  0026's `mr_flag` the same), name `WAVETABLE` / short `WTBL`. Its SRC knobs P1..P8 TBL POS WARP AMT UNI SUB CUT RES
  (long: Table, Position, Warp, Amount, Unison, Sub Osc, Cutoff, Resonance; looks: dial, dial, list, dial, dial,
  level, bipolar, dial; defaults 1 0 0 0 0 0 64 0 since rui1043, TBL was 0), page 2 ATK DEC SUS ENV + CHD INV SPR VOI (long: Attack, Decay,
  Sustain, Env>Pos, then as MULTISAMPLE's; looks: dial, dial, dial, bipolar, then 0026's chord texts; defaults 0 64 100
  64 0 0 0 0). Value texts: WARP OFF SYNC PWM FM (4 zones), CHD..VOI as 0026's `cu_chd` etc.; TBL the host's row names
  (47 with the bank byte, `OFF` at row 000, `A005` for an empty row; rui1043: bank A like B..H, default 1). 0029's stepped list
  knobs: WARP 4 zones, the chord knobs as MULTISAMPLE's.

## 7. Files

| part | file |
|---|---|
| WAV reader | `core/wavetable/wav_read.h` |
| harmonics, mip levels, frame sizes, built-ins | `core/wavetable/tables.{h,cpp}` |
| the machine | `core/wavetable/wavetable.{h,cpp}` |
| the note machines' dispatch (MULTISAMPLE, WAVETABLE, GRANULAR) | `core/nmach.h` |
| remote UI page | `core/ui/page_wavegrain.cpp` |
| tests, demo | `core/tests/test_wavetable.cpp`, `core/tests/wg_util.h`, `core/tests/demo/render_wavegrain.cpp` (`make -C core demo-wavegrain OUT=dir`, `--bench`) |

Shared files touched (small hooks): `core/machines.h` (flags), `core/voices.{h,cpp}` (V's `wt` `gr`, report / note-off /
silence / render through `nm::`), `core/poly.cpp` (live / sequencer notes through `nm::`), `core/engine.cpp` (begin,
idle step), `core/notes.cpp` (`wt` `gr` for the bench and MIDI notes), `core/ui/remote.cpp` (machine names, the
override), `core/ui/page_seq.cpp`, `core/ui/page_gfx.cpp` + `extra.h` (the pages), `core/host/core_c.cpp`,
`core/Makefile`, `pi/engine/Makefile`, `pi/engine/bridge.{h,cpp}` (console, live input), `pi/engine/ob_usb.cpp` (the IN
stream to GRANULAR), `pi/engine/cores.{h,cpp}` (bench), `pi/engine/web.cpp` (rescan), `pi/engine/tests/pi_run.py` and
`test_pi_run.py` (bench machines), `core/tests/test_notes.cpp`.

## 8. Tests [measured]

`core/tests/test_wavetable.cpp` (in `make -C core test`): the reader (16/24/32/8-bit PCM, float, stereo, extensible,
`clm `, a data chunk past the file's end, refusals); frame size detection and thinning; harmonics (FFT and DFT) and
every level (nothing over its top harmonic, the rest kept, the guards); the WAVETABLES list (built-ins and files as rows), a trig waiting for its table, frames loaded as written, `clm ` 256, a 600-sample cycle, a broken
file refused; the morph (a POS sweep's steepest step no more than the steepest frame's own; halfway between two frames
both at half); aliasing (a C7 saw -67 dB vs -14 dB without the levels, C2..C8 under -60 dB); the warps; unison (0.86 of
the fundamental's energy spread off it, the level within 0.6..1.6x); chords and the headroom; live and sequencer notes
and `wt=voice:V`; golden renders (pluck, pad, sync, vocal: rms and zero crossings within 10 %); the CPU.
`core/tests/test_notes.cpp` plays `wt` through the MIDI note adapter and checks CC 120's silence.
