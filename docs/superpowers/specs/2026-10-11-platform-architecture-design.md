# Platform architecture: one protocol, several engines, several Elektron hosts (2026-10-11)

Status: design. It extends the umbrella spec `2026-10-10-user-machines-design.md` (sub-projects 3-5) and
the user-slots spec `2026-10-10-user-slots-rytm-design.md`. Nothing here is built yet, except where marked
**[built]**.

## Goal

People choose the engine box that suits them, and plug it into the Elektron they own. Everything above
the hardware stays the same: the protocol, the plugins, the content and the web UI.

- **Engines**: Teensy 4.1 (small, cheap, instant-on; **[built]**), Raspberry Pi 4/5 on Linux or bare metal
  (much more power, its own web UI; spike in `docs/pi-circle-spike.md`), and later boards such as the i.MX RT1170.
  - Positioning (user, 2026-10-13):
    - The Pi is the easier entry point for new users: a kit you plug together, with no soldering, lots of RAM and CPU, and
      bare-metal timing.
    - The Teensy is the compact, much cheaper option for people who can solder: the USB host header, and PSRAM if wanted.
- **Elektron hosts**: Analog Rytm MKII (**[built]**, rytm-mods), Analog Four MKII next (**[check]**: Overbridge
  channels, the firmware platform, what the mods can reach), and others later.

## The three interfaces

```
  Elektron host (Rytm, A4, ...)        engine (Teensy, Pi, ...)             web UI (browser)
 +--------------------------+  1  +---------------------------------+  2  +----------------------+
 | patched OS: trig reports,|<--->| engine core: voices, plugins,  |<--->| companion: machines, |
 | names, slot menu, tags   | USB | presets, pool, store, slot maps|     | content, settings    |
 | Overbridge audio         |     |                                 |     |                      |
 +--------------------------+     +---------------------------------+     +----------------------+
                                     3: packages + content (SD / disk)
```

### 1. Host link: Elektron <-> engine (**[built]** for the Rytm)
- Overbridge audio (USB high speed, the engine is the USB host) plus the mods' SysEx (`F0 7D 52 54 cmd ver ...`):
  - trig and parameter reports `0x04`/`0x03`, with the sound tag;
  - note-offs `0x11`;
  - the user-slot messages `0x19`..`0x1E` (asks, meta, lists, write-back, slots changed, assign);
  - the project `0x10`;
  - the pool table `0x14`.
  
  The reference is `sim/protocol.py`, with the user-slots spec and `rmp_proto.h`.
