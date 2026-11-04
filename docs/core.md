# The engine core (`core/`)

`core/` is the portable C++17 core of the Sidecar's Pi engine: the generic services the host link reaches, with no Circle, no allocation in the audio path and no Pi-side counterpart of any Rytm mod. `pi/engine` compiles it hosted (`pi/engine/Makefile`'s `CORE_SRC`), and `make -C core test` builds it with the host's compiler (macOS or Linux) for the tests.

Since the strip-down of 2026-11-04 this is all of it. The voice engine's core (the machines, FX, sequencer, sampler, pools, library, plugins and the Teensy port) is in git history only: `3a8bf7d`.

## Layout

| folder | |
|---|---|
| `generic/` | the host link's router: a whole F0 .. F7 from the Rytm (or another box) to the service it is for (`gen::sysex`), every other command counted and dropped; the link reset and its listeners (`on_link`); the idle steps (`idle_save`) |
| `device/` | the device profiles and the 4F hello (`docs/device-profiles.md`): which box is on the link, its messages translated, its state folder; `digitakt.*` the Digitakt mk1 profile, `audio.h` a box's Overbridge stream as a table |
| `screen/` | the remote screen as a transport (30-3F): the Rytm's hello and caps, the connect splash, popups (a status popup waits for a closed page; the pairing code's modal goes `over` one), one client that draws pages |
| `ui/` | the 1-bit drawing the screen and a client use (`core/ui/README.md`) |
| `clock/` | the Rytm's step clock (2C) and transport (2D): pattern, BPM, the tracks' steps |
| `proc/` | the audio return's processor slots: four `Proc` slots between Overbridge IN and OUT, their parameters and link reset; empty by default |
| `browse/` | 0D -> 0E: a folder of the card's `/Samples` for the Rytm's file browser (rytm-mods 0023), read on the card core a few entries a step |
| `devmidi/` | the USB-A MIDI devices: plug / unplug, their channel and realtime messages to the Rytm (thru), to the slots (`Proc::midi`) and to listeners; held notes released at an unplug |
| `kv/` | 51 STORE: the record store on the card (`kv.h`, `kv_card.*`), its link to the host (`link.*`: answers from `host_step`, card ops on the card core, 04 NAME the project's name and ID: `project_name`, `on_name`) |
| `locks/` | the Pi-side parameter locks (51's lock ops), bound to the clock and the slots (`locks_bind.cpp`) |
| `store/` | the RAM store: card files held in RAM by path, so the control core never reads the card |
| `fmt/` | the card's plumbing: atomic writes (`write_atomic`, `AtomicWriter`), the boot sweep (`sweep_boot`), the control core's posted writes (`post_*`), `LineReader` |
| `msamp/cardq.*` | the card queue: the control core's card requests run on the card core (the Pi's core 0) |
| `lib/sha256.*` | one SHA-256 and one CRC-32 |
| `hooks.*` | where the core's SysEx for the host goes (`to_rytm`), and the restart another box's state folder needs |
| `platform.h`, `host/` | what a board supplies (files, the clock, the log, `sniprintf`) and the host's port for the tests; the Pi's is `pi/engine/platform_pi.cpp` |
| `machines.h` | the voice byte's machine flag that a device profile still names |

## The host link

The commands `core/generic` routes (the wire spec is `sim/protocol.py`):

| command | service |
|---|---|
| 4F | the device hello (`device`) |
| 0D | a folder for the Rytm's file browser (`browse`, answered with 0E) |
| 35 | the Rytm's hello: the link reset (the clock, the slots, the listeners) and the screen |
| 2C, 2D | the step clock and the transport (`clock`) |
| 30-3F | the remote screen (`screen`) |
| 51-53 | STORE (`kv`, through `gen::set_store`; `kv::link::attach_generic` wires it at boot) |

Every other command is counted (`gen::stats`: messages, dropped, the last dropped) and dropped. The project's name is 51's 04 NAME, not a command of its own.

## Rules

- Module state, one engine per program. `begin()` once at boot; `sysex`, `tick` and the idle steps on the control side (the Pi's core 3).
- The control side never calls the card: reads come from the RAM store, writes are posted (`fmt::post_*`) and run on the card core (`cardq`).
- Nothing here runs per sample except a filled processor slot (`proc`), which runs where the platform's OUT fill runs.
- A new command goes through `generic` with a test, and into `sim/protocol.py`.

## Tests

`make -C core test` builds every `tests/test_*.cpp` against the core's sources and ends `ALL CORE TESTS PASS`: the router (`test_generic`: drops, STORE, the clock, the hello and the screen, the pairing modal over an open page), the device profiles (`test_device`), the browse and the USB-A devices (`test_browse_devmidi`), the clock and the slots (`test_clock_proc`), the record store and its link (`test_kv`, `test_store_link`, `test_store_generic`), the locks (`test_locks`, `test_locks_bind`), the RAM store (`test_store`) and fmt (`test_fmt`). `tools/run_portable_tests.sh` runs these with the Pi engine's portable tests.
