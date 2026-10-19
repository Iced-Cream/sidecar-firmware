# Sidecar send FX: up to two extra send effects on the expander (research, 2026-10-16)

Question: how the Analog Rytm MKII gets extra send effects (or a replaced reverb or delay) that run on the
Sidecar (Pi 4 / Teensy 4.1 as the Rytm's Overbridge USB host).

User input (2026-10-16), the model this note designs for:

- The Dirtywave M8 model: keep the Rytm's own delay and reverb; add Sidecar send effects whose algorithm is
  **chosen per project** (chorus, comb filter, Cloud Seed reverb, Rings resonator, S4-style resonator, ...),
  stored with the project. Up to **two**: SEND FX 1 and SEND FX 2.
- Ideally without taking the Overbridge AUDIO IN channels (Rytm -> host 10/11).
- UI: on the FX track, pressing **SRC again** on the DELAY page opens SEND FX 1's page, **SMPL again** on the
  REVERB page opens SEND FX 2's; on every track, pressing **AMP again** opens a second AMP page with the
  per-track SEND FX 1 / SEND FX 2 levels (room for more Sidecar options later). Fallback: the web console /
  Sidecar Manager.

Research only: no hardware touched, nothing built or flashed. Tags: **[code]** read in the OS 1.73 image or
disassembly (MAIN OS sha256 28d9ef40..., load address 0x40000400), **[measured]** observed in rytm-emu (stock
1.73) or on a stock Rytm where stated, **[inferred]** reasoned, not shown. Every address is OS 1.73. Sources:
rytm-mods branch `ob-fx` (mods/0024-ob-mix research.md + design.md, re/symbols.toml, mods/0021-ext design.md
and sound-tag-research.md), rytm-mods re/subsystems/audio-engine.md and midi-cc.md, rytm-emu docs/OVERBRIDGE.md.

## Summary

- The delay and reverb run **on the main ColdFire, in the audio interrupt** (EMAC code; no DSP, the FPGA is not
  involved) [code]. Their send input is built digitally from the receive buffer, which holds each voice as the
  ADC hears it after the voice's analog filter and amp; the returns go out through the DAC into the analog mix
  [code]. A mod could tap the send bus or replace an effect's output in RAM.
- **The return path into MAIN needs no mod and no Rytm -> host channel:** host -> Rytm channels 2/3 (MAIN FX)
  land in the PRE return pair (into the analog mix before the master distortion and compressor), and 0/1 (MAIN)
  in the POST pair, on a stock Rytm [code; measured: emulator]. The Sidecar sends nothing on 2/3 today.
- **The send input is already on the Sidecar:** Rytm -> host 2..9 are the receive buffer's voice words verbatim
  [code; measured: emulator], the signals the Rytm builds its own sends from [code for the buffer; the exact
  per-voice send weighting is not decoded, inferred].
- **Recommendation: route A.** The Sidecar computes each send FX's input = sum over voices of stem x the
  track's level, runs the project's algorithm and returns it on host -> Rytm 2/3 (or 0/1 for a POST-routed send
  FX). AUDIO IN stays on USB; the Rytm's delay and reverb stay. The audio path works on stock firmware. The
  user's UI (page 2 on DELAY, REVERB and AMP) is feasible as one rytm-mods mod in the UI task: stock already has
  two-page views that a repeated page key toggles (SAMPLE / SMPL WAVEFORM) [measured]. The values live on the
  Sidecar (per project, per kit) and reach it in a new host-link message; p-locks are not available for them
  (no spare stored word, section 3).
- M1 is testable in rytm-emu with a host script and no Rytm mod (section 5).
- Prior work, rytm-mods `ob-fx` (0024 ob-mix; built, emulator only): the opposite direction - the Rytm's delay
  + reverb return as a stereo stem on Rytm -> host 10/11 (replacing AUDIO IN), and each track's effective
  VOL/PAN as CC 7/10 (or NRPN) from a 60 Hz UI-task scan. It stopped before hardware: stem polarity and level
  against MAIN, the codec round trip, the CC flood test and the audio interrupt's second block-driver path
  (0x4011fe08) are open on its checklist. It does not compute or return any new effect. Reusable here: its
  60 Hz scan pattern (the send levels' reporter), its PAN reporting, its emulator capture tools.

## 1. Where the effects run, the send buses, the return

| fact | where (1.73) | tag |
|---|---|---|
| No DSP: the container holds meta, bootstrap, FPGA, MAIN OS, panel MCU; no DSP strings | audio-engine.md | [code] |
| Block driver order: control frame 0x4010e1a0, return mixers 0x4010ede2 (the **previous** block's FX), receive copy 0x4010e944, `fx_generate` 0x4010e650 (block N's FX from block N's receive buffer), `ob_in_write` 0x40005c6e | 0x40121306..0x40121456 | [code] |
| `rx_buffer` 0x80007440: 32 frames x 12 words; 0/1 MAIN, 2..9 voices, 10/11 AUDIO IN | symbols.toml | [code; measured: emulator] |
| Sends built from the receive buffer; delay line DRAM 0x4b090000, reverb line 0x4b000000 | fx_generate | [code] |
| `fx_delay_out` 0x80007c44, `fx_reverb_out` 0x80007b44: 32 stereo frames Q31; the reverb buffer is also its send buffer, processed in place | symbols.toml | [code] |
| PRE mixer `fx_return_mix_pre` 0x4010eb26 -> output words 12/13; POST 0x4010ebfe -> 14/15 | | [code] |
| Return gains: PRE 0x42fb4ab4 (delay) / b8 (reverb), POST 0x42fb4abc / c0; router 0x4010e8a0 (levels 0x8000fbb8 / fbca, PRE/POST flags 0x8000fbbc / fbcc) | | [code; level/flag meaning inferred] |
| Host -> Rytm OUT lands in `fx_mix_ring` 0x42ac6f30 (slot 0x8000fef4): 0/1 MAIN -> POST pair, 2/3 MAIN FX -> PRE pair, 4..11 the voices' digital layers | OVERBRIDGE.md | [code; measured: emulator; voices on hardware] |
| PRE mixer per side `-(gd*delay + 2gr*reverb + c*host)`, fixed c = 0xeb2a7c30 (about 0.16, -16 dB) for the host pair | 0024 research A1 | [code; musical meaning inferred] |
| Per-track sends: AMP words 40 DEL, 41 REV; modulated copy (p-locks, LFO) `lfo_track_array` 0x8000f7a8 + 84 t | 0024 research B2 | [measured: emulator] |

- **Tap the send bus:** yes, after `fx_generate` builds the sends and before the reverb processes
  `fx_reverb_out` in place (the delay's send buffer is not named yet). 0006's hook at 0x401213a0 and 0024's
  `ob_in_write` operands (0x4012144e, 0x4011fe0a) show such hooks fit [code].
- **Replace an FX output:** yes, write `fx_reverb_out` / `fx_delay_out` after `fx_generate` and skip the
  internal algorithm; the MIX page's return level and PRE/POST then apply unchanged.
- **No feedback loop in route A:** the voice stems never contain the host pairs (`fx_mix_ring` 0..3 are not
  voice channels) and the Rytm's sends read the voices, not the host input [code]. A return reaches Rytm ->
  host only inside MAIN 0/1, which the Sidecar must never feed into a send.

## 2. Routes

### Route A: the Sidecar computes the sends, returns on host 2/3 (recommended)

```
 voices (analog, post filter+amp) -> ADC -> rx_buffer 2..9 -> Overbridge IN 2..9 -> Sidecar
 Sidecar, per send FX k (1, 2): x_k = sum_t stem(t) * level_k(t), panned by PAN(t)
          -> FX_k (the project's algorithm) -> return level_k
          -> OUT 2/3 (PRE: through distortion + compressor)  or  OUT 0/1 (POST)  -> MAIN
```

- **Rytm side, audio:** nothing; stock plays host 2/3 and 0/1 [code; measured: emulator]. With the pass-through
  to a computer (spike of 2026-10-16) the Sidecar sums the computer's own MAIN FX / MAIN into the same pairs.
  A per-FX PRE/POST choice comes free (P1 = ROUTE): 2/3 behaves like the stock effects' PRE routing, 0/1 like POST
  (measured 2026-10-16 on the real Rytm: 0/1 is added after the analog distortion + compressor, 2/3 enters before them).
- **Rytm side, UI:** the page-2 mod of section 3. Without it: the web console / Sidecar Manager.
- **Pan and shared voices:** the stems are mono, pre-pan [code]. PAN per track from 0024's `mixcc` (CC 10,
  p-locks and LFO included) [measured: emulator], or from the same reporter as the send levels; otherwise mono
  into the stereo effect. RS/CP, MT/HT, CH/OH, CY/CB share a stem; a per-track level on a shared voice needs
  the track that plays it (owner table 0x40290dd8) - report it, or use the louder of the two levels.
- **Latency:** codec ADC + Overbridge IN + Sidecar FIFO (Pi 96 frames = 2 ms; Teensy 256 = 5.3 ms, lowerable)
  + OUT in flight + DAC: about 3-8 ms after the dry voice [inferred; the codec part is unknown, the emulator
  models 130 frames]. Inaudible for chorus, comb, resonators and reverb (it adds to pre-delay); a comb's or
  resonator's pitch does not depend on it.
- **Level ceiling:** the host pairs enter at a fixed ~0.16 [code], so a full-scale return is about -16 dB
  against the internal effects' path, and the Sidecar limits its outputs at -12 dBFS today (`core/main_out.h`).
  M1 measures whether USB TO MAIN (0/+6/+12/+18 dB) scales the pair. If the ceiling is too low, a mod gain on
  the host pair (a few instructions in the PRE mixer's path) is the fix.
- **Effort / risk:** Sidecar FX code + the page-2 mod (UI task only); lowest risk: no interrupt code.

### Route B: replace the internal reverb (or delay) with a Sidecar algorithm

- **Rytm mod (audio interrupt):** after `fx_generate`'s send build, copy the reverb send (stereo) to the host,
  skip the internal reverb, write the host's return into `fx_reverb_out`. The REV sends with their p-locks and
  LFO, the MIX page's return level and PRE/POST keep working [code for the buffers; the skip point to decode].
- **Send channel without AUDIO IN:** the IN packet's 28 private header bytes are zero in every capture
  [measured: emulator and hardware captures, OVERBRIDGE.md 5]; 28 B per 7 frames is exactly **stereo int16
  at 48 kHz** (7 x 2 x 2 B) [inferred]. Return: host 2/3 read by the mod from `fx_mix_ring` (and kept out of
  the stock PRE term), or the OUT header bytes. Fallback: 10/11 as 0024 does. Safe with the Sidecar as the only
  host; the pass-through must zero the header toward a computer [inferred].
- **Latency:** one round trip, 3-8 ms [inferred]: pre-delay for a reverb; for a delay the Sidecar shortens its
  line by the measured round trip (calibrate once with a click); delays shorter than the round trip are
  impossible.
- **Fail-safe:** with no host streaming, the mod runs the internal algorithm again (check the OUT counter each
  block) [inferred].
- **Effort / risk:** high: interrupt code in a full cave map (0024 has no byte spare in cave2), the untested
  second block-driver path, header semantics unknown to Elektron's plugin. Later, if "replace" is wanted.

### Route C: a send computed on the Rytm

- The mod sums `rx_buffer` 2..9 by per-track levels in the audio interrupt (EMAC, ~0.3 % CPU like 0024's
  `obfx_in` [code count]) and sends it in the IN header; the return as in A. It needs the same level storage
  and UI as A, and the sum adds nothing A's Sidecar-side sum lacks (same stems, same block). Only a sequencer
  p-lock on the level would justify it, and that needs a spare stored word first (section 3). Not recommended.

### Ranking

| route | quality | latency | effort | risk | AUDIO IN kept |
|---|---|---|---|---|---|
| **A** Sidecar sums the stems, return on 2/3 or 0/1 | same post-filter stems as stock's sends; PRE or POST per FX | 3-8 ms (fine for added sends) | Sidecar FX module + UI-task mod | low | yes |
| **B** replace reverb/delay | native REV/DEL sends, p-locks, return level, PRE/POST | round trip; delay compensated | interrupt mod + header channel + fail-safe | high | yes (header) / no (10/11) |
| **C** Rytm-computed send | as A | as A | interrupt + UI + storage | medium-high | yes (header) |

## 3. UI on the Rytm: the page-2 mod (user's preferred design)

**What 1.73 does today** [measured in rytm-emu, stock 1.73, cold boot on `sections/led.img`, key presses
with screenshots after each]:

- FX track: **SRC opens DELAY** (TIM X WID FDB / HPF LPF REV VOL), **SMPL opens REVERB** (PRE DEC FRQ GAI /
  HPF LPF - VOL, knob 7 drawn empty). A second SRC or SMPL press stays on the same page.
- A track: AMP opens AMPLITUDE (ATK HLD DEC OVR / DEL REV PAN VOL); a second AMP press stays there.
  SMPL then SMPL toggles **SAMPLE <-> SMPL WAVEFORM**: the stock two-page view.
- The parameter page table 0x4028843c (12 entries {name, id[8]}) in the image [code]: 7 AMPLITUDE 56..63,
  8 DELAY 72..79, 9 REVERB 82..87, **0**, 88, 10 DISTORTION 90 91 80 **0 0 0** 81 89, 11 COMPRESSOR. Every id
  72..99 is in use, so the FX track has no free parameter id; the zeros are empty knob positions.

**The repeat-press hook** [code, from symbols.toml; feasibility inferred]:

- The page view's key handler already cycles `index = (index + 1) % count` (0x4003ab86) for a view with more
  than one page; 0004 detours right there (0x4003ab94) to refresh the title. Views get their page vector and
  count in the view factory 0x400ca9b2 (the LFO view's count is the `moveq #1` at 0x400cb02a, its vector
  0x401e1b14). So DELAY, REVERB and AMPLITUDE become two-page views by giving their vectors a second entry and
  count 2: the stock toggle does the rest, as for SAMPLE / SMPL WAVEFORM. To find: the three views' count and
  vector sites in that factory (the same pattern as 0x400cb02a).
- The new pages need ids above 11: `page_info` 0x400ff574 clamps ids below 12, so a detour there answers
  SEND FX 1 / SEND FX 2 / AMP 2 descriptors (0021 replaced the same kind of lookups for EXT PAGE 2: page
  record 0x4003795a, title 0x4003793c, `sound_param_record` 0x40007028).
- Knobs: `page_knob_turn` 0x40039f3c (vtable +0x48) and `page_get_value` 0x40038462 (vtable +0x54) are the
  view's hooks. The new pages' values are not sound or kit words, so the mod answers get and turn itself from
  its own table (records copied for the look, as 0021 copies records 422..429).
- Everything runs in the UI task; nothing touches the audio interrupt.

**Where the values live:**

| value | storage | p-lock | why |
|---|---|---|---|
| SEND FX 1 / 2 level per track (AMP page 2) | the mod's per-track table (12 x 2 B), mirrored by the Sidecar per project and kit | no | the only spare stored word in a sound is index 0 [measured: 0021 sound-tag research], taken by 0021's tag and by 0004; slots 42..47 and +0x6d..+0x6f do not survive a power-off [measured]. One record maps one word, so even index 0 would give one p-lockable level, not two |
| SEND FX k type + 7 params (FX track pages) | the Sidecar, per project (`/.rytm/<project>/sendfx`), echoed to the mod for display | no | no free FX-track id and no known spare kit word [code: page table]; the kit's FX block layout is not decoded |
| Sidecar-voiced tracks (0021 user slots) | additionally, a slot knob "SEND FX 1" in the sound | **yes** | it is a sound word of the slot (1A..1C path); applied at the source on the Sidecar, no stem needed |

- Kit vs project: the Rytm's own sends are per sound (kit); the M8 model is per project. Proposal: FX types and
  parameters per project; the per-track levels per kit (a kit load asks the Sidecar for that kit's levels).
- Without a Sidecar the pages show "NO SIDECAR" and change nothing (no effect runs anyway).
- A spare-word search in the kit struct (2531 B per kit, `0x412558e3 + 2531 k`) could later give the FX
  pages kit storage and p-locks; open research.

**How the values reach the Sidecar:** a new host-link message (next free id after 0021's 15..1E, allocated in
the platform spec's table) in both directions:

- Rytm -> Sidecar: `sendfx level t k v` on each change and a full dump on hello / 0x10 / kit load, from the
  UI task, rate-limited like 0024's scan; `sendfx param k i v` for an FX-page turn.
- Sidecar -> Rytm: the stored levels and parameters on a project or kit load, the type list and parameter
  names and ranges for the pages (1A-style descriptors, as 0021 serves user-slot knobs).
- The 0x03/0x04 reports stay for mod-voice trigs; they do not cover stock tracks, so they are not used.
- Dependency: the build sends 0x10 only around EXT trigs today (measured 2026-10-15, pi-engine.md); the
  send FX need it on every project load (0021's us_loaded hook already sees every load).

**Cost** [inferred]: three view-factory patches, one `page_info` detour, the get/turn handlers, the message
codec and the table: roughly 0.8-1.2 kB of cave. The hardware builds' caves are nearly full (0021, 0024), so
`make cavescan` decides which mods it excludes.

## 4. The Sidecar side (shared core)

- `core/fx/`: header-only, allocation-free like `core/main_out.h`. `SendFx` with a type id and 7 params
  (0..127), `Process(const int32_t* stems /*8 per frame*/, const float* level, const float* pan, float* l,
  float* r, int n)`; two instances; their sum goes to OUT 2/3 or 0/1 per instance.
- Per project in `/.rytm/<project>/sendfx` (types, params), per kit the levels; loaded on 0x10 and kit load.
- CPU / RAM [inferred; bench on each board]: chorus, comb, S4-style resonator well under 1 % of a core. Rings'
  resonator ran on a 168 MHz STM32F4 at 32 kHz, so it fits the Teensy's 600 MHz M7 at 48 kHz and a Pi core
  easily. Cloud Seed (MIT): Pi first, on its own core; on a stock Teensy 4.1 (no PSRAM, the rule) a reduced
  line length or not offered. A budget guard like `voicebudget` mutes a send FX before any voice.
- Licences: Cloud Seed Core MIT, Rings (Mutable eurorack) MIT, Plasma to check (BACKLOG 2).

## 5. Recommendation and step plan

Route A with the page-2 UI: the Rytm keeps delay and reverb; SEND FX 1 and 2 run on the Sidecar, their types
per project; levels on AMP page 2; FX pages behind a second SRC / SMPL press on the FX track; the console as
fallback. Route B stays a later option for "replace the reverb".

- **M1 (rytm-emu, no Rytm mod):** a host script on `dtboot --usb-host --ob-link` (extend rytm-mods
  `tools/emu/obmix_emu.py`, which captures the IN stream): read stems 2..9, comb-filter SD's stem (ch 3) at a
  fixed level, send it on OUT 2/3, then on 0/1. Check with `dtboot --wav`: the comb tail in MAIN, the stock
  delay/reverb unchanged, the voice stems clean (no loop). Measure the round-trip lag, the pairs' gain (~0.16)
  and whether USB TO MAIN scales them.
- **M2:** `core/fx` SendFx (chorus + comb) with Mac unit tests; the Pi engine's host loop sends it (the Pi
  already runs against rytm-emu's link); console `:sendfx=k,type`, `:sendlvl=k,t,v`; per-project file.
- **M3:** web console / Sidecar Manager page for types, params, levels.
- **M4 (rytm-mods, emulator first):** the page-2 mod: find the view-factory sites, AMP page 2 with SEND FX
  1/2, the new host-link message; then the FX track pages behind SRC / SMPL.
- **M5:** 0021 user slots: a SEND FX knob in the slot, applied at the source (p-lockable).
- **M6:** Rings and S4-style resonators; Cloud Seed on the Pi.
- **Hardware** (after the firmware review, as rytm-mods docs/FLASHING.md stages it): M1 again on a stock Rytm
  (host 2/3 is stock, so no flashing): level, lag and polarity of the return in MAIN.

## M2 (built, 2026-10-16): the Sidecar side

Design changes from the user during M2 (they replace "FX type per project on the Sidecar card" in sections 3-5):

- **The TYPE is a knob stored in the Rytm's kit**, so each kit (and its patterns) has its own send-FX types and a kit
  copy/save/load carries them. Per FX: TYPE + P1..P5 in the FX words (storage spec option A + B): FX 1 TYPE = word 29
  hi, P1 = 29 lo, P2..P5 = 38 hi/lo, 39 hi/lo; FX 2 TYPE = word 0 hi, P1 = 0 lo, P2..P5 = 40, 41. TYPE and P1 work with
  stock storage alone. **For M4 to confirm** (proposed by M2): the mapping, and that a coarse-only lock leaves a lo
  half 0.
- **Ids:** 0 off; 1..63 built in, stable (1 CHORUS, 2 COMB built; 3 DELAY 4 PINGPONG 5 TAPE DLY 6 RINGS 7 S4 RESO
  8 CLOUDSEED 9 PLASMA 10 PLATE 11 HALL 12 SHIMMER 13 FLANGER 14 PHASER reserved); 64..67 **UFX1..UFX4, user-effect
  slots like the user machines**: the project's slot map on the Sidecar card (`/.rytm/fxslots/<project>.txt`) names
  the effect each slot holds with its 14-bit tag (FNV-1a based, as the sound tag); the kit stores the effect's tag, so a
  kit moved where the slot holds another effect is detected: that FX silent and MISSING (`!NAME`). Built-ins need no
  tag. **For M4:** the tag's kit storage (non-lockable: the stored kit gaps 0x832..0x841 / 0x902..0x916 with load/save
  hooks like 0025's, or a spare half word), reported as 0x20 track 13; the slot menu for UFX slots (19/1E-style).
- **Messages** (`sim/protocol.py`): 0x20 track 13 (the kit's two tags), 0x21 SENDFX NAMES (the TYPE knob's list, 16/17
  rules, ids with names), 0x22 SENDFX STATE (per FX: id, 1A state READY/MISSING, name), 0x23 SENDFX ASK.
- **TYPE changes** (kit load, p-lock) fade the old effect out over 10 ms, start the new one from a clean state, and fade
  it in over 10 ms.

Built: `core/fx` (docs/core.md "Send FX"), the Pi engine's wiring (docs/pi-engine.md "Send FX"): stems from the IN
completion, `SendFx` on render core 2 after its block, the return added on 2/3 or 0/1 in the OUT fill.

Measured on the Mac (M-series, `-O2`, 2026-10-16) [measured]: both FX on, every send up: **0.28 us a 12-frame block
(0.11 % of one core)**; `SendFx` 49.9 KB. Comb impulse peaks at 873 / 1746 / 2618 samples for 55 Hz (period 872.7);
on-tune vs a fifth off: +27.4 dB; chorus impulse first at 311 (L) / 391 (R) samples (8 ms -/+ depth); a TYPE change:
largest sample step 0.0024, silent at the switch; loud noise and 20 s of silence: within the limit, no NaN, no
subnormal. The Pi kernel builds (757816 B). Not yet on hardware.

### Hardware test plan (when the Pi is free; nothing flashed by M2)

1. Flash the Pi engine (`CIRCLE_TREE=circle-engine pi/circle/flash.sh pi/engine`), Rytm (stock is enough for 1-4)
   connected as the Overbridge device.
2. Console: `:sendfx=1:comb`, `:sendlvl=1:0:127` (BD), `:sendknob=1:2:40` (feedback), `:sendfx`. Play a BD pattern:
   the comb tail must be audible on the Rytm's MAIN out; the stock delay and reverb unchanged; `:sendfx` again: frames
   ~48000/s, underruns 0 after the start, drops 0, faults 0; note **core 2's send-FX load** and the ns per block.
3. **The 0.16 return gain:** a fixed sine into the return (`:sendfx=1:chorus`, BD with a long sine-like sound, or a
   test tone through `:sendlvl`), capture MAIN with the Pi's 10 s capture (`capch`) or an interface: compare the return's
   peak (`:sendfx` peak) with MAIN's level; repeat with USB TO MAIN at 0 / +6 / +12 / +18 dB and with
   `:sendfxout=main` (0/1, POST). Record whether 2/3 goes through the master distortion/compressor (answered 2026-10-16: it does, 0/1 does not; see P1 = ROUTE).
4. Latency: a single BD hit with a comb at high feedback; the lag between the dry hit and the first comb pulse minus
   the comb period (expected 3-8 ms).
5. With an `hwsfx1b`-like build (0025): set a track's SEND FX 1/2 (sound index 0) and the FX words via a SysEx kit
   (M0's method) or M4's pages when they exist; check `:sendfx` shows the Rytm's values (0x20 msgs counting), the TYPE
   from word 29 hi switching the effect with a fade, p-locked sends per step, and 0x22 answers in the Rytm's MIDI log.
6. Stress: ISLAND (stress budgets spec) with both FX on: Pi FIFO min, core 2 load, no underruns for 10 min.

## M6 part: RINGS, CLOUDSEED and the comb's decay (built, 2026-10-16; not yet on hardware)

From the first real-Rytm test (2026-10-16: comb + chorus 3.7 us a 12-frame block on a Pi 4 core, core 2 at 7.8 %
with a busy project, 3 return underruns in ~6 min, the 10 ms TYPE fade click-free).

**P1 = ROUTE (user, 2026-10-16), built (branch `sendfx-route`):** 0..63 **PRE**, returned on host -> Rytm 2/3 (through
the Rytm's analog distortion + compressor; the default, today's sound), 64..127 **POST**, returned on 0/1 (clean).
MEASURED 2026-10-16 on the real Rytm (OS 1.73, Overbridge mode, from the Pi): OUT 1-2 "MAIN" is added after the analog
master distortion and compressor (a Pi drone there stayed clean with DIST at max); OUT 3-4 "MAIN FX" enters before them (a
CLOUDSEED tail returned there got gritty and louder with DIST up). So PRE/POST is just the choice of return pair and needs
no firmware mod (this reverses the first mapping, 0..63 POST). A P1 change crossfades between the pairs over 8 ms. P1
shares TYPE's word (kit only). The console's `sendfxout=slot|fx|main` is a global test override (`slot`, the default, uses
P1; an explicit `fx` or `main` wins over every P1). The effects use **P2..P5 only, all p-lockable**; every knob at 0 is a usable default.

| TYPE | P2 | P3 | P4 | P5 |
|---|---|---|---|---|
| 1 CHORUS | rate | depth | delay | feedback (the level knob dropped) |
| 2 COMB | tune 55 Hz x 2^(v/24) | decay T60 0.2..8 s (64..127 negative) | damping | spread (the level knob dropped) |
| 6 RINGS | pitch C2 + v/2 | model zone (MODAL, MODAL MONO, STRINGS, CHORD) x structure | brightness | decay |
| 8 CLOUDSEED | character zone (ROOM, HALL, DARK PLATE, AMBIENCE) x size x1..x2.5 | decay: preset T60 x1..x10 | damping | modulation |
| 15 STUTTER (an insert, below) | ENGAGE (64..127 on) | LENGTH 1/64..1 bar | 0 none, 1..63 decay, 64..127 pitch down (per repeat) | GATE |

- **COMB decay (P3)** is a time, not the per-pass feedback: T60 = 0.2 s x 40^((v & 63) / 63) (0.2..8 s, log; 64..127
  the same times negative: an octave down, odd harmonics). The loop gain is `10^(-3 period / T60)` with the linear
  interpolation's loss at the sounding pitch made up, so a note rings as long at every tune [measured, Mac: T60
  within 1 % at 55 and 554 Hz, both polarities]. Kits saved before this play other knobs: the map moved.
- **6 RINGS** (`core/fx/rings_fx.h`): a port of Mutable Instruments Rings' modal resonator and sympathetic strings
  (MIT, kept in `core/fx/THIRD_PARTY_LICENSES.txt`) in its external-exciter mode, with a strum detector of our own on
  the send: an onset moves to the next voice, which takes P2's note: p-lock P2 per step and the hits ring over each
  other. 72 KB a slot.
- **8 CLOUDSEED** (`core/fx/cloudseed_fx.h`): a port of Ghost Note Audio's Cloud Seed core (MIT) with fixed 48 kHz
  buffers, no multitap, at most 6 lines x 3 diffuser stages; four characters (DARK PLATE after Cloud Seed's factory
  program), the preset T60s room 0.75 s, hall 2.4 s, plate 3.6 s, ambience 7 s [measured: P3 within 15 % of its
  ratio]. 1.25 MB a slot.
- **Structure zones are p-lockable:** a change of RINGS' P3 zone or CLOUDSEED's P2 zone fades out 10 ms, waits for the
  left state's clear (in pieces of 2 KB a frame: CLOUDSEED over ~50 12-frame blocks) and fades in from clean: no
  click, a ~10..30 ms gap. Within a zone the knob moves smoothly (structure / size glide).
- **Platform split:** both need `RYTM_SENDFX_LARGE` (default 1, 0 under Teensyduino): the Pi plays them in both slots
  (two CLOUDSEEDs included; `SendFx` 2.69 MB in .bss, the kernel ends at 4.8 MB of its 8 MB); the Teensy, which places
  no `SendFx`, lists OFF CHORUS COMB and reports 6 and 8 MISSING.
- **CPU** [measured: Mac M-series `-O2`, ns a 12-frame block; Pi 4 estimated x13.2, M2's Mac/Pi ratio]: chorus + comb
  300 (Pi ~3.9 us, 1.6 %); RINGS 830..1040 (~11..14 us, 4..6 %); CLOUDSEED 930..1280 (~12..17 us, 5..7 %); two
  CLOUDSEED AMBIENCEs 2380 (~31 us, 12.6 %); AMBIENCE + RINGS STRINGS 2260 (~30 us, 12 %). On top of core 2's voices
  (7.8 % busy): watch `:sendfx`'s ns a block and the underruns.

Hardware checks for this step: (1) `:sendfx=1:comb`, `:sendknob=1:2:0` then `:sendknob=1:2:80` with the same P3: the
ring is as long at both pitches; (2) `:sendfx=1:rings`, a BD/SD pattern, `:sendknob=1:2:42` (A3): the hits ring at A3,
a P2 p-lock per step gives overlapping notes, P3 32 (MODAL MONO) is wide; (3) `:sendfx=1:cloudseed` and
`:sendfx=2:cloudseed` with `:sendknob=2:2:96` (AMBIENCE): core 2's load and ns a block in `:sendfx`, underruns over
10 min against the 3 in ~6 min of the comb + chorus test; (4) switch TYPE, RINGS' P3 zone and CLOUDSEED's P2 zone
while playing (also as p-locks): no click, a short gap; (5) P1 0 vs 127: no change.

## STUTTER: beat repeat as a master insert (built, 2026-10-16, branch `sendfx-stutter`; not yet on hardware)

TYPE **15 STUTTER** (core/fx/stutter_fx.h, in core::fx::SendFx) is not a send: the FX slot that has it ignores the
track send levels; its input is the plain sum of the 8 Overbridge voice stems (IN 3..10; they are mono, so the sum is
mono, on both sides) and its output goes to the pair its slot's P1 names (PRE by default, host -> Rytm 2/3: through the analog
distortion + compressor; POST, 0/1), without the send return's soft limit (only the int32 saturation). Disengaged it is that sum at unity: bit-exact in the core (the
float sum); on the Pi the int32 stems go through float, exact for 24-bit samples, at most a 24-bit LSB's rounding of the
sum otherwise. Engaged it repeats a slice of the last ~2 bars.

**Knobs (all p-lockable; all at 0 = off, the dry mix):**

| knob | 0 | range |
|---|---|---|
| P2 ENGAGE | off | 0..63 off, 64..127 on. A lock on a step fires: the repeat plays the slice that ENDED at the latest LENGTH grid point (lock step 5 at 1/16: step 4 again), at the clock's phase, so it starts at once and in time; a grid point under 10 ms ahead is waited for (an early report). The first repeat always plays to its end (a single locked step = one repeat); after that it returns to dry (5 ms) as soon as P2 is below 64. Lock consecutive steps (or set the base above 63) for a roll. |
| P3 LENGTH | 1/64 | 7 zones of ~18: 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar (4/4 from Start / SPP). Read again at every slice boundary. |
| P4 DECAY / PITCH | none | 1..63 volume decay per repeat, (v/63) x 12 dB; 64..127 pitch down per repeat, 0.25..12 semitones (repeat n at 2^(-n st/12) speed, floor 1/16: a tape stop in steps; a slower repeat plays less of the slice, its length stays on the grid). Repeat 0 is the slice as captured. Not "centre = none": a Sidecar knob's 0 is its default. |
| P5 GATE | whole repeat | 1..127: each repeat sounds for 100 %..10 % of its length (1 - 0.9 v/127), then a 2 ms close to silence. No wet/dry knob: engaged, the repeat replaces the mix. |

**Capture and fades:** a ring of 2^19 mono float frames = **2 MiB** (10.9 s at 48 kHz: 2 bars at 88 BPM, 1 bar at 44;
a slice longer than half the ring halves until it fits). SendFx grows from 2.69 MB to 4.78 MB. Engage and release are
5 ms linear crossfades dry <-> repeat, every slice boundary a 2 ms crossfade (the old repeat reads on past its end),
the gate's close 2 ms: the largest sample step on a 437 Hz sine at 0.5 FS is 0.0297 against the sine's own 0.0286.

**Clock:** the Pi's MIDI in (MidiIn's new realtime callback: F8, FA, FB, FC, F2 SPP) -> core::fx::clock_in -> one
packed word (song position, tick count mod 128, running). The audio side stamps each new tick with its frame count
and takes the tempo from the last 24 ticks; the grid is the song position while running. No tick for 250 ms, or
stopped: free run at the last tempo (120 BPM if none), the grid anchored at the engage. `:stutterlag=N` (frames,
default 0) moves the grid later, for the stems' latency behind the MIDI (to measure on hardware). CC 120 (a double
STOP): the repeat drops to dry in 5 ms (the mix itself stays) and stays dry until P2 is seen below 64.

**One at a time:** FX 1 wins; a second STUTTER (FX 2) is silent and reports MISSING (the Rytm shows !STUTTER).
**Teensy:** not built (RYTM_SENDFX_LARGE: the 2 MiB ring is twice the Teensy's RAM, and it places no SendFx):
STUTTER is MISSING there, as RINGS and CLOUDSEED.

**CPU** (core/tests/test_stutter.cpp, 12-frame blocks, Mac x13.2): dry 0.22 us (~2.9 us a Pi 4 core), engaged
0.19 us (~2.5 us), with pitch 0.19 us, decay + gate 0.15 us: about 1 % of a 250 us block.

### How to use it on the Rytm

1. AUDIO ROUTING (OS 1.72 manual 14.6): **ROUTE TO MAIN off for all 12 tracks.** They still go to their Overbridge
   outputs, so the Sidecar gets every stem and carries the whole mix back on MAIN. (Leave the Sidecar on first: with
   the tracks off main and no Sidecar, the Rytm is silent.)
2. SEND FX 1 TYPE **STUTTER** (FX 2 if FX 1 is busy; never both). Set P2 to 0 first: a kit never edited by the
   send-FX mod carries 127 in P2..P5 (hardware test, above), which would mean "engaged" all the time.
3. P-lock **P2 to 127** on the steps where it should fire (consecutive steps for a longer roll).
4. **P3** the slice length (lockable per step too: 1/32 rolls on one step, 1/4 on another), **P4** decay (left half)
   or pitch-down per repeat (right half), **P5** a shorter gate per repeat.
5. Latency: while the Sidecar carries the mix, the **whole mix is ~4-5 ms late** (the Overbridge round trip), dry
   or repeating. With **P1 0..63 (PRE, the default) the mix returns on 2/3 and keeps the Rytm's analog master
   distortion and compressor**; with P1 64..127 (POST) it returns on 0/1, after them (measured 2026-10-16), clean.
6. The Rytm must send its clock to the Pi (MIDI CONFIG: CLOCK SEND, TRANSPORT SEND on the Sidecar's port); without it
   STUTTER runs free at 120 BPM from the engage.

### Later options

- (Done, 2026-10-16: no EXT track and no PRE-FX mod are needed; OUT 3-4 already enters before the analog distortion +
  compressor, so STUTTER on P1 PRE gets them.)
- Stereo: the stems carry no pan; a stereo insert needs the Rytm's MAIN 1-2 (IN 1-2) with the tracks still on main,
  which then cannot be muted from the dry path.

## Open

- The per-voice send weighting in the stock send build and the delay's send buffer (before route B/C).
- Whether USB TO MAIN applies to host 2/3 and 0/1 (M1).
- The view-factory count/vector sites for DELAY, REVERB and AMPLITUDE (M4).
- A spare stored word in the kit struct for p-lockable FX-page values. **Answered** in
  `2026-10-16-sidecar-send-fx-storage.md`: FX words 0 and 29 (stock-stored, p-lockable), FX words 38..41 (with
  detours), and sound index 0 for the per-track levels.
- Header bytes: whether Elektron's own plugin reads them (only if B/C use them with the pass-through).

## M4 status (2026-10-16, rytm-mods branch `sendfx-ui`, mod 0029-sendfx-ui; emulator only)

- **Built** (build `sfxui` = hwus1nlel2 + 0025 + 0029, 0022/0025/0029 in elekloader's area; verify PASS): a second
  AMP press opens AMPLITUDE 2 (SFX1 / SFX2 = sound index 0 hi / lo); on the FX track a second SRC (DELAY) press opens
  SEND FX 1 and a second SMPL (REVERB) press SEND FX 2: TYPE P1 P2 P3 P4 P5. The M2 mapping is used as proposed
  (FX 1: 29 hi/lo, 38, 39; FX 2: 0 hi/lo, 40, 41). The view-factory sites (open point above): AMPLITUDE count
  0x400cafd4 / vector 0x400cafda, DELAY 0x400cb07c / 0x400cb082 (key 0x31), REVERB 0x400cb0d8 / 0x400cb0de (key
  0x32) [code; measured].
- **Choice:** TYPE and P1 are kit-only (no p-lock): TYPE shares its word with P1, so a P1 lock would freeze TYPE.
  P2..P5 and SFX1/2 lock through the stock path (type 41 + fine; 0025's 42..49). TYPE shows the Sidecar's names
  (21), `!NAME` on 22 MISSING, and 0029 sends 23 until answered (0022 router param `sfx=1`).
- **Tests** (rytm-mods `tools/emu/sfu_emu.py sfxui all`, strict): pages appear and return; turns reach the live kit,
  the project copy and the trig array; SEND and FX-page p-locks arrive in 0025's 0x20 per step; the incremental
  save's FX >= 38 branch (next step 2 of the storage spec) ran and kept the locks; power-off, SAVE / wipe / LOAD
  PROJECT; the mod's project on stock 1.73 harmless; two patterns on two kits switch TYPEs in the reports. Strict
  cold boot under DTEMU_CACHE=log: 0 stale fetches. Cost: no ISR code; UI <= ~955 instructions a tick, <= 142 a call.
- **Open:** the user-FX tags (0x20 track 13) and the UFX slot menu - no live home for a stored kit tag found yet
  (rytm-mods mods/0029-sendfx-ui/design.md section 7); labels per effect from the Sidecar; hardware.

## Hardware test (2026-10-16, Pi 4 + real Rytm MKII)

- Stage 1, stock Rytm 1.73, Pi console only: comb on BD audible on MAIN via the FX return (2/3), retuned by P1; comb -> chorus switch without a click. Both FX on, all 12 sends at 100: core 2 1.44 %, 3.6 us a 12-frame block, 0 underruns/drops/faults. Finding: the comb's ring shortens with pitch (fixed feedback per pass), see the BACKLOG row "P2 as decay time".
- Stage 2, sfxui build (rytm-mods sendfx-ui 105dc35, sha256 af35ec46..., the elekloader area's first hardware run): boots, EXT machines and user slots as before; 0x20 reports arrive at the Pi (732 in the first seconds); AMPLITUDE 2 and SEND FX 1/2 pages work, effects audible from the Rytm's own knobs, SAVE KIT + reload keeps every value. Finding: existing kits carry 127 in FX words 38..41, so P2..P5 start at max; consider a 0 default (or treat 127 as unset) when a kit has never been edited by 0029.
- Same session: p-locks on the send pages work per step and come back on a pattern reload; no click on kit reload or save. Busy project (many Pi voices on core 2): send FX 3.7 us a block, 3 return underruns in ~6 min, inaudible; watch in the ISLAND run.
- P-locks on the SEND FX P2..P5 parameters also survive save and reload.