- **Device profile.** What differs between Elektron hosts lives in a profile, not in the protocol. The
  protocol itself stays the same for every host. The profile says:
  - how many tracks and voices there are, and which voice serves which track (shared voices);
  - the Overbridge channel map in each direction: which channel is MAIN, the voices, the FX return
    (Rytm with 0024: AUDIO IN replaced), the inputs;
  - where an engine voice enters the host's audio path. On the Rytm a voice's digital layer goes through
    the analog filter, overdrive and amp. On the A4 **[check]**: per-track injection, or only MAIN/FX
    return;
  - the parameter count per report (16 on the Rytm), the machine-slot range (the Rytm's U1..U5 = 35..39), and
    which features the mods provide on that host (TEENSY folder, looper, ob-mix).
- A new message, `0x1F` **profile** (host -> engine, sent with `0x10` and on attach): the host id and
  firmware/mod version, then the profile fields above. An engine that has no profile for the host logs it
  and stays silent. A Rytm without `0x1F` (today's mods) is treated as profile "rytm-mk2/1.73".
- Engines keep the Overbridge host and the SysEx parser in a **host adapter** module per host
  (`rytm`, `a4`, ...). The engine core never sees host-specific details.

### 2. Management protocol: web UI <-> engine (new; replaces the Teensy's dev frames)
One versioned message set, carried over whatever link the engine has:

| Engine | Transport | Where the UI comes from |
|---|---|---|
| Teensy | WebSerial (USB device port), framed binary | a static page (GitHub Pages), Chrome/Edge |
| Pi (Linux) | WebSocket on the Pi's own web server | the Pi serves it: `http://sidecar.local` (the Move approach) |
| Pi (bare metal) | WebSerial or a minimal HTTP/WebSocket server, if Circle's network stack allows | static page or the Pi |

- **Framing.**
  - Messages are `{version, id, op, payload}`, with request/response ids and async events.
  - Binary chunks are used for files, with a CRC per file (the existing install CRC).
  - One encoding is used for every transport: CBOR or a small fixed binary layout; the plan picks one.
  - WebSerial and WebSocket only differ in transport.
- **Operations.** They map 1:1 onto what the Teensy already does as dev frames:
  - `info`: the device-info report, extended into the capability list below;
  - `store.list/install/remove/export/wipe`;
  - `slots.get/assign/clear` per project;
  - `card.list/put/remove` for samples, presets and packages;
  - `project.get/set`;
  - `presets.list/put/remove`;
  - `settings.get/set` for the platform features;
  - `log.subscribe`;
  - `firmware.update` (Teensy: the bootloader over WebHID; Pi: an A/B partition update, the AbletonOS pattern).
- **Capabilities.** The `info` response lists what this engine can do, and the UI shows only those features.
  It covers:
  - the target arch and the plugin API version;
  - the package formats accepted;
  - CPU budget, RAM, PSRAM;
  - arena and state limits;
  - store size and free space;
  - voices;
  - the supported hosts and profiles;
  - feature flags: sample streaming, granular/large RAM, master FX, recorder, looper.
  
  The per-machine "fits / won't load / heavy" verdict uses these numbers. That is umbrella spec section 3.
- **Safety, carried over from the Teensy rules.**
  - Installs and wipes are refused while the transport runs.
  - A wipe needs an explicit confirmation op.
  - No operation may stall audio. Long jobs run in steps and report progress as events.
- The Teensy keeps its current dev frames until the management protocol replaces them in the companion
  sub-project. `tools/rmp_install.py` becomes a client of the new protocol.

### 3. Packages and content
- **Multi-target packages.** A package (`.rmp`, format 4) has one manifest: id, version, knobs, labels, enums,
  presets, licence. Format 3 is **[built]**. The package also carries one code image per target arch:
  - `armv7em` for the Teensy, as today: position-independent, run from the arena;
  - `aarch64` for the Pi: a relocatable ELF, loaded by the Pi engine's loader;
  - later `armv7em-rt1170` and others.
  
  An engine installs a package only if it has its arch. The UI shows the targets.
- **Plugin API.**
  - The plain C API, version 2 today **[built]**, stays the contract on every engine.
  - Engines may raise the limits in their capabilities (state per voice, arena, CPU budget).
  - A plugin declares a minimum. Packages that fit only on a large engine are marked as such.
  - Host API functions behave the same everywhere (`pool_info`/`pool_read`, presets, random), so a plugin
    never knows which engine runs it.
  - An official Rust template (`no_std`, `extern "C"`) joins the C one in the SDK sub-project.
- **Content layout** is the same on the Teensy's SD card and the Pi's disk:
  - `/MACHINES/<id>-<ver>.rmp`;
  - `/PRESETS/<id>/`;
  - the sample folders;
  - `/.rytm/slots/<PROJECT>.txt` and `/.rytm/pools/<PROJECT>.txt`;
  - JUNO and DX7 banks.
  
  The `.rytm` folder becomes `.elk/<host>/` once a second host exists, so projects of different hosts don't
  collide. The plan migrates it.

## Engine-specific notes
- **Teensy**: the reference engine. Its limits are the floor that every plugin must run within, unless it
  declares more: 32 KB arena, 16 KB state per voice without PSRAM, the CPU guard.
- **Pi on Linux**:
  - A minimal Yocto/Buildroot image with the PREEMPT_RT kernel.
  - Audio and USB interrupt threads pinned to one core at high RT priority, everything else on the
    other cores. This is what AbletonOS does on Move: `5.15.92-rt57`, `chrt` 90/91 on the SPI threads,
    IRQs and work queues on cores 0-2, audio IRQs on core 3.
  - An Overbridge host built on libusb (Overwitch documents the protocol).
  - A/B root partitions for updates.
  - Boot time is a known cost: a splash screen, a target of 5-8 s.
- **Pi bare metal (Circle)**: instant-on and stable timing. Its feasibility depends on Circle's high-speed USB
  host (spike step 2). Same engine core, a different platform layer.
- **One engine core**: the voices, the plugin loader interface, presets, pool, store logic and slot
  maps are portable C/C++. Each engine adds a platform layer (audio and USB, storage, the transport for
  interface 2). That is how one code base serves the Teensy, a Pi on Linux and a Pi on bare metal.

## Several Elektron hosts on one engine (Pi)
A Pi can be the Overbridge host of several Elektron devices at once, e.g. a Rytm and an A4 MKII. Overwitch
already runs several devices at the same time on Linux.
- **Bandwidth**: about 2.5 MB/s each way per device, well within the Pi 4's USB 3 controller (4 host ports).
- **One host adapter per connected device.** Each has its own profile, channel map, SysEx parser and slot
  state. Voices, plugins, presets and the store are shared: a plugin may play on both hosts at once.
- **Audio clocks.** Each Elektron is the clock master of its own stream, so two "48 kHz" streams drift.
  - The engine takes one device as the audio master.
  - It runs the others through an adaptive resampler: their render and capture paths are resampled to
    the master clock. Overwitch's resampling against JACK is the reference.
  - Resampler latency and quality are measured in the Pi engine's plan.
- **Tempo**: one Elektron is the MIDI clock master, as usual. The engine can forward clock, transport and
  program changes between the hosts' USB MIDI ports. This is a setting in the web UI.
- **Web UI**: the connected hosts are shown side by side, each with its own projects and slots.
- **Management protocol**: operations that touch a host (`slots.*`, `project.*`) carry a `host` id from
  `info`'s list of connected hosts.
- **Teensy**: one host. A hub plus two Overbridge streams, a resampler and voices for both machines doesn't
  fit its one core and 1 MB of RAM reliably; not a goal.

## Analog Four MKII (next host) - research first
To check before any design work:
- the A4 MKII's Overbridge channel map in each direction;
- whether host->device audio can enter a track's analog path, or only MAIN/FX;
- the A4's firmware platform and how much of rytm-mods' tooling applies (emulator support in rytm-emu,
  code caves, the SysEx hook points);
- what a "machine" means on a 4-track polysynth: engine voices as an oscillator source feeding the A4's
  analog filters, if the path allows.

The protocol and engines above need no change for that. Only a profile, a host adapter and a new mods repo
are needed.

## Pi display and case (idea, 2026-10-12; after the Pi spike)
A touchscreen on the Pi engine, in a 3D-printed case that stands behind the Rytm and A4.
- **Screen**: 7" IPS, capacitive touch, 1024x600, HDMI picture plus USB HID touch (e.g. Waveshare 7" HDMI LCD (C)).
  This works with both Circle (HDMI frame buffer plus its USB digitizer driver) and Linux. Not a resistive
  SPI screen (e.g. Joy-IT RB-LCD5): it needs a pen and the SPI link is slow. The official Touch Display 2 (DSI) is
  Linux only on a Pi 4, since Circle on the Pi 4 supports only the v1 display.
- **Views**, by data source:
  - from data we already have:
    - the machine editor: 16 parameters, both pages, names, enums and presets;
    - level meters for the 12 Overbridge channels plus the FX return (0024);
    - an oscilloscope or spectrum per voice;
    - a sample browser with waveforms and preview;
    - CPU per voice and the crash guard;
  - via the management protocol: slots and presets by touch;
  - later: recorder and looper (stems, takes), and Rytm and A4 side by side.
- **Sequencer view**:
  - The trig reports (`0x04`/`0x03`) give a live grid that fills in while the pattern plays.
  - Steps that don't fire and the whole pattern ahead need more: stock pattern SysEx dumps, or (better) a new
    mod that reports a pattern's step data when it loads or is edited. That second option also allows an
    editable step grid later.
- **Touch as a controller**: XY pads, faders and macros that send parameter changes back to the host (the
  `0x1C` write-back path or CCs).
- **UI technology**:
  - Linux: the same web companion, full screen in a kiosk browser on cores 0-2. Audio and USB stay alone on
    core 3. One UI to build, shown in two places.
  - Circle: a native LVGL UI on its own core, which means a second UI to maintain.
  - This counts towards the Circle/Linux choice after the spike. Audio and USB results still decide first.
  - **Decided 2026-10-15:** the Pi runs Circle, so the display UI is native LVGL on core 3 (see the Pi engine spec, "Later"). The Teensy gets no display.
- **Case**:
  - A wedge with the screen at about 35-45 degrees, low enough not to block the Rytm's and A4's rear connectors.
  - The Pi 4 lies flat in the base with a passive heatsink against a vented floor, so it is silent.
  - Power, the host USB and Ethernet come out at the back, with one USB port on the side.
  - A parametric OpenSCAD model, made once the screen is chosen and the Rytm's rear height is measured.

## Sub-project order (updated)
1-2 done (plugin loader, Rytm user slots). Next:
- **3a** the management protocol with a Teensy implementation;
- **3b** the web companion UI on it (waiting for the user's example tools);
- **Pi spike**;
- then the Pi engine (Linux or Circle);
- the in-browser OS patcher;
- the SDK (C and Rust templates, multi-target packaging);
- **A4 research**.
