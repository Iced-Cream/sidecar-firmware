# The Sidecar Mini's recorder (Teensy 4.1)

The Rytm's Overbridge IN stream to the Teensy's SD card, in the same layout and format as the Sidecar (Pi)
recorder (docs/pi-engine.md "Recording"): `/Recordings/<prefix>_NNN/NN_NAME.wav`, 48 kHz PCM, 16 or 24 bit, a
512-byte header (RIFF, fmt, a JUNK pad, data) so the samples start on a sector. Every file of a take starts on the
same frame and has the same length: they line up in a DAW. Host build only (`RMP_EXTRA_FLAGS=-DRYTM_USB_HOST=1`).

| mode | files | rate (24 / 16 bit) | needs |
|---|---|---|---|
| stereo | `00_MASTER.wav` (MAIN L/R, stereo) | 281 / 188 KB/s | a stock Teensy 4.1 **and a fast SD card** |
| stems | `01_MAIN_L` `02_MAIN_R` `03_BD` .. `10_CY-CB` `11_IN_L` `12_IN_R` (mono) | 1.65 / 1.1 MB/s | PSRAM |

**Recording without PSRAM needs a fast SD card (A1/A2, U3/V30 class).** The Teensy's own buffer is 32 KB in the
release build (teensy/host_stereo.sh: 114 ms of 24-bit stereo, 170 ms at 16-bit), shared with USB audio: **while the
recorder is armed or records, USB audio to the computer is paused** (the device stays, it streams silence; USB MIDI
keeps running) - you record either to the computer or to the card. 16 KB (57 / 85 ms) in the plain host build. A slow card falls behind (dropped blocks, written as silence); record 16-bit then,
or use another card. PSRAM (a 768 KB+ buffer) takes away the need.

