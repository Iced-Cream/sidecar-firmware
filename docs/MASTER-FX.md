# Master FX

One stereo chain after the Rytm's own FX, on the POST return (Pi; the Mac for tests; not the Sidecar Mini).

Tags: **[measured]** run on the Mac or read from a real Rytm's `stereo=measure`; **[code]**; **[inferred]** not run.

## What it can and cannot process

The Sidecar can add to the Rytm's output. It cannot take anything out of it: Overbridge OUT 0/1 and 2/3 are added into the analog
mix. MAIN on Overbridge IN is the finished analog mix plus what the Sidecar returned [measured: POST and PRE are both in it], so
running it through a chain and sending it back is a feedback loop. So there are two modes, and one switch, **SRC**, saved per kit.

| | **SRC RET** (default) | **SRC MIX** (through mode) |
|---|---|---|
| needs | nothing | the tracks to process off ROUTE TO MAIN on the Rytm (GLOBAL > AUDIO ROUTING, or the kit's TRACK ROUTING). A voice left on main stays analog and out of the chain |
| in the chain | voices on PRE and POST, the send FX returns, bus 3, the computer's OUT, `out=main`'s sum | all of that, and the stems off main: every such Rytm voice and TRK voice, after its track's filter, amp and drive, panned as on the Rytm |
| not in the chain | the Rytm's analog mix; voices on TRK | the Rytm's delay and reverb (SEND TO FX is its own page: they stay on, in MAIN, outside the chain) |
| Rytm's distortion and compressor | as ever on the analog mix; 2/3 leaves them while a chain runs | only process the Rytm's FX returns routed PRE: not the voices |
| latency | +2 ms on what goes through the chain | analog tracks about 5.9 ms later (282 frames [measured: the return path]), +2 ms with a chain on |

Bypass in SRC MIX keeps the mix: the stems stay on 0/1 and only the chain leaves.

The Rytm's own controls in through mode:

| | where it acts | what the Sidecar does |
|---|---|---|
| filter, amp, overdrive, track LEVEL, VOL, velocity, accent | before the stem | nothing: it is in the stem |
| MUTE | stops trigs, before the stem (a tail rings, as on a stock Rytm) | nothing |
| PAN | after the stem | reads it from the Rytm's report 50, p-locks and LFOs in, and pans the stem by the Rytm's law (L + R constant, linear), within 50 ms |
| ROUTE TO MAIN | after the stem | the same report: a stem still on main stays out of the sum, so nothing doubles |
| delay, reverb (SEND TO FX) | fed from the voices | nothing: they stay in MAIN. They cannot join: MAIN holds the Sidecar's own return, so summing it would feed back |
| master distortion, compressor | analog, after the main bus | they now hear only the Rytm's FX returns routed PRE. The chain's COMP is the bus compressor |

Report 50 needs a Rytm with this 0.4.0 0030-fx-in. On older mods every stem is summed, centred: mono, and all tracks must be taken off main by
hand or they double. The page says which.

Take playback (and the looper, later) comes after the chain: it is not processed twice. The recorder's source 24 POST stays the
voices' pair before the chain. MASTER (IN 0/1) holds the chain's output, the Rytm's mix around it.

## Levels

The returns are not unity [measured on a real Rytm, master FX neutral: POST -10.9 dB, PRE -2.0 dB into MAIN IN]. What was on 0/1 stays
at 0 dB, going through the chain or not. What moves onto 0/1 is raised to keep its place in the mix: 2/3 by +8.9 dB
(`kPreOnPost`), the stems by +10.9 dB (`kStemOnPost`, taking a stem to MAIN as unity [inferred]), MT/HT and CH/OH 0.85 dB less
(they read hot against what they put in MAIN [measured on a Rytm, 2026-10]). The chain's output has a soft
ceiling at full scale, linear to 70 %. Constants, from one run; `stereo=measure` checks them and always measures the plain
return: the chain stands aside while it runs.

## The chain

Four slots in a row, then a POST EQ, then LEV.

- **One FX code path.** Slots 1, 2 are chain M1 of `ins::Engine`, slots 3, 4 chain M2, with its POST EQ. Every INSERT TYPE works.
- **AMP**: its own cap, 2 instances, outside the voices' 6. One instance on (L + R) / 2 on both sides, or an L/R pair (two instances,
  each side its own). `MAX 2` past that.
