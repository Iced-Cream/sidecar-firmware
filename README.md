# Sidecar | Expander: firmware

> **`dev` is the development branch: unstable, not a release.** It carries work after Sidecar 0.5.0 and can need matching
> builds of both repos; flash at your own risk; keep your 0.5.0 firmware for rollback. Use `main` for a release. Pull
> requests go to `dev` (see CONTRIBUTING.md).

The source of the Sidecar firmware: the engine that runs on a **Raspberry Pi 4B or Pi 5** on the USB port of an Analog
Rytm MKII, or of an Analog Four MKII, and adds voices, effects and a sequencer to it. One SD card boots both boards. The
Pi 4B is the main target, and the one the measurements are made on; the Pi 5 scales its HDMI picture.

> **Sidecar Mini (Teensy 4.1) is phased out.** It is frozen at 0.1.0: no new features. Its source stays in `teensy/`
> and in the history (commit `101a3fb`) for reference, and the 0.1.0 release stays downloadable.

This repository holds the source of **Sidecar 0.5.0**. It adds the SAMPLER machine (a Digitakt-style sample player
next to DSP SAMPLE), a better CHOP (per-slice POLY and GATE, ALL-slice editing, RESAMPLE), SAMPLE EDIT, FILL (UP + RIGHT: p-locks a
range of values onto a track's trigs), the sample browser's preview, quick load and file menu, the Rytm's own
parameters on the XY pad, MIDI learn and poly-track p-locks, an A and a B LFO on each track, user send effects (Plugin
API 3: PHASER, BBD, NIMBUS and GALACTIC) and the MIST texture machine, send FX as inserts on the bus chains, send FX
BASE and WIDTH, the new web page (web-next) with updating from the web page (`.scu` files), SD card images that grow
to fill the card on their first boot, and Sidecar voices about 9 dB lower with a soft limit (they clipped at the
Rytm's track ceiling). MORPH, the looper, sidechain and the MIDI page's learn rows come in 0.5.1. It comes with the
**Rytm firmware 0.5.0**, for **OS 1.74 only** (elekloader modules, applied in the browser patcher to your own stock
OS). A Rytm that keeps the Rytm firmware 0.4.4 works with this card, without the parts that need the new Rytm firmware.
The release notes have each item's status: <https://trigcondition.github.io/sidecar-expander/release-notes/#sidecar-050>.

- Release and binaries: <https://github.com/trigcondition/sidecar-firmware/releases/tag/v0.5.0> (the SD card images,
  the Pi card files, the update file, the user machines and effects, the starter content, the release zip); 0.1.0 and
  0.2.0: <https://github.com/trigcondition/sidecar-expander/releases>
- Documentation, the browser patcher and the web console: <https://trigcondition.github.io/sidecar-expander/>
- The Rytm side (the OS mods the Sidecar talks to) and the A4 mod: [sidecar-mods](https://github.com/trigcondition/sidecar-mods),
  its 0.5.0 release (OS 1.74 only): <https://github.com/trigcondition/sidecar-mods/releases/tag/v0.5.0>

> **Unofficial, experimental software.** Not affiliated with, endorsed by or supported by Elektron. No Elektron
> firmware, and nothing extracted from it, is in this repository.

## Layout

| path | what |
|---|---|
| `core/` | the engine core: voices, machines, send FX, LFOs, presets, the remote pages' UI, the recorder, the device profiles; host tests in `core/tests` |
| `teensy/` | Sidecar Mini, frozen at 0.1.0 (for reference): the `voiceloop` sketch, USB host to the Rytm, the plugin loader (`libraries/RytmPlugins`), the Overbridge block codec (`libraries/RytmHost`), setup scripts for the engine libraries |
| `pi/` | Sidecar: the bare-metal engine on Circle (`pi/engine`, `pi/circle`), the display UI (`pi/ui`), Linux experiments (`pi/linux`) |
| `plugins/` | user machine plugins (`.a64.rmp` on the Pi): the six starter machines `sc_*` (MIT), NUSAW, VOX, HUSH1, RAFFO, BP303 (MIT ports), MD (`mdkit`), SOPHIE (`sophie`, MIT), MODEL E (`modele`, GPL-2.0-or-later, its own licence notes), MIST (`mist`, Plugin API 3, MIT), the user effects `ufx_*` (PHASER, BBD, NIMBUS, GALACTIC; PINGPONG as the example), Noise Plethora packages, test plugins |
| `third_party/` | vendored third-party sources: mdkit (CC0 1.0, the MD machine), TONE3000's NAM A2-Lite engine (MIT, the AMP insert) |
| `sdk/` | the plugin SDK: ABI header, linker scripts, the Teensy Audio shim |
| `sim/` | the Mac-side simulator, protocol and end-to-end tools (some need the emulator, [analog-emu](https://github.com/trigcondition/analog-emu)) |
| `web/` | the Sidecar Mini web console (WebSerial), frozen at 0.1.0 |
| `content/` | the starter content generator (Juno-106 and DX7 patch libraries) |
| `tools/` | packaging, version, plugin pack/install, the portable test runner |
| `docs/` | design notes and measurements |

The version is in `VERSION` (`docs/versioning.md`).

## Build

Third-party engine sources (Mutable Instruments Plaits, DaisySP, Circle, LVGL, ...) are not vendored: the setup scripts
clone each at a pinned commit into git-ignored folders; mdkit and TONE3000's NAM engine are vendored in `third_party/`, ModelDigi in `plugins/modele/third_party/`. See `NOTICE`.

- Engine core on macOS: `make -C core engines && make -C core test` (the last line says `ALL CORE TESTS PASS`).
- Sidecar (Pi 4B and Pi 5): Docker, `pi/engine/build.sh` (`docs/pi-engine.md`, `docs/pi5.md`); one SD card for both boards: `pi/circle/make-sd-dual.sh`.
- Sidecar Mini, frozen at 0.1.0: arduino-cli with Teensyduino, `teensy/run_sketch.sh` (`docs/TEENSY-FIRST-TEST.md`); build it from commit `101a3fb`.
- Plugins: `make -C plugins/sc_acid` and so on (`docs/plugins.md`).
- Starter content: `content/make_content.sh`.

The build scripts were written on macOS; some use BSD `sed -i ''` and macOS tool paths.

## Tests

```
tools/run_portable_tests.sh                  # Linux or macOS: no hardware, no firmware, no cross toolchain
make -C core engines && make -C core test    # macOS
```

Both run on every pull request (`.github/workflows/test.yml`).

## Written offer of source

The released binaries (Sidecar for Pi 4B and Pi 5, version 0.5.0; Sidecar Mini, version 0.1.0) contain GPL and LGPL code, among it Circle
(GPL-3.0-or-later, with its `addon/rp1dsi` display driver linked into the Pi 5 kernel) and the Teensyduino core and SPI library
(LGPL-2.1). The complete corresponding source of version 0.5.0 is this repository at tag `v0.5.0` (of Sidecar Mini 0.1.0: commit `101a3fb`), plus the third-party
sources the build scripts fetch at the pinned commits named in `NOTICE` (Circle with `pi/circle/*.patch`). For three years
from the release, anyone who received a binary can also ask for it on a physical medium for the cost of that medium, or for
the materials needed to relink the Teensy firmware with a modified LGPL part: open an issue in this repository.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

GPL-2.0-or-later (`LICENSE`), except where a file says otherwise; third-party parts and their licences: `NOTICE`,
`THIRD_PARTY_LICENSES`, `licenses/`.
