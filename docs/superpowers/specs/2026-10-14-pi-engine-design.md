# Pi engine on Circle with a shared engine core (2026-10-14)

Status: design, approved by the user 2026-10-14. It builds on:
- `2026-10-11-platform-architecture-design.md`: one protocol, several engines, several hosts;
- the finished Pi spike, `docs/pi-circle-spike.md` steps 1-3.

**Spike results on a real Pi 4 4GB with a stock Rytm MKII** [measured]:
- Circle boots to the kernel in 3.6 s;
- Overbridge ran 12 minutes with 0 gaps each way;
- one Plaits voice costs 1.25 % of a core at 1.5 GHz;
- the voice lands 2.9 ms after the Rytm's own drum on the same step.

## Goal

A Raspberry Pi 4 running Circle (bare metal) becomes a full voice engine for the patched Rytm. It does what the Teensy does today:
- per-track voices through the Rytm's analog chain;
- the built-in machines with their names, knob labels and presets;
- user slots, the sample pool, and DSP SAMPLE streaming from the SD card;
- the host-link protocol.

It uses the **same engine code** as the Teensy, through a shared core library. That core is also the base for later boards: Pi 5, i.MX RT1170.

## Decisions (user, 2026-10-14)
- **Approach A: a shared core library.** The engine is pulled out of `teensy/voiceloop` into portable C++. The Teensy and the Pi each get a thin platform layer. No fork, no Linux.
- **v1 = all built-in engines.** That is Plaits, Juno, 6-OP FM, DigiDrum, DaisySP and DSP SAMPLE, plus presets, the sample pool, the slot protocol and names. **The .rmp user plugins come in v2:** they need an AArch64 loader and multi-target packages, see the platform spec section 3.
- **Test path: emulator first, then the real Rytm.**
  - Phase 1 runs against rytm-emu over Ethernet, so every mod is exercised without flashing.
  - The real Rytm comes later: first stock (MIDI notes, MAIN), then the reviewed mod build after the flash gate in rytm-mods `docs/FLASHING.md`.

## Layout (all in rytm-teensy)

```
core/            portable C++17, no Arduino, no Circle, no dynamic allocation in the audio path
  engines/       Plaits, Juno, FM (DX7 6-op), DigiDrum, DaisySP, DSP SAMPLE (stream voice)
  voices.*       voice allocation, per-voice state, render(block) -> per-voice int32/float buffers
  host_link.*    the mods' SysEx parser and replies (0x03/0x04 trigs and params, 0x11 note-off,
                 0x10 project, 0x14 pool, 0x19-0x1E user slots, names), moved from voiceloop.ino's
                 report() (voiceloop/host_link.h is the Teensy's USB host transport and stays there)
  presets.*      presets (generic + native), knob write-back
  pool.*         sample pool, card index
  slots.*        slot maps per project (v1: built-in machines only in the slot list)
  platform.h     the interface the core calls (below)
teensy/voiceloop/  the Teensy platform layer + core; behaviour unchanged
pi/engine/       the Circle kernel: the Pi platform layer + core
```

`core/platform.h` holds the only things the core needs from a board:
- file access: open, read, seek and list, for the SD layout;
- a microsecond clock;
- logging;
- a random seed;
- the per-block "now" for stats;
- a CPU-time measure for the crash and overrun guard.

Audio and USB are not in it. The platform layer calls `core::render(block)`, and it feeds the host-link bytes in.

## The Pi engine at run time

