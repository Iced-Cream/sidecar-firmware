# Spike: a Raspberry Pi 4 on Circle (bare metal) as a "super Teensy"

Goal: find out whether a Pi 4 running bare metal with [Circle](https://github.com/rsta2/circle) can
take the Teensy's role next to the Rytm. That means being the Overbridge USB host, rendering voices and
answering the Rytm mods' SysEx, with fast boot and stable timing, at 10-20x the Teensy's compute.
The spike only answers the risky questions; it ports nothing beyond one test voice.

Reference projects: [MiniDexed](https://github.com/probonopd/MiniDexed) (8 DX7 engines on Circle,
boots in about 2 s), [Overwitch](https://github.com/dagargo/overwitch) (the Overbridge 2 protocol on Linux),
and this repo's `teensy/libraries/RytmHost/` (`ob_block.h`, the host code) and `teensy/voiceloop/usb_host.h`.

## Hardware
- Raspberry Pi 4B and a micro-SD card. Circle boots from FAT32 with its own `kernel8-rpi4.img`.
- A USB-TTL 3.3 V serial adapter for logs:
  - adapter GND to Pi pin 6;
  - adapter RX to Pi pin 8 (GPIO14, TXD);
  - adapter TX to Pi pin 10 (GPIO15, RXD);
  - **3.3 V levels, and do not connect the adapter's VCC**;
  - 921600 baud (spike2 since the audio capture; spike1 115200).
- Optional: an I2S DAC board (PCM5102A type) for local audio; HDMI audio works for step 1.
- The real Analog Rytm MKII on stock firmware for step 2. The emulator can't stand in, because this needs real USB.

## Step 1 - toolchain, boot, audio out (half a day)
- [ ] Install the AArch64 bare-metal toolchain (`aarch64-none-elf` from Arm GNU Toolchain).
      Build Circle for `RASPPI=4 AARCH=64`.
- [ ] Run `sample/01-gpiosimple`, then `sample/29-miniorgan` (or Circle's sound samples) over I2S or HDMI.
- [ ] Build MiniDexed and play it from a USB MIDI keyboard.
- [ ] Measure:
  - time from power-on to first sound;
  - dropouts at 64- and 128-frame buffers over 10 minutes;
  - core load (Circle's multicore: audio on one core, idle cores spare).
- Pass: 5-10 s to sound is fine (relaxed 2026-10-14 from under 3 s: the Rytm itself boots and loads a
  project's samples slower), no dropouts at 128 frames.

### Results so far
- **2026-10-13, Raspberry Pi 3 Model B 1GB** (revision a22082; the user's spare board turned out to be a Pi 3, not a 4)
  [measured on hardware, HDMI log]:
  - spike1 built with `pi/circle/build-rpi3.sh`.
  - Circle 51.1.1 started in AArch64, and the HDMI console worked.
  - **Boot timestamp 3.01 s**: the system timer at `Run()` entry, counted from the GPU firmware start. That includes
    bootcode.bin/start.elf loading from SD, and Circle's own init, about 0.66 s in Circle's log time.
  - The ticks came 1.0058-1.0061 s apart: `MsDelay(1000)` plus about 6 ms for writing and scrolling a line on the HDMI console.
    The first tick, before the screen scrolled, was +1 000 001 us.
  - Not usable for step 2: the Pi 3's USB 2.0 is behind the shared LAN9514 hub, and Circle clamps interrupt endpoints on
    Pi <= 3. Step 2 waits for a Pi 4 (ordered, 4 GB).
- **2026-10-14, Raspberry Pi 4 Model B 4GB** (revision c03115) [measured on hardware, HDMI log]:
  - The same card as the Pi 3 (out/sd plus out/sd3, with a [pi3]/[pi4] config.txt).
  - Circle 51.1.1 in AArch64, and Circle's init again took about 0.66 s.
  - **Boot timestamp 3.62 s**: slower to `Run()` than the Pi 3's 3.01 s. The Pi 4 boots through its SPI EEPROM bootloader
    and start4.elf, and that counts too [inferred].
  - Ticks came 1.0041-1.0044 s apart: an HDMI line costs about 4 ms, against 6 ms on the Pi 3.
  - Well within step 1's target (5-10 s to sound). Optional levers, if it ever matters:
    - `boot_delay=0`, `disable_splash=1`;
    - the cut-down firmware (`start4cd.elf`/`fixup4cd.dat`, no camera/codec);
    - EEPROM settings (`BOOT_ORDER` with SD first, no network/USB probing, `BOOT_UART=0`);
    - a kernel without the screen device;
    - measure power-on to first sound with an external clock (phone video of the LED and a speaker).

## Step 2 - Overbridge over Circle's USB host (the make-or-break, 1-2 days)
- [ ] Write a minimal Circle USB device driver for the Rytm (VID 0x1935, PID 0x0b48):
  - claim the Overbridge interface;
  - select alternate setting 3;
  - run the interrupt endpoints, EP 0x83 Rytm->host and EP 0x03 host->Rytm. The blocks are 368 bytes:
    7 frames of int32 big-endian samples, 12 channels each way, 48 kHz, as in `ob_block.h`.
- [ ] Check what Circle's xHCI stack supports on the Pi 4 (VL805):
  - high-speed interrupt endpoints;
  - the packet size and interval the Rytm asks for;
  - one transfer per microframe without gaps.
  
  This is the main unknown.
- [ ] Do a loopback test: send a sine on host->Rytm MAIN L, and capture Rytm->host MAIN and a voice channel.
      Count gaps and underruns over 10 minutes. Compare the counters with the Teensy host build.
- [ ] Also open the Rytm's USB MIDI interface. The Rytm mods' SysEx (trig reports 0x04/0x03, names
      1A..1E) runs over it.
- Pass: 12 channels each way with no gaps for 10 minutes, and MIDI works at the same time.
- Stop here if it fails. Note why, and decide whether to fix Circle's USB or drop the idea.

### Results so far
- **2026-10-14, first run: Pi 4 4GB + stock Rytm MKII OS 1.73 in OVERBRIDGE** (spike2 at 3cd9756,
  stock Circle with 2 URBs per endpoint, OUT free-running) [measured on hardware, HDMI log]:
  - Enumeration is perfect: high speed, a 263 B configuration with 6 interfaces.
    Interface 1 alt 3 has ep 0x83, and interface 2 alt 3 has ep 0x03, both mps 368 and interval 1.
    Alt 3 was selected and both streams started, with 2 URBs in flight each.
  - The IN count grows by about 6809 blocks/s. 48000/7 = 6857/s is expected, so about 0.7 % is lost.
    Counter gaps grow by about 14/s (12 at 1 s, 82 at 9 s), and there are no short, bad-header or error blocks.
  - OUT stops at 16 blocks and never moves again. Both OUT URBs stay pending (2/2), with no error.
- **Analysis** [inferred from the code and the numbers]:
  - OUT: there is no error, so the xHC never reported a transfer event for the third and later OUT TRBs.
    The OUT endpoint shares the ring and event code with IN, which ran for thousands of TRBs,
    so the Rytm is NAKing OUT for good.
    - The first build sent OUT as fast as the xHC could: one block per microframe, 8000/s, from the moment
      the stream started and before any IN arrived. So 16 blocks went out in about 2 ms.
    - Every working host paces OUT by IN, one OUT block per IN block received: Overwitch, rytm-emu's
      simulated host (`src/usbhost.rs`, credit) and the Teensy's `RytmHost.cpp` (`credit_`/`pump_out`).
    - rytm-emu found that the firmware primes a ring of OUT dTDs. It only enables its OUT reader
      (0x402d73c4, reader 0x40005e9c) once its stream sync has locked to the host, and that sync needs
      IN completions one packet per microframe (RYTM-PORT.md) [measured in the emulator].
    - 16 is the size of that primed ring [inferred]. Either the reader never came on, because the IN gaps
      kept the sync from locking, or an over-full ring at start-up put it in a state it never leaves.
  - IN gaps: Circle's xHCI allowed only 2 URBs per endpoint, each one TRB with IOC.
    Interrupt moderation (IMODI 500 = 125 us) can report a completion a microframe late, which leaves under
    125 us to resubmit before the endpoint runs dry. A missed poll costs the blocks the Rytm drops,
    about 3.4 per gap on average.
- **What the new build changes**:
  - `spike2/circle-xhci-urbqueue.patch`, applied by `build.sh`:
    - a 16-deep URB FIFO per xHCI endpoint;
    - the endpoint lock held from the FIFO slot to the doorbell, so TRB order matches URB order
      when an IRQ resubmits;
    - a barrier before each TRB's cycle bit, because the xHC now fetches ahead;
    - `GetEPState()`;
    - a hook that sees every transfer event's completion code.
  - spike2 defaults:
    - 8 URBs per endpoint (1 ms deep), 1 block each;
    - a fixed URB pool, so nothing is allocated while streaming;
    - **OUT paced by IN**.
  - Options go in `cmdline.txt`, one line, all optional. For example, `oburbs=2 obout=free` reproduces
    the first run. `rytmobdevice.h` lists them:
    - `oburbs=1..16`;
    - `obblocks=1..8` (blocks per TRB);
    - `obout=paced|free|off`;
    - `oboutdelay=N` (IN blocks before the first OUT);
    - `obctr=zero|in` (where the OUT counter starts).
  - One line a second, plus a one-off `start:` line at 2 s (when the first IN, the first OUT and the
    16th OUT block completed):
    ```
    <ms since start>ms IN <rate>/s (measured interval) <total> gap <n> lost <blocks> max <largest> odd <n> sh bad err dt <longest IN gap us> pk <peak L>
      | OUT <rate>/s <total> err age <ms since last OUT> cr <credit> drop <credit dropped>
      | pend <in>/<out> q <xHCI FIFO in>/<out> ep <state in>/<out> rq sf <in>/<out>
      | evI <events> ok sh ms(missed service) x(last other code) o(orphan) evO ...
    ```
- **How to read the next run**:
  - Healthy: IN 6857/s with `gap` and `lost` flat, `dt` under about 1000 us, OUT 6857/s, `age` 0-1 ms,
    `cr` small, `drop` 0, and `ep 1/1`.
  - OUT stalls again with `ep .../1`, `evO` frozen, `age` growing, `cr` at its cap and `drop` growing:
    the Rytm is NAKing.
    - If IN still has gaps at the same time, the sync theory holds. Next try `oburbs=16` or `obblocks=2`.
    - If IN has no gaps, the trigger is something else. Try `oboutdelay=48000` (about 7 s of IN first),
      then `obctr=in`.
  - `ep x/2` or `/4`, or `x`/`ms` counts: an xHC-side halt or error, with the completion code printed.
  - IN `gap` still growing with `oburbs=8`: look at `dt`. Several ms means something holds IRQs off,
    such as the HDMI console write.

### Audio capture (spike2, `pi/circle/capture.py`)
- `uv run --with pyserial python pi/circle/capture.py` (stop flash.sh's monitor first) sends `w`.
  The kernel records the next 10 s of Rytm->host MAIN L/R (channels 0, 1) into a 1.92 MB heap buffer,
  filled in the IN completion. It then dumps over serial at 921600 baud, which takes about 30 s
  while streaming continues:
  - `WAV-BEGIN <bytes> <rate> <channels> <gaps>`;
  - base64 lines of 76 characters (log lines may fall between them);
  - `WAV-END <crc32>`.
- The tool checks the length and crc32 and writes `pi/circle/out/capture-<time>.wav` (16-bit, 48 kHz, stereo).
  It prints the peak/RMS dBFS per channel and the counter gaps during the capture.
- Scaling: the int32 samples are full scale (`ob_block.h` writes int16 << 16, and `sim/obhost.py` divides
  by 2^31), so int16 = sample >> 16, truncated with no dither. The stats line's `pk` stays >> 8 (24-bit units).
- The serial baud went from 115200 to 921600, and flash.sh's `--userbaud` with it. A Pi still running an older
  115200 build needs `USERBAUD=115200 ./flash.sh` once.

## Step 3 - one voice end to end (2-3 days, only if step 2 passes)
- [ ] Port Plaits, the portable C++ that the Teensy build already compiles, as the test voice on one core.
- [ ] Port the trig-report parser from `voiceloop` (0x04 and 0x03, 16 params), so that a Rytm track with a
      Teensy machine (the patched Rytm firmware, `hwus1`-style build) plays the Pi's Plaits voice through
      its analog chain.
- [ ] Measure:
  - CPU per voice compared with the Teensy (Teensy: about 10.8 % per voice for Plaits MODAL);
  - round-trip latency (trig to sound);
  - boot to playable.

### Results so far (spike3, 2026-10-14, Pi 4 4GB + stock Rytm MKII OS 1.73)
- Build [measured]: the Teensy's curated Plaits (eurorack 08460a6, `spike3/setup_plaits.sh` strips PROGMEM/.flashmem)
  compiles as hosted C++ with the toolchain's libstdc++ headers and `-DTEST`, plus newlib's `libm.a`, which Circle's
  `STDLIB_SUPPORT=1` already links (only sinf/cosf are pulled in). `plaitsvoice.cpp` keeps Plaits and Circle headers apart.
  The voice renders just in time in the xHCI IRQ on core 0 (12-sample blocks into an 18-frame FIFO). Every source
  goes through a 200 ms fade-in and a -12 dBFS hard limiter; the voice peaks at -18 dBFS.
- CPU per voice, boot bench, plaits_bench's method [measured]:
  - **600 MHz** (the Teensy's clock): mean 2.37 %, MODAL 3.10 % (max 6.7 %), worst HARMONIC 4.97 %.
    Teensy 4.1: mean about 5.8 %, MODAL 10.3-10.8 %, so the A72 does about 3.4x more per clock.
  - **1500 MHz** (`CCPUThrottle` max): mean 0.95 %, MODAL 1.24 %, worst HARMONIC 1.98 % (about 50 voices a core).
    Boot to playable is 13.7 s on the timer, counting the bench (about 1.7 s).
- MIDI [measured]: Circle's `umidi1` (int1-3-0), read only. Notes on ch 1 (note 36, velocity 51-115) and clock at
  48 packets/s arrive.
- Stuck Rytm [measured]: a spike3 build that hung the Pi mid-stream (fixed: the voice was created after the USB init)
  left the Rytm streaming IN at 6858/s with silent MAIN (pk 0) and NAKing OUT after 16 blocks. spike2 did the same.
  Pi reboots and SET_INTERFACE alt 0 then alt 3 did not clear it. **Power-cycling the Rytm cleared it**, and the
  running kernel picked the Rytm up again by plug-and-play. Ordinary reboots (flash.sh) since then are fine.
- Live, Plaits MODAL at 1500 MHz, the rendering inside the xHCI IRQ [measured]:
  - 4000 renders/s at **1.25 % per voice**, max 1.29 % (3.2 us of a 250 us block). FIFO 0-11 frames.
  - The OUT voice peaks at -21 dBFS.
  - IN/OUT 6858/s, 0 gaps and 0 lost over the runs (minutes), no xHCI errors.
  - MIDI note-on arrival to the render that carries the trigger: avg 40-140 us, max 364 us.
- Latency, `capture.py` (4 channels; channel 3 marks note-on arrivals and trigger frames) and
  `spike3/latency.py` [measured]:
  - Setup: track 1 of the playing pattern sends ch 1 note 36 on its steps, 70 notes in 10 s.
  - OUT -> IN loop through the Rytm's mix: 278 samples = **5.8 ms** (GCC-PHAT; voice in MAIN at -10.9 dB).
    An earlier capture gave 289.
  - MIDI arrival -> trigger frame sent: median 0.25 ms, max 0.38 ms.
  - The note-on reaches the Pi **3.2 ms before** the step's drum onset appears in MAIN (Overbridge IN).
  - **Plaits in MAIN after the Rytm's drum on the same step: median 2.9 ms** (2.6-3.7 ms, 70 of 70 notes).
    The drum onset is taken from MAIN with the voice subtracted. The onset detector reads about 0.1 ms late
    on synthetic data [inferred].
  - What the listener hears out of the Rytm's main outputs adds the same DAC path to both.
- WAV: `pi/circle/out/capture-20261014-212244.wav` (4 channels), and the IN MAIN L/R as a stereo copy in
  `~/Downloads/rytm-plaits-pi4.wav`. The Rytm pattern plus the Pi's MODAL voice on note 36, peak -13.5 dBFS.
- capture.py now zero-fills garbled base64 lines (a 7.7 MB dump once lost bytes) instead of failing.
- Also seen: once the bench lines filled the console, HDMI scrolling during the stream start caused IN gaps.
  spike3 now turns the screen log off before the USB init.

## What to write down at the end
- Boot time, the dropout and gap counts, CPU per voice and latency against the Teensy.
- Circle's USB limits found, and what porting the rest would take: the plugin loader (.rmp, Cortex-M7
  Thumb code, so plugins would be recompiled for AArch64), the SD sample streaming, presets and the slot
  protocol.
- A recommendation: go (a `rytm-pi` sub-project), Linux plus Overwitch instead, or stay on the Teensy.

## Fallback if step 2 fails: the Linux route
Use a minimal image: Buildroot or Yocto, with the Raspberry Pi kernel and PREEMPT_RT. Set it up the way
AbletonOS does on Move (from its recovery image: kernel `5.15.92-rt57-v8`, SysV init, A/B root partitions):
- `chrt` 90/91 on the USB and audio interrupt threads;
- all other IRQ threads and work-queue cpumasks on cores 0-2 (`taskset -p 7`, cpumask 7);
- the audio interrupts and the engine's audio thread alone on core 3.

Use an Overbridge host on libusb (Overwitch as the reference), and a web UI served by the Pi. Measure the
same numbers as steps 2-3, plus boot time (target 5-8 s, behind a splash screen).
See `docs/superpowers/specs/2026-10-11-platform-architecture-design.md`.

## Notes
- Overbridge has one host at a time: the Pi and the Teensy can't both be connected as host.
- Rust is an option later (bare-metal AArch64 crates exist), but Circle's C++ USB stack is the
  fastest way to answer step 2.
