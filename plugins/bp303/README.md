# BP303: a 303-style bass line voice

A Pi Sidecar user machine (RMPA package `bp303.a64.rmp`, id `bp303.bass`), shipped on its own, not in the release
build. The voice is **BLACKPYRAMIDS_303**'s, the TB-303 voice that
[OT Kyoti FW](https://github.com/Zac-Kyoti/octatrack-kyoti-fw#blackpyramids_303) (Zac Kyoti and contributors, MIT:
`LICENSE.kyoti`) runs as a track machine on the Octatrack MKI. That voice began as a port of
[Open303](https://github.com/RobinSchmidt/Open303) (Robin Schmidt, 2009, MIT: `LICENSE.open303`) and was recalibrated
on the TB-303's circuit and against recordings of a real one, every choice made by its author's ear. BP303 is that
project's float / C++ reference of the voice rewritten in float C for the Sidecar (`NOTICE` has the files and the
commit); its DSP56300 assembly and the Octatrack parts are not ported.

Credits: the filter, oscillator and envelope algorithms are Open303's (Robin Schmidt); the engine layout, most of the
values that shape the sound, the accent sweep, the output stage and every knob law are BLACKPYRAMIDS_303's (Zac Kyoti
and the OT Kyoti FW contributors; their document `reference/BLACKPYRAMIDS_303.md` credits the circuit sources and the
reference recordings they measured against). The Sidecar port: trigcondition and contributors.

```
saw (polyBLEP) -+- blend (WAVE) - input coupling 0.7 Hz - ladder (4x, 192 kHz) --- half-band 4x>2x (15 taps)
square (tanh)  -+      resonance feedback - high-pass 55 Hz -+                - half-band 2x>1x (43 taps)
  - allpass 14 Hz - output coupling 110 Hz - notch - VCA (amp envelope, de-clicker) - 16 kHz rounding
  - soft drive (VOL 65..127)
cutoff = base (CUT, ENV's offset) + ENV x filter envelope + accent RC + the accent sweep capacitor
```

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | CUT | 44 | cutoff: 0..100 is the real 303's range (a base cutoff of about 206 Hz .. 1.6 kHz, evenly in octaves), 101..127 an octave more |
| SRC | P2 | RES | 64 | resonance, the real 303's range, every step the same amount more; with CUT above 100 the top (above 100) is eased |
| SRC | P3 | ENV | 32 | env mod, the real 303's range |
| SRC | P4 | DEC | 72 | filter envelope decay, 50 ms .. 2 s (72 is about 400 ms); read when a note starts; an accented note ignores it (tau 68 ms) |
| SRC | P5 | ACC | 100 | how strong an accent is (0: no accents); the accent sweep comes in above 60 % of the pot |
| SRC | P6 | WAVE | 0 | saw (0) .. square (127), a blend at equal loudness |
| SRC | P7 | SLID | 73 | slide time (73 is the 303's, tau 22.7 ms; 0 jumps) |
| SRC | P8 | VOL | 99 | 0 off; 1..64 clean, -30 dB .. full; 65..127 into a soft drive, up to about +7 dB |
| SMPL | P9 | TUNE | 64 | A4 = 400 .. 480 Hz (64 = 440 Hz); read when a note starts |

**Playing it on the Sidecar**

- **Accent:** a note with velocity above 100 (of 127) is accented, by the ACC knob's amount. Velocity does nothing
  else (a 303 has none): lock VEL to 127 on the steps to accent and leave the rest at 100 or below.
- **Slide:** a trig while the gate is still open (the note before lasts into it: a longer note length on that step)
  glides to the new note without retriggering the envelopes, in the SLID time. One note_off then ends the note. (A
  note held until it has died away, some 20 s, is silent: a trig then starts a new note.)
- **Gate:** the track's own note length. The original's GATE knob (a share of the step) is not here: the machine is
  not told the step length or what the next step holds.
- The oscillator runs free, as the 303's, and the filter runs on behind the closed VCA for 2 s after a note, so the
  next note of a line meets a filter in motion; after that the voice sleeps and takes no CPU.
- Clean VOL (1..64) can clip on bright accents at low RES, as the original's; the driven half never does.

**What differs from the original** (`bp303.c`'s header): floating point instead of 24-bit fixed point; every time
constant made for 48 kHz (the filter at 192 kHz; the original runs 44.1 / 176.4 kHz); a 16-sample control step with the
eases scaled to it; knob values with their fraction; notes, accents and slides as above; no GATE knob; no word
saturation inside the voice (a guard restarts the filter's memories far above any level it reaches); the Octatrack
parts (storage, AMP page hooks, trig-key modes, ties from trigless trigs) do not exist here.

**How close it is.** Built for 44.1 kHz and fed the same score, the Mac build was compared with that project's
reference renderer (`tools/ot303/body/bp_cf.cpp`, the voice exactly as the Octatrack plays it): ten two-bar lines
(defaults, RES 100 and 127, square, a blend, clean VOL, CUT at both ends, ACC 127, a CUT / RES sweep) agree in level
within 0.1 dB and in third-octave bands over 46 ms windows within 0.05 dB on average; a single note's waveform
agrees to about -28 dB (the rest is the original's 24-bit pitch word, which drifts in phase against an exact one).
At 48 kHz against 44.1 kHz the same lines differ by 0.3 dB at most in any third-octave band. The comparison needs
that project's sources, so it is not a test in this repository.

State 800 B a voice, image 37 KB, declared CPU 300 permille (0.47 % of a Pi 4 core by the host's conversion; `make
check` prints the Mac estimate, 140 .. 410 by the reference machine, about 1 us a 12-frame call on an M-series Mac).
Not measured on a Pi yet. It is a Pi machine: the Teensy package (a plain `make`) has not been built or run.

## Build, check, install

```
make a64         # bp303.a64.rmp (aarch64-none-elf-gcc, else the rytm-circle docker image)
make package     # out/: bp303.a64.rmp, LICENSE.txt (ours, BLACKPYRAMIDS_303's and Open303's), NOTICE.txt, SHA256SUMS, BUILD-INFO-plugins.txt
make check       # tests/bp303_state.c (the voice's state) and tests/bp303_check.cpp (the package run on this arm64 Mac as the Pi runs it)
make demo DEMO=/path/dir    # tests/demo.py: five acid lines as WAV files
../../.venv/bin/python ../sc_render.py bp303    # the starter render check (pitch, sustain, release)
```

Install: upload `out/bp303.a64.rmp` on the Pi's web page (http://sidecar.local, Machines / User machines, or Files into
`/Plugins`), or copy it into `/Plugins` on the card; then pick BP303 for a user slot (U1..U5) from the Rytm's slot
menu or the web page.

"BP303" names the machine after BLACKPYRAMIDS_303. It is not affiliated with or endorsed by any synthesizer maker.