`rec=ch:LIST` picks any of ids 0..12 (as the Pi's), within the memory: without PSRAM at most two samples a frame
(MASTER, or two mono channels). Stems without PSRAM are refused: "stems need PSRAM: stereo (MASTER) only on this Teensy".

## Where the code is

| | |
|---|---|
| `core/rec/rec_wav.h` | the WAV header, its placeholder sizes and the repair (moved from pi/engine; the Pi includes it from here) |
| `core/rec/rec_names.*` | the sources and file names, the channel lists, the take names, PLAY/STOP arming: shared by both recorders |
| `core/rec/mini_rec.*` | the Mini's recorder (portable): rings, drops, the card steps; `core/tests/test_mini_rec.cpp` |
| `teensy/voiceloop/rec_mini.h` | the Teensy side: memory, the SdFat backend, the console, the remote REC page and SYSTEM row |
| `web/rec.js` | the web console's Record tab (`node web/rec.test.mjs`; `?mock`, `?mock&psram` for stems) |

The Pi's recorder (`pi/engine/recorder.*`) keeps its own ring and 192 KB chunks; only the portable parts moved. Its
tests (`make -C pi/engine/tests`) are unchanged and pass.

## Design

- **Tap** (`MiniRecorder::in_block`, from `ob_rx` in the USB host interrupt): copies only. The selected samples go
  straight into one byte ring a file, already packed (the file's own bytes). Room is checked in every file's ring
  first, so a 7-frame block goes into all files or none. Nothing runs while idle. 658 B of ITCM (in_block + put);
  everything else is FLASHMEM.
- **Writer** (`MiniRecorder::poll` from `loop()`, between requests, ahead of saves and stream refills): at most one
  card operation a pass. A write is one whole block (4 KB without PSRAM, up to 32 KB with), file offset
  512 + k x block: sector-aligned, whole sectors, no SdFat cache. **It writes only while the card is not busy**
  (`SdioCard::isBusy()`): the card's own programming time is never spent waiting in the loop, so the voices'
  FIFO and the USB host keep running; the ring covers it. A write's transfer is ~0.2 ms (4 KB) to ~1.6 ms (32 KB)
  at the SDIO's ~20 MB/s, like the sampler's stream reads (~0.75 ms).
- **Contiguous files**: each file is reserved at its start (`FsFile::preAllocate`, its share of the free space, at
  most 256 MB a file) so a take does not touch the FAT / exFAT bitmap between blocks; on close it is cut to its
  length (`truncate`) and the header gets the true sizes. Past 256 MB a file grows the normal way.
- **Drops**: a full ring drops the block (counted: `drops`, `silence`); its frames go back in as silence (64 a call)
  when the card catches up, so the files keep time and stay aligned. The page and the log say "the card is behind".
- **Stops**: `rec=stop`, the Rytm's STOP (armed), the Overbridge stream stopping for 1 s (the Rytm unplugged or left
  Overbridge mode: "the Overbridge stream stopped"), the card full (16 MB left: "card full"), 4 GB a file (RIFF),
  a failed write (the card pulled: "write failed"). Every stop but the last drains the ring and closes the files
  properly (header, length).
- **Power loss**: the take being written is named in `/Recordings/.recording`; the next boot gives its files their
  true sizes from their lengths (`fix_pending`, the Pi's header repair) and removes the marker. No scan of all takes.
- **Free space**: the first start after boot scans the FAT / bitmap once (shared with the console's `df`); after
  that it is kept up to date by the bytes written.

### Memory and the ring size

| | ring | block | holds (24-bit) | holds (16-bit) |
|---|---|---|---|---|
| no PSRAM, stereo (release: host_stereo.sh) | 32 KB, RAM1, shared with USB audio's rings (`rec_pool`) | 4 KB | 114 ms | 170 ms |
| no PSRAM, stereo (plain host build) | 16 KB, RAM1 (DTCM .bss, `RYTM_REC_RING_KB`) | 4 KB | 57 ms | 85 ms |
| PSRAM 768 KB, stems | 64 KB a file x 12 | 16 KB | 455 ms | 680 ms |
| PSRAM 1.5 MB, stems | 128 KB a file x 12 | 32 KB | 910 ms | 1.36 s |
| PSRAM, stereo | 1 MB | 32 KB | 3.6 s | 5.5 s |

RAM2 has 6.4 KB left for malloc: no room. RAM1 had 53.8 KB of locals; the ring takes 16 KB, the 13 file handles and
the recorder 1.8 KB: **locals 35.6 KB** (35616 B). That is under `teensy/run_sketch.sh`'s old 40 KB floor, so the floor
is now 34 KB (the stack's measured high-water mark is 7.6 KB, docs/plugins.md). A 32 KB ring (113 ms) would have left
19 KB of locals: the smaller buffer and a fast card were chosen instead. With
PSRAM the ring is extmem'd after `plug_setup()` from what the sample pool's 4 MB reserve still has after the plugin
state (2 MB) and the plugin code arena (~1.1 MB, psram-arena): about 0.9 MB, so 768 KB in practice.

**Shared with USB audio** (the USB audio builds, `-DUAC2_RING_MEM=rec_pool`; 1018): one 32 KB block of RAM1 holds
the recorder's ring or USB audio's two rings (the UAC2 driver's OUT ring, then `uac_in`: 2 KB in the stereo build),
never both. Before `rec=start` / `rec=arm` (console, web, the REC page) `rec_pool_take()` pauses USB audio
(`usb_uac2_pause(1)`: OUT packets received and dropped, `usb_uac2_out_read` silence; `usb_uac2_in_fill` silence and
the IN packet sizes from the clock; nothing touches the rings), then the recorder writes the block from its next
Overbridge block on. Each loop pass `rec_pool_give()` hands it back once the recorder is idle again (the take closed,
disarmed, or the start refused): the rings reset, USB audio resumes, the computer never sees the device go. Arm mode
re-arms after each take, so USB audio stays paused while it is armed. With PSRAM the ring is there and nothing is
paused. State: `rec`'s JSON `usbShared` / `usbPaused`, `rec=info`'s line "the ring shares 32 KB with USB audio", the
USB status line's ", paused for the recorder", the REC page's "USB OFF" (in the free-space row).

What the ring has to cover: the card's busy time after a write (the writer does not wait for it) plus the loop's
longest pass. Estimated, not yet measured on this card: a preallocated sequential write on a decent SDHC/SDXC card
is busy 1-5 ms, with garbage-collection stalls of 20-100 ms now and then; the SD spec allows up to 250 ms (SDHC) /
500 ms (SDXC). The loop's passes are under ~4 ms (a project load 2.7 ms, stream reads 0.75 ms). So **57 ms of
24-bit stereo covers a fast card's usual busy time but not its rarer stalls**, hence the fast-card requirement;
16-bit (85 ms) has more margin, PSRAM much more. The recorder measures it on the device: `worstWriteUs` / `worstWriteMs`, `busy` (polls the card was busy), `ringMax` (the
ring's high-water mark, %), `cardKBps`; each take's closing log line has them all, they stay in `rec`'s JSON until the next take, and `rec=info` prints them
as a log line ("rec: card N KB/s while writing (need N), worst write N us, ring high-water N %, busy polls N, ..."). A card that drops blocks shows
the warning; then try 16-bit or another card.

## Controls

- Console (`:` lines, the web console's too): `rec` (the state as `@{"t":"rec",...}`), `rec=info` (state and card line), `rec=start`, `rec=stop`,
  `rec=arm` (PLAY starts a take, STOP ends it, re-armed for the next), `rec=mode:stereo|mode:stems`,
  `rec=ch:LIST`, `rec=bits:16|24`, `rec=name:PREFIX` - the Pi's `rec=` commands plus `mode:`.
- Web console (web/index.html, Record tab): Record / Arm / Stop, elapsed time, Manual or Arm, Stereo or Stems
  (stems greyed without PSRAM), bit depth, take name, the card's free space and time left at the selection, levels
  of the 13 sources (the ones recorded lit), drops and the last error, the card's figures.
- The Rytm (remote UI, core/ui/page_rec.cpp): the REC page now shows on the Teensy too (LEFT twice from SIDECAR):
  YES records / stops, A turned right arms, left disarms. The SYSTEM page's recorder row shows IDLE / ARM /
  REC m:ss (or OFF without a card).

### IN L/R source (rytm-mods 0030-fx-in, the rui1024 Rytm build)

Sound tab: **IN L/R source: Inputs / FX return** (console `fxin=1|0|?`, `fxin` reports as `@{"t":"fxin","fxin":{"want","state","rytm"}}`).
FX return puts the Rytm's delay + reverb return (stereo) on Overbridge IN L/R (channels 10/11) instead of AUDIO IN. The setting is kept on
the card (`/.rytm/fxin.txt`) and sent to the Rytm again after every connect; the tab shows the state the Rytm confirms
(`not supported by this Rytm build` if it never answers). While on, stems record `11_FX_L.wav` / `12_FX_R.wav` (the Record tab's
levels read FX L / FX R) and the Rytm's SCOPE page names its sources FX L / FX R. Never send channels 10/11 back into the
Rytm through Overbridge (feedback). Details: docs/pi-engine.md, "IN L/R source".

## Tests

- `make -C core test` (core/tests/test_mini_rec.cpp: names and lists, stereo 24/16 bit in a 16 KB ring (4 KB writes), stems
  refused without PSRAM and recorded with it - every sample checked in all files -, a busy card (drops written as
  silence, the file in time), the card pulled, the stream stopping, a full card, PLAY/STOP arming and re-arming,
  the power-loss repair)
- `make -C pi/engine/tests` (the Pi's recorder on the shared parts, unchanged tests)
- `node web/fxin.test.mjs` (the IN L/R switch; `?mock&oldrytm`), `node web/rec.test.mjs`, `node web/webui.test.mjs`, `node web/stream.test.mjs`; the page in `?mock` / `?mock&psram`
- `python3 sim/rui_e2e.py rec status` (the REC page and the SYSTEM row against the emulated Rytm)

## Tester note (Sidecar Mini, recorder)

1. Rytm in OVERBRIDGE mode, Teensy with a **fast SD card (A1/A2, U3/V30)** - without PSRAM it has only a 57 ms
   buffer - (exFAT is best), the web console connected (Record tab).
2. Pick Stereo, 24-bit, a take name; press Record, play, press Stop. Or choose Arm, then PLAY / STOP on the Rytm.
   Also try the Rytm's REC page (LEFT twice from the SIDECAR page): YES records and stops.
3. The takes are in /Recordings/<name>_NNN/ (Files tab, or the card in a computer): `00_MASTER.wav`.
4. Please report the Record tab's "Card:" line after a 5-minute take (MB/s, worst write, buffer most used, card
   busy) - or `:rec=info` in the console - and any "the card fell behind" warning, with the card's make and size.
   Try a 16-bit take too (the setting for slower cards).
5. With PSRAM fitted: Stems gives 12 mono files (01_MAIN_L .. 12_IN_R) of one take.
6. Pull the USB cable to the Rytm while recording: the take should end by itself with its files whole.
