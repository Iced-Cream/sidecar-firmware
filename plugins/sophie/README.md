# SOPHIE: metallic FM percussion (kicks, clangs, zaps, bells, shards)

A Pi Sidecar user machine (RMPA package `sophie.a64.rmp`, id `sophie.perc`), shipped on its own, not in the release
build. The DSP is [Sophie](https://github.com/mestela/schwung-sophie)'s, v0.5.1 (Matt Estela, MIT: `LICENSE.sophie`;
the source and commit in `NOTICE`): a carrier and three modulators at non-integer ratios that COLR moves, operator
feedback, a fast bipolar pitch sweep, an amplitude envelope set as the audible length, four operator structures
and Sophie's kick (a tuned sine body with a quiet folded / FM texture), then the digital stage in Sophie's order:
decimation and bit reduction, a soft-clip drive, the Ring short feedback delay, and last a multimode filter.
`sophie.c` lists the changes for the Sidecar.

**One track, not a 16-pad kit.** On the Move, Sophie is a kit of 16 pads; here each Rytm (or A4) track is one
SOPHIE, and Sophie's pads are knob settings (the table below). Her pad 1 kick topology is the `KICK` entries of MODL.

**Hits:** each hit takes a snapshot of the knobs, as Sophie's voices do: a knob moved (or an LFO) while a hit rings
shapes the next hit, and a p-lock shapes its own step. A hit rings on under the next one (two at once a track; a
third fades the oldest out over 2 ms). The hits are one-shots: the note-off does nothing, so the box's AMP envelope
(HOLD, DEC) cuts them if you want. A poly track plays its notes on instances (manifest `max_voices` 8).

**Pitch:** note 60 (the Rytm's default) plays at Sophie's note 36, her pad 1. TUN moves it in half semitones (two
steps a semitone, +-32), so a Sophie pad n is TUN 64 + 2 x (its Tune + n - 1).

State 13,776 B a voice, image 12 KB. Declared CPU 220 permille (two of the heaviest hits ringing at once; the host's
conversion makes it 0.35 % of a Pi 4 core a voice); measured on an arm64 Mac, the package's own code: 0.40 us a
12-frame call for the kick, 0.87 us for two Stack crashes with CRSH, RES and the ring.

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | TUN | 54 | pitch, half semitones, 64 = note as played (54: Sophie's kick, -5) |
| SRC | P2 | MODL | KICK | FUSE STACK SPLIT SHARD (Sophie's models), KICK, KICK ST, KICK SP, KICK SH (her kick, alone or with the model's overtone) |
| SRC | P3 | COLR | 30 | Color: the modulators' inharmonic ratios |
| SRC | P4 | METL | 10 | Metal: FM depth and density |
| SRC | P5 | FDBK | 8 | Feedback into the first operator |
| SRC | P6 | SWP | 99 | Sweep: the pitch envelope, 64 = none, left from below, right from above |
| SRC | P7 | DEC | 58 | Decay: the audible length, 0.03 .. 4 s (58: 0.28 s) |
| SRC | P8 | CRSH | 0 | Crush: sample-rate and bit-depth reduction |
| SMPL | P9 | DRV | 74 | Drive: the soft clip, hotter above 64 |
| SMPL | P10 | CUT | 127 | filter cutoff, 40 Hz .. 16 kHz; with FLT DJ the sweep position |
| SMPL | P11 | RES | 0 | filter resonance |
| SMPL | P12 | FLT | LPF | filter type: LPF HPF NOTCH DJ BPF |
| SMPL | P13 | RTIM | 77 | Ring time, 0.5 .. 30 ms (77: 6 ms) |
| SMPL | P14 | RFB | 0 | Ring feedback |
| SMPL | P15 | RMIX | 0 | Ring mix (0: no ring) |
| SMPL | P16 | RTON | 89 | Ring tone: the delay's lowpass |

Sophie's factory kit (her `set_factory_kit`) as knob values; every pad has CUT 127, RES 0, FLT LPF, RTIM 77, RFB 0,
RMIX 0, RTON 89, at note 60:

| Pad | TUN | MODL | COLR | METL | FDBK | SWP | DEC | CRSH | DRV |
|---|---|---|---|---|---|---|---|---|---|
| Kick (the defaults) | 54 | 72 KICK | 30 | 10 | 8 | 99 | 58 | 0 | 74 |
| Rim | 66 | 56 SHARD | 97 | 91 | 43 | 74 | 29 | 61 | 74 |
| Snare | 68 | 56 SHARD | 66 | 97 | 23 | 82 | 54 | 38 | 66 |
| Clap | 70 | 40 SPLIT | 107 | 112 | 53 | 59 | 48 | 71 | 86 |
| Snare 2 | 72 | 24 STACK | 80 | 104 | 36 | 79 | 47 | 48 | 74 |
| Low Tom | 74 | 8 FUSE | 30 | 56 | 20 | 101 | 68 | 10 | 61 |
| Closed HH | 76 | 56 SHARD | 119 | 124 | 74 | 64 | 16 | 66 | 79 |
| Floor Tom | 78 | 8 FUSE | 39 | 61 | 25 | 98 | 71 | 13 | 64 |
| Pedal HH | 80 | 56 SHARD | 112 | 119 | 64 | 65 | 31 | 58 | 74 |
| Mid Tom | 82 | 8 FUSE | 48 | 66 | 28 | 94 | 64 | 15 | 66 |
| Open HH | 84 | 56 SHARD | 116 | 122 | 79 | 61 | 72 | 53 | 76 |
| Low-Mid Tom | 86 | 8 FUSE | 57 | 71 | 32 | 92 | 61 | 18 | 69 |
| High-Mid Tom | 88 | 8 FUSE | 65 | 76 | 36 | 89 | 57 | 20 | 71 |
| Crash | 90 | 24 STACK | 99 | 117 | 91 | 56 | 104 | 36 | 69 |
| High Tom | 92 | 8 FUSE | 74 | 81 | 38 | 87 | 53 | 23 | 74 |
| Ride | 94 | 40 SPLIT | 88 | 109 | 84 | 60 | 95 | 28 | 66 |

Not in this port: Sophie 0.6.0's PIPE and BOOM models, Metal Decay, Feedback Mode and Tail Mode (see `NOTICE`), and
her LEVEL knob (left at 100 %: use the box's own level).

## Build, check, install

```
make a64         # sophie.a64.rmp (aarch64-none-elf-gcc, else the rytm-circle docker image)
make package     # out/: sophie.a64.rmp, LICENSE.txt (ours and Sophie's), NOTICE.txt, SHA256SUMS, BUILD-INFO-plugins.txt
make check       # tests/sophie_check.cpp: the package run on this arm64 Mac as the Pi runs it
make demo DEMO=/path/sophie-demo.wav    # tests/demo.py: the 16 pads, then a two-bar pattern
../../.venv/bin/python ../sc_render.py sophie    # the starter render check
```

Install: upload `out/sophie.a64.rmp` on the Pi's web page (http://sidecar.local, User machines), or copy it into
`/Plugins` on the card; then pick SOPHIE for a user slot (U1..U5) from the Rytm's slot menu, the USER SLOTS page or
the web page.
