# Where the Overbridge voice inputs enter (2026-10-07) [measured, disassembly of OS 1.73]

`voice_output_mix` (0x4010ea2c), which builds each voice's digital-layer DAC
output every 32-sample block, reads, besides the voice's buffer A
(0x80006bf8 + 128v, the machine engine's output), buffer B (0x80007040 + 128v,
written by the stock engines) and the sample layer (0x800067f8 ...), an
Overbridge-shaped input block: a5 = 0x42ac6f30 + 1536 * block (block index from
0x8000fef4), 32 frames x 12 channels x 4 bytes, 48 bytes a frame.

The channel each physical voice takes is a table at 0x401bea90:
`[4, 7, 5, 8, 10, 9, 11, 6]` for voices 0..7 (BD, BT, SD, LT, CH/OH, MT/HT,
CY/CB, RS/CP). In Overwitch's host->Rytm channel list (MAIN L/R 0-1, MAIN FX L/R
2-3, BD SD RS/CP BT LT MT/HT CH/OH CY/CB 4-11) those are exactly each voice's own
input. So:

- The per-voice Overbridge input is mixed into the voice's digital layer, the
  same DAC channel a sample or FM4 plays on, before the voice's analog filter,
  overdrive and amp (the digital layer feeding the analog chain is the Rytm's
  documented design; the emulator models it so). This matches the Overbridge
  manual: the VCA must be triggered for the input to sound.
- In the input branch (taken when the table entry is >= 0, i.e. every voice) the
  per-voice gain ramp at 0x80007000 (+8v; read as the sample gain until now)
  multiplies the Overbridge input, and the sample layer gets a fixed -0.5 weight
  [measured instructions; what each term means musically is inferred]. The
  digital machines in rytm-mods 0020 zero that gain and buffer B every block, so a
  Teensy machine must not: it should leave the input's gain open.

Open: who fills the 0x42ac6f30 ring (the USB driver from EP 0x03), its length,
and the gain the stock machines set in 0x80007000 when Overbridge is connected.
For the emulator: injecting samples into this ring directly (as rytm-emu feeds
USB MIDI above the controller) is enough to hear a simulated Teensy through a
voice's analog chain, before modelling the USB controller.