- **Send FX types as inserts**: CHORUS, COMB, SPACE, RINGS, CLOUD, HALO: a second `fx::SendFx`, through `SendFx::Insert`, the same
  fades, zones and clears, the same output [measured: under 1e-4 of the send FX's own return after a settling time]. Two at a time; a third is refused and the slot keeps what
  it has. Mono in, as on the buses. A MIX knob each (127: only the return). CRUSH and RINGMOD exist as INSERT types.
- **STUTTER** is not a master type: its ring is mono; idle, it would fold the chain to mono.
- **BYPASS** crossfades over 10 ms. The chain keeps running, so tails stay and re-engaging is instant; its core time stays too.
  Bypassed and faded, the 12 channels are bit for bit what they were [measured]. Every slot off: no work on any core.

| key | what | range |
|---|---|---|
| SRC | RET / MIX | per kit |
| BYP | on / bypass | per kit |
| LEV | the chain's output | 0..127, 100 unity |
| TYPE, P1..P4 | as in docs/INSERT-FX.md; the send types' P2..P5 | |
| MIX | a send type's dry / wet | 0..127 |
| POST EQ | HPF LPF LOW HIGH | |

No p-locks and no LFO targets yet.

## Where to set it

- **Rytm**: page MASTER, host-drawn, so it takes no Rytm code space: on the LEFT / RIGHT ring and in the SIDECAR MENU; no UP key (T1..T13 are
  taken, a new key is a Rytm mod). A..D P1..P4, E SLOT, F TYPE, G MIX or MODE, H SRC, YES bypass, RIGHT the EQ view (H LEV there).
- **Touchscreen**: Settings > Send FX > Master. Every size. Tap a TYPE for the next, ON, SRC, the AMP's mode; the knobs are shown, set from the Rytm or the web.
- **Web**: the Send FX tab.
- **Console**: `mfx`, `mfx=byp:0|1`, `mfx=src:ret|mix`, `mfx=lev:V`, `mfx=S:type:T`, `mfx=S:p1..p4:V`, `mfx=S:mix:V`, `mfx=S:pair:0|1`,
  `mfx=S:off` (S 1..4), `mfx=eq:hpf|lpf|low|high:V`, `mfx=eq:off`, `mfx=reset`, `mfx=stats` (core 2's load, underruns).

## Saved per kit

In the INSERT file (docs/INSERT-FX.md): `M1 A COMP 50 60 40 20`, `AMP M2 A file`, `PEQ M2 40 0 64 70`, and only off its defaults
`MFX BYP SRC LEV MIX1 MIX2 MIX3 MIX4 PAIRS` (`PAIRS`: a bit a slot). A kit without them plays and saves byte for byte as before [measured].
An older Sidecar does not read these lines: it keeps such a file as it is and plays the kit without a chain.

## Signal path, threads

```
core 0, FillOut: Pull -> computer OUT -> AddReturn -> masterfx_pi::Process -> take playback -> taps
                                                       the stems (MIX), 0/1 + 2/3 to core 2, wet back on 0/1, the crossfade
core 0, IN completion: masterfx_pi::Stems: each stem x its L, R gains (report 50, ramped at most 10 ms) into a stereo queue
core 2, after its block and the send FX: masterfx_pi::Render -> core::master::render
core 3: settings, /NAM/ loads as for voices (no card on it, cardq)
```

The added latency is the way back: `kPrime` 96 frames, the send FX's margin against core 3's longest pass, plus a fill: 98 frames
(2.04 ms) at the default 7-frame URB [measured on the host]. The effects add none. The direct path adds none.

## Cost

See `test_master`, `bench mfx` in the boot log (a chain without AMP; add 2 x `bench amp` for a pair). Memory: +5.9 MB (the second SendFx),
+280 KB (2 AMP engines), +128 KB (DELAY lines).

## Tests

`core/tests/test_master.cpp` (TYPEs and refusals, against the send FX, noise, AMP cap and pair, per kit, old kits, console, the Rytm page,
control core, cost), `pi/engine/tests/test_masterfx_pi.cpp` (bit for bit, latency, clicks, PRE moved, a stalled core, SRC MIX,
`stereo=measure`, report 50), `pi/engine/tests/page_mfx.test.mjs`, `pi/ui/host` shots `mfx-ret`, `mfx-mix`, `mfx-through`; on the Rytm's side sidecar-mods
`tools/emu/fxin_emu.py TAG mix`.
