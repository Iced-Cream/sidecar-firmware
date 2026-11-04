# Sidecar | Expander: firmware (generic engine)

> **A development fork, not a release.** This branch strips the Sidecar's Pi engine down to generic services. It does
> not go upstream, and nothing here is published as firmware. Keep a released card for rollback.

## About

The source of the Sidecar engine: a bare-metal kernel for a **Raspberry Pi 4B or Pi 5** on the USB port of an Analog
Rytm MKII (or another box with a device profile). It provides only **generic services**. No Rytm mod has a Pi-side
counterpart of its own; a mod reaches the Pi through the host link's generic commands. The services are:

- the Rytm link (Overbridge, its USB MIDI, the USB-C gadget to the computer, the device hello and profiles);
- the Rytm's step clock and transport;
- the remote screen as a transport (popups, the pairing code, one client);
- record storage and Pi-side parameter locks (51 STORE, with the project's name);
- a card browse for the Rytm's file browser;
- the USB-A MIDI devices;
- the audio return's processor slots (empty by default);
- OS updates through the Sidecar and the Rytm MIDI bridge for tools;
- the platform: storage, the network, a small web page, pairing, Wi-Fi, temperature and supply warnings.

The voice engine this was until 2026-11-04 (machines, FX, sequencer, sampler, recorder, pools, presets, bundles, library,
plugins, the Teensy firmware and the local display) is in git history only: commit `3a8bf7d` is the last one with all of
it.

- The Rytm side (the OS mods the Sidecar talks to): [sidecar-mods](https://github.com/trigcondition/sidecar-mods).

> **Unofficial, experimental software.** Not affiliated with, endorsed by or supported by Elektron. No Elektron
> firmware, and nothing extracted from it, is in this repository.

## Layout

| path | what |
|---|---|
| `core/` | the engine core: the host link's router and the generic services (`docs/core.md`); host tests in `core/tests` |
| `pi/` | the bare-metal engine on Circle (`pi/engine`, `docs/pi-engine.md`), Circle's build, patches and SD card scripts (`pi/circle`), Linux experiments (`pi/linux`) |
| `sim/` | the host link's wire spec (`sim/protocol.py`) and its tests |
| `tools/` | the version stamp, the portable test runner, the Rytm MIDI bridge's relay |
| `docs/` | design notes and measurements |

The version is in `VERSION` (`docs/versioning.md`).

## Build

Circle is not vendored: `pi/circle/build.sh` clones it at a pinned tag into a git-ignored folder. See `NOTICE`.

- The engine core on the host: `make -C core test` (the last line says `ALL CORE TESTS PASS`).
- The Pi 4B and Pi 5 kernel: `pi/engine/build.sh` (Docker, or a local aarch64 toolchain; `docs/pi-engine.md`,
  `docs/pi5.md`); one SD card for both boards: `pi/circle/make-sd-dual.sh`.

## Tests

```
tools/run_portable_tests.sh     # Linux or macOS: no hardware, no firmware, no cross toolchain
```

It runs on every pull request (`.github/workflows/test.yml`).

## Written offer of source

A kernel built from this repository links Circle (GPL-3.0-or-later), so it is distributed under GPL version 3, which
GPL-2.0-or-later allows. The complete corresponding source of a build is this repository at its commit, plus Circle at the
tag `pi/circle/build.sh` pins with `pi/circle/*.patch`.

## Licence

GPL-2.0-or-later (`LICENSE`), except where a file says otherwise; third-party parts and their licences: `NOTICE`,
`THIRD_PARTY_LICENSES`, `licenses/`.
