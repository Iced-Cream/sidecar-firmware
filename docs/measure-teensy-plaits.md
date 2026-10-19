# Plaits on the Teensy 4.1: CPU per voice (measured 2026-10-08)

`teensy/plaits_bench` (run: `teensy/run_sketch.sh plaits_bench`): one Plaits voice
(eurorack 08460a6, user data stubbed out), 600 MHz, -O2 (Teensyduino 1.62),
12-sample blocks at 48 kHz, 2 s per model, retriggered every 0.25 s with timbre
and morph moving. Cycle counter per block; 100 % = the whole core in real time.

```
 #  model         avg %   max %   voices (avg)
 0  VA VCF         6.24    6.54    16.0
 1  PHASE DIST     6.53    6.92    15.3
 2  6-OP FM 1      5.84   18.20    17.1
 3  6-OP FM 2      5.68   18.30    17.6
 4  6-OP FM 3      5.65   18.31    17.7
 5  WAVE TERRAIN   5.38    5.78    18.6
 6  STRING MACH    5.53    7.66    18.1
 7  CHIPTUNE       1.92    3.39    52.0
 8  VA             5.75    6.17    17.4
 9  WAVESHAPE      4.02    4.42    24.9
10  2-OP FM        5.74    6.22    17.4
11  GRANULAR       6.05    7.10    16.5
12  HARMONIC       9.66    9.91    10.3
13  WAVETABLE      5.62    7.07    17.8
14  CHORDS         5.44    6.52    18.4
15  SPEECH         1.81   34.61    55.2
16  SWARM          6.27    6.64    15.9
17  NOISE          5.20    5.34    19.2
18  PARTICLE       6.55   10.61    15.3
19  STRING         6.99   11.44    14.3
20  MODAL         10.34   10.76     9.7
21  BASS DRUM      5.40    5.49    18.5
22  SNARE          6.28    6.69    15.9
23  HI-HAT         5.30    5.76    18.9
mean 5.80 % per voice; worst model 10.34 % -> 9.7 voices of the worst
```

Reading:
- Eight voices fit: about 46 % for an average mix, 83 % with all eight on MODAL.
- Peaks come at trigs (6-op FM loads a patch: 18 %; SPEECH: 35 %, one block).
  Several voices triggered on the same step add up for a block or two, so the
  host needs a few ms of output buffering (Overbridge buffers anyway) and should
  not plan for more than about 70 % average.
- Not included: the USB host / Overbridge transfers and MIDI parsing.
- Plaits' code runs from the Teensy's ITCM (Teensyduino's default); its tables
  are read from flash through the cache.
