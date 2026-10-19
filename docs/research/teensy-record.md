# Recording on the Teensy Sidecar (design note, 2026-10-17)

Not built: the Teensy firmware is being changed elsewhere. This note carries the Pi recorder (`pi/engine/recorder.*`,
docs/pi-engine.md "Recording") over to a stock Teensy 4.1, with and without PSRAM.

## What the Pi does, and what carries over

The Pi copies the selected Overbridge IN channels (plus its own voices and the FX returns) from the IN completion
into a ring, and drains the ring from its main loop to one WAV a channel in 192 KB writes. A block that finds the
ring full is dropped, counted, and later written as silence, so the files keep time and stay aligned. On the Pi
the ring is 64 MB (about 22 s for 12 IN channels plus master at 24-bit), so a card stall never reaches it. The
worst single write measured there was 100 ms (13 files) to 137 ms (23 files) on a 55 GB card.

The portable parts carry over unchanged: `recorder.cpp` (the ring of fixed slots, the channel table, the
deinterleave, the gap-as-silence rule, the take numbering, the boot-time header fix) and `rec_wav.h` (the
512-byte header with its JUNK pad, the power-loss placeholder sizes). The Teensy needs its own Backend (SdFat) and
its own call sites: the USB host's IN callback, `loop()`, the web/console commands.

## The constraint: card stalls against RAM

SD cards pause writes now and then for their own housekeeping (erase, wear levelling). Typical pauses are 20-100 ms.
Pauses of 100-250 ms are common on consumer cards, and some cards go past that. The ring must hold the
whole pause at the data rate:

| selection | rate | 100 ms | 250 ms |
|---|---|---|---|
| master stereo, 16-bit | 192 KB/s | 19 KB | 48 KB |
| master stereo, 24-bit | 288 KB/s | 29 KB | 72 KB |
| 4 mono, 16-bit | 384 KB/s | 38 KB | 96 KB |
| 12 IN + master, 24-bit | 2.0 MB/s | 200 KB | 500 KB |
| everything (23 sources), 24-bit | 3.6 MB/s | 370 KB | 920 KB |

Free RAM on a stock 4.1 with the send FX built in: about 18 KB in RAM2, and about 70 KB of stack headroom in RAM1.
The send FX takes its share before anything is left for a ring. A ring of 32-48 KB is the realistic maximum without
PSRAM: 16 KB from RAM2 plus 16-32 KB of RAM1 taken from the stack headroom, which still leaves at least 38 KB of stack.
48 KB covers a 250 ms pause only for master stereo at 16-bit. Anything wider drops audio on an ordinary card.

## Keeping the stalls short

These apply in both cases:

- **Pre-allocate and keep the file contiguous.** Use SdFat `preAllocate()`, on exFAT, for the expected length: for
  example 30 minutes, trimmed with `truncate()` at stop. No FAT or bitmap updates happen while the take records.
  These updates cause most of the long stalls on FAT32.
- **Never wait on the card in `loop()`.** SdFat's SDIO driver has `isBusy()`: issue the next write only when the
  card is idle, so a stall costs ring space instead of loop time. The USB host, MIDI and the web UI keep running.
- **Write whole sectors**: multiples of 512 bytes, from the 512-byte header on (the same layout as the Pi). Write
  8-32 KB at a time, one file per `loop()` pass.
- **The IN callback copies only.** It copies the selected samples (7 frames x N channels) into the ring, or drops
  and counts the block. It never touches the card, never allocates and never logs.
- **Check the card at the start of a take**: measure the worst write latency over the first 2 s, and show a
  warning when the ring would not cover it.

## With PSRAM: the full multitrack path

Several users have PSRAM fitted (one or two APS6404L, 8 or 16 MB). With PSRAM, recording is the same as on the Pi:

- **Detect PSRAM at runtime** with Teensyduino's `external_psram_size` (in MB; 0 when none is fitted). The
  sample-RAM pool already reads it (docs/psram.md). Size the ring from it: for example a quarter of PSRAM, at least 2 MB
  (2 MB is 1 s of 12 IN + master at 24-bit). Leave the rest to `SampleRamPool`. One build serves both cases.
- **Put the ring in `EXTMEM`**, from the same boot-time allocation the pool uses. PSRAM bandwidth (tens of MB/s
  through the cache) is far above the 2-4 MB/s needed for writing and reading the ring. The SDIO driver's FIFO
  mode copies with the CPU, so a ring in PSRAM needs no special DMA handling. If a DMA mode is used, flush the
  cache lines before each write (`arm_dcache_flush`).
- **The page offers what the memory can carry.** The firmware reports `ring_bytes` and a `max_kbps` (the ring at
  500 ms: twice a bad stall). The Record tab enables only selections under that rate:
  - with PSRAM: every channel at 24-bit (12 IN, master, the voices, the FX returns);
  - without PSRAM: master stereo at 16-bit, or up to two mono 16-bit channels, with a note that more channels
    need PSRAM.
- **One file a channel** (as on the Pi), each pre-allocated. With contiguous files, writing to many files is no
  slower than writing to one, and the takes match the Pi's (same names, same header, same alignment).

## Recommendation

1. **One firmware build for both cases.** Detect PSRAM at runtime and size the ring from `external_psram_size`.
2. **With PSRAM (the full path):** a multi-MB ring in PSRAM, every channel at 24-bit, the same files as the Pi.
   This is the main recording target on the Teensy.
3. **Without PSRAM:** master stereo at 16-bit only (or two mono 16-bit channels) in a 48 KB ring (16 KB of RAM2
   plus 32 KB of RAM1 stack headroom), and only while the card check passes. The page greys out the other channels
   and says why.
4. **On both:** exFAT, pre-allocated files, `isBusy()`-gated writes, drops counted and written as silence, and the
   same boot-time header fix as the Pi.
5. **Measure first:** the worst write latency of the cards users have, with SdFat on the 4.1, pre-allocated
   against not. If pre-allocation keeps stalls under 50 ms on typical cards, the no-PSRAM limit can rise to
   master stereo at 24-bit: the 48 KB ring holds 166 ms of it.
