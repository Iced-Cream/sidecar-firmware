# Looper: per-track or master grid? (2026-10-08)

## What the Rytm gives a USB host

- **Rytm -> host, per voice: dry.** The analog voices are digitized after their own
  chain (filter, drive, amp envelope); those signals feed the digital delay and
  reverb, whose returns go back into the analog mixer, and MAIN is that analog mix
  digitized again (rytm-emu docs/RYTM-PORT.md, codec link and USB audio
  [measured]). The 12 channels to the host are MAIN L/R, the 8 voices, AUDIO IN
  L/R, so a per-voice channel has no delay, reverb or master compressor/drive.
  Manual 14.6: tracks keep "their individual output" whatever the FX routing.
- **Rytm -> host, MAIN L/R: everything**, FX and master processing included.
- **Host -> Rytm, stereo into the main path** [OS 1.73 disassembly, inferred;
  not measured]: besides the per-voice inputs (`voice_output_mix`, before the
  filter and gated by the amp envelope), two routines read the input ring every
  block: `0x4010eb26` adds ring channels 2/3 to the delay/reverb return buffers
  into DAC words 12/13 (the return before the master analog distortion and
  compressor), and `0x4010ec04` reads channels 0/1 into DAC words 14/15 (after
  them). Their gain depends on the USB IN routing setting (0x413189bf, 0x413189c7,
  0x8000ee74), which the manual (14.6) describes as PRE-FX ("affected by the
  effects") and POST-FX ("not affected"). rytm-emu leaves these silent: it does
  not model the USB controller, so no USB mode is ever active.

## Per-track looper: not a good fit

A loop of one track would be recorded dry: its delay and reverb tails and the
master drive/compressor are missing. Played back into the main path, it would
sound different from the live track; played into the voice input instead, it
would go through the filter a second time and only sound while the track's amp
envelope is open. Track mutes and send changes would also not affect it the way
they affect the live track.

## Master grid (the plan)

Record MAIN L/R (the finished sound) and play it back on the POST path, so
nothing is processed twice:

- TRK + REC enters looper mode. The 12 pads are clip slots; UP/DOWN pages through
  more slots (like chromatic mode pages notes).
- An empty pad: records the master from the next bar for N bars (4 by default),
  then plays it from the SD card, looping.
- A playing pad: stops it at the end of the bar; another pad takes over
  (one clip playing at a time, Octatrack-style), so patterns, mutes and kits can
  change underneath and the loop is dropped on a bar line.
- Clips stream from SD (docs/PLAN.md, step 4); timing from the Rytm's clock.

Rytm side (a mod): catch TRK + REC and the pads in looper mode, keep the pads'
normal action from running, light them by clip state, and report pad presses and
the bar position to the Teensy (the EXT report channel). Teensy side: record
MAIN from the Overbridge stream to SD, play clips into MAIN L/R.

## In the emulator (2026-10-08)

rytm-emu now runs Overbridge for real (`--usb-host`, USB CONFIG = OVERBRIDGE;
rytm-emu docs/RYTM-PORT.md): host channel 0 (MAIN L) reaches the post-FX return,
channel 2 (MAIN FX L) the pre-FX return [measured, DAC words 15 and 12]. The
Rytm's 12 output channels reach the host over the link (`R` messages).

`sim/obhost.py` has the looper core (`Looper`): a rolling recording of MAIN L/R
and a loop mixed into MAIN L/R. `--loop-test AT,SECS,UNTIL` exercises it: with
the pattern playing, at AT s it loops the last SECS s of MAIN; the Rytm's STOP
then leaves the loop sounding until UNTIL s [measured: sound continued after STOP,
silence after UNTIL].

Open: the loop came back about 18 dB down (the emulator's MAIN return level, or a
firmware gain not traced yet: calibrate against a real Rytm); bar sync (clock);
the Rytm-side looper mode (TRK + REC, pads as clips, pages); SD streaming on the
Teensy.

## Looper mode (2026-10-08)

rytm-mods 0022-looper: TRK + REC enters looper mode; pads are clip slots, UP/DOWN
pages; reports `05 on page` and `06 slot down` (SysEx). `sim/obhost.py` acts on
them: empty slot -> record 4 bars from the next bar, then loop; slot with a clip
-> play from the next bar; playing slot -> stop at the next bar; with the
transport stopped, at once. Bars from MIDI clock (CLOCK SEND, TRANSPORT SEND on).
Checked end to end in rytm-emu (`--usb-host`, OVERBRIDGE): 7.56 s recorded at 127
BPM (4 bars), loop continues after STOP, the pad stops it.

## Resampling and level (0.5.0 master looper)

The ring records the master as heard: the loop plus the live Rytm. A grab while a loop plays (a resample) therefore holds both, at
about twice the level of the first grab when the Rytm is not muted. After one or two resamples with the Rytm live the master can clip:
turn LEV down for the resample (LEV 100 is the level as grabbed), or use auto-mute / LIVE VIA so the live Rytm is down under the loop
(then a resample is the loop alone, as the Octatrack's). The page and the console say nothing about this: it is the design.

The Pi plays a loop out on POST, which reaches the Rytm's main about 10.9 dB down, so the looper makes that up (x3.508). A quiet
loop comes back at the level it was grabbed (and a resample of a loop alone keeps its level). POST itself tops out about 10.9 dB
under the Rytm's full scale, so a loud loop cannot come back as loud as it was heard: the make-up is capped per loop.

## OUT: POST or PRE, and the level cap

LOOP page, encoder G (second row, third cell: OUT, POST / PRE; `loop=out:post|pre`; saved as `out=post|pre` in loop.txt).

- **POST** (default): the loop goes onto the Rytm's POST return (the Pi's outputs 0/1). The Rytm's USB IN levels are fixed in
  Overbridge mode, so this return always reaches the main 10.9 dB down, after the Rytm's own distortion and compressor.
- **PRE**: the loop goes onto the PRE return (outputs 2/3), added to whatever is already there (voices on OUT PRE, the send FX's
  PRE return, the computer's OUT, which the master FX take first). PRE reaches the main only 2.0 dB down (make-up x1.259 instead of x3.508), but it
  enters before the Rytm's analog distortion and compressor, so a loud loop is shaped by them a second time.

The cap, on both: when a loop is grabbed its largest sample (L or R) is measured while it is copied to the store. The loop is
played with the make-up, or less if that would put the loop alone above 75 % of full scale (the soft knee's start): gain =
min(make-up, 0.75 / peak). So one loop never reaches the knee: clean, never clipped. A loop with a peak under -13.4 dBFS (POST)
or -4.5 dBFS (PRE) keeps the whole make-up, a louder one is exactly as much quieter as it is louder than that (a peak at
-6 dBFS: 7.4 dB quieter on POST; at 0 dBFS: 13.4 dB). PRE is the way to get a loud loop back at full level, at the price of the
Rytm's compressor and distortion again. The cap follows the loop (each loop its own peak: a second grab never changes the one
that plays), LEV and BAL are inside it (the cap is on the final gain: LEV 127 never takes a loop over the ceiling, LEV below 100 makes it quieter), and the knee stays the backstop
for several loops, or the Sidecar's own output, summed. A resample's loop is a new grab with its own peak.

## Calibrating the POST return (`loop=cal`)

`loop=cal` sends three clicks onto POST 0/1 and listens for them in the Rytm's MAIN. Besides the POST latency it now measures
the return's gain: the click's peak out / the peak that comes back (the result line shows it, `POST LAT 12 X3.51 10.9DB`). A gain
in 1.5 .. 6.0 is saved as `postgain` (thousandths, in loop.txt) and replaces the x3.508 on POST; anything else is ignored. It
is measured on POST only: PRE keeps its x1.259. Play nothing loud on the Rytm while it runs (a live signal at the click raises
the peak back and lowers the gain it finds).

Switching OUT while a loop plays crossfades between the pairs over 10 ms (no click).