- **Core 0:**
  - the xHCI interrupt;
  - the Overbridge host, from spike2/3: 8 URBs per endpoint, OUT paced by IN, the patched Circle xHCI;
  - USB MIDI and SysEx (Circle's USB MIDI driver);
  - the host-link byte stream into the core's parser.
- **Cores 1 and 2:**
  - voice rendering in blocks, with the voices split across the two cores;
  - results go into lock-free SPSC FIFOs that the OUT completion drains (spike3's FIFO, widened to the voice channels).
- **Core 3:**
  - FatFs on the SD card: samples, presets and projects, the same `/MACHINES`, `/PRESETS`, `.rytm/` layout as the Teensy's card;
  - DSP SAMPLE streaming;
  - the serial console;
  - the stats.
- **Audio out:**
  - each Pi voice goes on the Overbridge channel of its Rytm track, into the track's analog chain as on the Teensy, using the mods' channel map, `ob_block.h` and the platform spec's device profile;
  - MAIN carries nothing by default;
  - a -12 dBFS limiter and a 200 ms fade-in on every output path.
- **Clock:** the Rytm is the clock master. Blocks are paced by the Overbridge IN stream, as in spike3.

## Test path

### Phase 1: the emulator over Ethernet
- rytm-emu's Overbridge link (`src/oblink.rs`, TCP, today 127.0.0.1:7301) gets:
  - its bind address, which `--ob-link ADDR` already takes: `--ob-link 0.0.0.0:7301` serves the LAN;
  - a message pair carrying the USB MIDI and SysEx bytes in both directions. It carries the mods' reports and the engine's replies, and replaces today's `T`/`P` stand-ins when the mods send real reports.
- Today the emulator asks the host for frames (`N`/`A`). The Pi side implements this protocol as a second **host transport** next to the USB one, both behind one interface: blocks in, blocks out, MIDI bytes. The engine core does not know which one it runs on.
- The Pi uses Circle's network stack (the Pi 4's on-board Ethernet) with a static address: `ip=`/`netmask=` in `cmdline.txt`, default 192.168.77.2/24. A direct cable to the Mac at 192.168.77.1 has no DHCP server. The emulator's address comes from `cmdline.txt` (`emuhost=IP:PORT`, default 192.168.77.1:7301) or the console's `:link=tcp:IP:PORT`.
- What runs there: the full mod build from rytm-mods (the `hwus1`/`us1` style builds) in rytm-emu.

### Phase 2: the real Rytm over USB
- Stock OS first: MIDI notes drive the voices, and audio goes to MAIN. This is already proven by spike3.
- After the flash gate: the reviewed mod build, with per-track voices.

## Errors and safety
- **Voice guard:** a voice that overruns its time budget or faults (exception handler) is muted and flagged `!NAME`, as on the Teensy. The others keep playing.
- **Clean stop:** on reboot (the reboot magic or a fatal error) the Pi stops the Overbridge stream before resetting. That way the Rytm never stays stuck; in spike3 a Pi crash mid-stream needed a Rytm power cycle.
- **Underflow:** if the FIFO runs empty, OUT gets silence, never stale data, and the event is counted.
- **USB detach and reattach:** handled by Circle's plug-and-play; the stream restarts from zero.
- **Development:** the serial bootloader, `flash.sh` with the reboot magic, and `capture.py` stay.

## Testing
- **Core on the Mac:**
  - the core builds as a host library;
  - `sim/` (Python, already used for the Teensy protocol) drives it;
  - renders are compared with the current Teensy renders of the same notes and parameters. They should be bit-identical where the engine is float-deterministic, otherwise within a stated tolerance.
- **Teensy refactor gate:**
  - voiceloop on the core passes the existing end-to-end tests: e2e serial and host, 153 checks;
  - it gives the same `plaits_bench` numbers within noise;
  - the same ITCM and RAM budget.
- **Pi, automated over serial (flash, logs, `w` captures):**
  - CPU per core and per engine;
  - gaps and lost blocks;
  - FIFO levels;
  - one note per engine checked for pitch and level in a capture.
- **Phase 1 acceptance:** in rytm-emu with the mod build:
  - a pattern with Pi machines on several tracks sounds on the right tracks;
  - names, knob labels and presets appear on the Rytm screen;
  - knobs and p-locks change the sound;
  - 0 gaps over 10 minutes.
- **Phase 2 acceptance:** the same on the real Rytm once flashed. Before that, the stock-Rytm test from spike3, with all engines on MIDI notes.

## Order
1. Extract the core. The Teensy is unchanged, and the refactor gate passes.
2. The Pi platform with all engines on MIDI notes (stock Rytm, USB), on multiple cores.
3. The TCP host transport to rytm-emu, with the full mod protocol (phase 1 acceptance).
4. Performance: core split, CPU budget, and voice counts per engine.
5. The real Rytm with the mod build (phase 2), after the flash gate.

## Later (not v1)
- **v2:** .rmp user plugins on the Pi (AArch64 loader, multi-target packages, user slots U1-U5).
- The management protocol and web UI (platform spec section 2).
- **Setup over USB, like Ableton Move** (user request, 2026-10-14; high UX priority once the web UI exists):
  - Plug the Pi into a Mac, PC, tablet or phone by USB and open `http://sidecar.local` (project name Sidecar, user 2026-10-16) in a browser. No Wi-Fi, LAN cable or screen is needed.
  - First use: configure the Pi, e.g. enter the Wi-Fi network and password, update, and manage machines and samples.
  - How:
    - the Pi 4's USB-C port in device (gadget) mode presents a USB network adapter: CDC-NCM, with CDC-ECM as fallback; macOS, Linux, Windows 10+ and iPadOS/Android support these natively [inferred, check per OS];
    - a small DHCP server on the Pi gives the host an address;
    - an mDNS responder publishes `sidecar.local`;
    - the web UI is served by an HTTP server. Circle has a network stack, an HTTP daemon and an mDNS publisher, and USB gadget support on the Pi 4 [check: which gadget classes exist; CDC-NCM likely to be written].
  - The web app is served by the Pi itself (user, 2026-10-16): the same page as the Teensy's `web/` console, but from the Pi's HTTP server at `sidecar.local`, over the USB-C network gadget (primary), Ethernet, or Wi-Fi.
  - Wi-Fi off by default (decided 2026-10-16): with Circle nothing initialises the Wi-Fi chip unless the WLAN driver is built in, so today it costs nothing. If Wi-Fi is added for setup/updates, it stays off until the user enables it: from the Rytm's "Sidecar Manager" screen (a mod menu item, like Move Manager; entering it turns Wi-Fi, web and file access on, leaving it turns them off again, so management never runs during a performance; docs/BACKLOG.md) or the control panel, because its SDIO/firmware work and interrupts would compete with core 0's USB work, and its bursts are a possible RF source next to the Rytm's analog lines. USB-C network and Ethernet need no radio.
  - Requirement (user, 2026-10-14): the connected device must be able to power the Pi over USB-C, e.g. a laptop. Phones and low-power tablets are out of scope for now.
  - Caveats:
    - on the Pi 4 the USB-C port is also the power input, so the host powers the Pi. A separate supply (GPIO 5 V pins or a USB-C power/data splitter) is only a later option for phones;
    - the Rytm stays on the Pi's USB-A host ports, so both work at once.
  - The Teensy equivalent is WebSerial over its USB device port (platform spec section 2).
  - Reference: study how AbletonOS does it in the Move recovery image (`Move-Image-2.1.1.zip`, already explored in Docker in October 2026). Look at its USB gadget setup (configfs: which function, NCM, ECM or RNDIS), the DHCP server, the mDNS name and the web server. Use it to learn the configuration only; copy no code.
- **A display or touchscreen with LVGL** (user decision, 2026-10-15; screen, views and case in the platform spec's "Pi display and case"):
  - The UI is built with **LVGL** (MIT licence; https://lvgl.io), using Circle's LVGL add-on (`addon/lvgl`). The add-on draws to the HDMI frame buffer (or the official v1 DSI display) and takes touch from Circle's USB digitizer or touchscreen drivers [check: the add-on's version and which touch drivers it covers on the Pi 4].
  - It runs on **core 3** next to FatFs, the console and the stats. Cores 0-2 (USB, render) never wait on it. It reads engine state through lock-free snapshots and sends edits through the same paths as the host link (parameter write-back), so the UI is never in the audio path.
  - It redraws only regions that changed (dirty areas), at up to 30 fps, and sleeps when nothing changes.
  - **The Teensy gets no display:** it stays lightweight, and its single core belongs to the voices.
- **Other boards:** the Pi 5 needs a new platform layer only (Circle supports it), and so would an RT1170.
- Boot-time trimming: `disable_splash`, `boot_delay=0`. The target is 5-10 s to sound, already met.
