# The Pi engine (`pi/engine`)

`pi/engine` is a bare-metal Circle kernel for the Raspberry Pi 4 (and the Pi 5: `docs/pi5.md`). It is the Sidecar's engine, built as generic services only: the link to a Rytm MKII (or another box with a device profile), storage on the card, the network and its web page, the MIDI bridge and the platform. No Rytm mod has a Pi-side counterpart of its own; a mod reaches the Pi through the generic commands below. The Rytm in OVERBRIDGE mode is its USB clock.

The engine was stripped to this on 2026-11-04. The voice engine it was before (the machines, FX, sequencer, sampler, recorder, pools, presets, bundles, library, plugins, the Teensy firmware and the local display) is in git history only: `3a8bf7d` is the last commit with all of it. The kernel is about 2.6 MB (it was 12.1 MB).

## What runs

- **The Rytm link and its transports.** The Overbridge host (`ob_usb.cpp`), the USB-C Sidecar gadget (`sidecar_gadget.cpp`: UAC2 and MIDI to the computer), the TCP emulator link (`link=tcp`, rytm-emu), the Rytm's USB MIDI, the 4F device hello and the device profiles (`core/device`, `docs/device-profiles.md`).
- **OS updates and tools.** OS updates through the Sidecar (`usbmidi=os:`), the Rytm MIDI bridge (TCP 7330), the USB-C reboot (`usbreboot=1`), the hub re-link fix.
- **Storage.** `core/fmt`'s atomic writes and boot sweep, the RAM store (`core/store`), the card queue (`core/msamp/cardq`), USB drives and NVMe (`stor_pi`, `docs/STORAGE.md`), the storage bench (`stor=bench`), the file service (`fsweb`, `/fs/*`, `/Samples` only), the SD file port 7310, the black box (`/.rytm/blackbox.log`).
- **The host link's generic commands** (`core/generic`, `docs/core.md`):
  - 4F, the device hello (`core/device`);
  - 0D, a folder of the card's `/Samples` for the Rytm's file browser (`core/browse`, answered with 0E; rytm-mods 0023);
  - 35, the Rytm's hello: the link reset and its listeners;
  - 2C / 2D, the Rytm's step clock and transport (`core/clock`);
  - 30-3F, the remote screen as a transport (`core/screen`: the connect splash, popups, the pairing code's modal, one client; no pages of its own);
  - 51-53, STORE (`core/kv`: records on the card, the Pi-side parameter locks, 04 NAME the project's name and ID).
  Every other command is counted and dropped (`/status` `generic`). The old 10 (the project's name) is one of them: the name comes from 51's 04 NAME.
- **The USB-A MIDI devices** (`core/devmidi`): up to four MIDI devices on the USB-A ports, their channel and realtime messages on to the Rytm (thru, on by default), to the processor slots and to listeners.
- **The audio return** (`audio_return.h`, `core/proc`): Overbridge OUT carries the computer's audio from the USB-C gadget plus whatever the four processor slots add. Every slot is empty by default, so OUT is a pass-through.
- **The audio route** (`core/route`, the audio route phase, decided 2026-11-04): up to 16 patch points, each adding one Rytm IN channel (a voice stem or AUDIO IN; never MAIN, which already holds the Pi's return) onto one OUT channel (MAIN, MAIN FX or a voice input) at a gain. A Rytm mod asks for a point with 51 `20 ROUTE` and gets its number back in `60 ROUTED` (refusals `61 REFUSED`); the gain is also lock target 4 (param = the point), so the lock service plays it per step. Gains ramp over 5 ms; a lock's gain is stamped with its tick's block and lands by that block's audio (`locklead=`, `/status` `route.timed` `late` `spread`); a voice loop is refused. Its underrun count is `/status` `aret.inMissing` (OUT frames whose IN frame had not come). It runs in the OUT fill before the slots; with no point open it costs one test a block. Console `route`, `/status` `route`. For now the slots run on core 0 in the OUT fill; moving them onto a core of their own comes with the first processor that needs it.
- **Shipped code** (`core/code`, design "Sidecar code shipping", 2026-11-07; phase 1, the loader): a Rytm client mod's native AArch64 blob (SCX, `core/code/scx.h`, packed by sidecar-core `tools/scxpack.py`) is offered by its SHA-256 with 51 `33 OFFER`, sent in `34 CHUNK`s from the offset `73 WANT` asks for, closed by `35 COMMIT` and answered `74 LOADED` (or `75 REFUSED`: 3 full, 5 bad, 9 abi, 10 hash mismatch, 11 import, 12 name clash, 13 crashed before); `36 DROP` unloads it once no slot holds it. A loaded blob is one more DSP library block, PLACEd by name by its offerer only. The card keeps blobs by hash in `/.rytm/code/` (16 MB, the oldest offered evicted first, never one offered this boot; `index.txt`), so after the first time an offer costs one message; `/.rytm/code/.armed` holds the hashes whose code has run for under a second, and a boot that finds it refuses those with 13 until a new version. The code arena (8 MB, 64 KB pages) is executable only where a blob sits: Circle maps the heap never-executable, and `code_mmu.cpp` changes the arena's own level-3 descriptors (writable to place, writable + executable for the constructors, then read-only + executable). Blobs import only the export table in `scx.h` (allocation in make(), C memory, float math). Console `code`, `/status` `code`. Not yet: the companion's side, the slot trig service, LFI sandboxing, a fault or CPU guard per slot (a fault in a blob reboots the Pi, as any fault does).
- **The platform.** The web page and `/status`, Wi-Fi, the pairing code, the console, the supply and temperature warnings, the CPU clock option, the idle sleep, the crash breadcrumb (`crumb.h`).

## Build and run

- Once: `pi/circle/build.sh`, which builds the Circle checkout with spike2's two xHCI patches. Then `pi/engine/build.sh`, which builds:
  - the `circle-engine` tree (`--multicore`, FatFs, the SD driver, the HTTP and mDNS patches);
  - the core's and the engine's portable sources (`hosted/`);
  - the kernel, `pi/engine/kernel8-rpi4.img`. It links Circle (GPLv3), so the built image is GPLv3 as a whole.
  Everything builds in Docker (image `rytm-circle`), or with a local aarch64 toolchain on the PATH.
- The Raspberry Pi 5: `RASPPI=5 pi/engine/build.sh` -> `pi/engine/kernel_2712.img` (its own Circle tree, no USB-C gadget), the SD card set by `pi/circle/make-sd-pi5.sh`: `docs/pi5.md`.
- For a Mac on a self-assigned link-local address: `PI_IP=169.254.61.60 PI_NETMASK=255.255.0.0 pi/engine/build.sh` (any free 169.254.x.y/16).
- After a change: `pi/engine/build.sh kernel && CIRCLE_TREE=circle-engine pi/circle/flash.sh ../engine 45`.
  - `flash.sh` builds the `.hex`, sends the reboot magic (`RYTM-REBOOT`, 921600 baud) to the running kernel, uploads through the serial bootloader and logs SECS seconds to `pi/circle/out/engine-<time>.log`.
  - The kernel lives in RAM only: the SD card is not touched, and the next reboot or power cycle boots the card's `engine.img` again (below).
  - A flasher killed while it writes can hang the Mac's FT232R adapter (the Pi then hears nothing, not even the reboot magic). `uv run --with pyusb python pi/circle/sdboot.py usbreset` clears it.
- The SD card's FAT32 partition is also the engine's card: `/Samples` (the content `/fs/*` and the browse serve), `/.rytm/` (the engine's state: Wi-Fi, pairing, the CPU clock, the black box, the store's records).
- Options go in `cmdline.txt` on the SD card, one line. The firmware hands it to every kernel, the SD's `engine.img` and a flashed RAM kernel alike. Without one, the build's defaults apply (`build.sh`'s `PI_IP`, `PI_NETMASK`, `EMUHOST`, `GADGET`). `pi/circle/sdboot.py cmdline` shows it, `sdboot.py cmdline "..."` writes it with the card in the Pi (the original kept as `cmdline.txt.bak`).
- The Rytm: OS 1.73 or 1.74 in OVERBRIDGE mode (USB CONFIG = OVERBRIDGE), on one of the Pi's USB-A ports. It can come and go while the kernel runs.

## Boot from the SD card (sdboot)

Power on: the engine runs from the card within about 11.5 s (reboot to its first stats line), with no Mac. A crash, a fatal stop or a watchdog reboot ends in the card's engine too, instead of a bootloader waiting for a reflash.

- The card's boot partition (2026-10-16): the firmware (`start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `armstub8-rpi4.bin`), `config.txt`, Circle's stock serial bootloader as `kernel8-rpi4.img` (unchanged), and:
  - `sdboot.img`: `pi/circle/sdboot.patch` on Circle 51.1.1's `tools/bootloader` (921600 baud), built by `pi/circle/build-sdboot.sh` into `pi/circle/out/sdboot.img`;
  - `engine.img`: a 32-byte header (`SDCRENG1`, the length, two running sums over the 32-bit words) and the raw `kernel8-rpi4.img`; `engine.img.prev`: the one before;
  - at the end of `config.txt`, between `# sidecar sdboot begin` and `# sidecar sdboot end`: `[all]`, `kernel=sdboot.img`, `initramfs engine.img 0x02000000`, `disable_splash=1` (no rainbow square), `boot_delay=0`. Everything above them is the user's, unchanged; `config.txt.bak` is the original.
- The boot: the firmware loads `sdboot.img` as the kernel and `engine.img` at 0x02000000. sdboot arms the BCM watchdog (15 s), checks the header and the sums, prints `sdboot: engine.img N bytes ok, starting in 1 s`, then `IHEX-F`, and waits 1 s for a byte.
  - No byte: the engine is copied to 0x80000 (30 ms) and started as the stock bootloader starts a flashed kernel; the watchdog is off again, and the engine starts its own.
  - A byte (flashy's records come within ms of `IHEX`): it is the stock serial bootloader, with one change: an upload that stalls for 5 s (a flasher killed, a cable pulled) prints `#ERR:timeout` and goes back to the 1 s window, so the card's engine starts after about 6 s. Before, the bootloader waited for a power cycle.
  - No valid `engine.img` (missing, truncated, bad sums): `sdboot: no engine.img (the serial bootloader only)`, and it waits for the flasher as the stock one does.
  - sdboot prints its own lines before `IHEX-F`: a byte that comes while it still prints overran the 32-byte RX FIFO, and flashy's fast mode failed (measured).
- Settings: `cmdline.txt` (above). There is no separate settings file.
- **The pairing code gates port 7310** (below, "Pairing code"): a TCP connection answers nothing but `PAIR <code>`, `AUTH <id> <secret>` and `BYE` until it is authenticated, because this is the port that replaces the firmware. `sdboot.py` does it for you: `SIDECAR_PAIR_CODE=12345678 uv run ... sdboot.py install` the first time per Pi, then it is remembered in `~/.config/sidecar/pair.json` and later runs never send the code again. The serial `:sdf` path is **not** gated (physical access, and the way back in when the network side is locked out).
- `pi/engine/sdfiles.h`: the running engine reads and writes the card's boot files. A file is written to `NAME.new`, read back and checked (CRC-32), then renamed into place; the old file is kept as `NAME.prev`, and `config.txt`/`cmdline.txt` keep their first version as `NAME.bak`. Two ways in, the same commands: TCP port 7310 (the network; `sdport=0` turns it off; built but not yet measured: no Ethernet link on 2026-10-16) or the serial console's `:sdf` (about 45 KB/s, base64 lines each acknowledged). `pi/circle/sdboot.py` (run as `uv run --with pyserial python pi/circle/sdboot.py ...`) tries TCP for 3 s, then the serial port; `SIDECAR_LINK=serial` goes straight to serial. One process on the serial port at a time.
- The firmware's tryboot: `TRYBOOT` sets the firmware's one-shot reboot flag (mailbox tag 0x00038064) and reboots; that boot reads `tryboot.txt` instead of `config.txt`, the next one `config.txt` again. `sdboot.py tryboot` uses it to boot a new sdboot from the card once, before it is made permanent.

| what | command (from the worktree's root) |
|---|---|
| dev flash (RAM only, as before) | `pi/engine/build.sh kernel && CIRCLE_TREE=circle-engine pi/circle/flash.sh ../engine 45` |
| install a build to the card | `uv run --with pyserial python pi/circle/sdboot.py install` (the running engine writes `pi/engine/kernel8-rpi4.img` as `engine.img`; about 35 s on serial) |
| the build before | `sdboot.py previous` (swaps `engine.img` and `engine.img.prev`) |
| a new sdboot | `pi/circle/build-sdboot.sh`, `sdboot.py bootloader` (as `sdboot-try.img`), `sdboot.py tryboot` (one boot with it), then `sdboot.py commit` |
| the card's files | `sdboot.py ls`, `sdboot.py get NAME [OUT]`, `sdboot.py scratch` (the write path's test: 64 KB written, read back, deleted) |
| off again (the stock bootloader at the next boot) | `sdboot.py disable` (removes the block), or `sdboot.py restore-config` (`config.txt.bak` back) |
| the serial adapter hangs | `uv run --with pyusb python pi/circle/sdboot.py usbreset` |

- Restore by hand (the card in a Mac): copy `config.txt.bak` over `config.txt`; `sdboot.img*` and `engine.img*` can then be deleted. `kernel8-rpi4.img` (the stock bootloader) was never changed.
- Risks: a broken `sdboot.img` made permanent leaves the Pi without a boot until the card is restored in a Mac (hence `tryboot` first); the engine's watchdog then still resets it, into the same sdboot. `config.txt` is replaced by two renames: a power cut between them (milliseconds) leaves no `config.txt`. Port 7310 writes the boot partition for a paired client. A build that dies at boot on the card (a fatal stop) reboots into itself, each time through the 1 s window, so `flash.sh` still gets in.

Measured 2026-10-16 (Pi 4 4GB, the card in the Pi, the Rytm attached, no Ethernet link):
- Scratch write, read back, delete: PASS. Installs: `engine.img` (1.56 MB) 35-38 s on serial, read back and checked on the Pi.
- tryboot: the firmware read `tryboot.txt`, `kernel=sdboot.img` under the last `[all]` replaced `[pi4]`'s `kernel8-rpi4.img`, and the engine came up; the next reboot read `config.txt`.
- Reboot to the engine (the card's config.txt path): `reboot:` at 0 s, sdboot's line and `IHEX-F` at 5.7 s, `starting the engine` at 6.7 s, the copy done at 6.8 s, Circle's first line at 9.9 s, the engine's banner at 10.1 s, its first stats line at 13.6 s. The stock bootloader's `IHEX-F` came at 5.2-5.3 s; sdboot's check adds about 0.4 s.
- The kernel constructor's `m_ActLED.Blink (3)` (from the skeleton) held the boot 2.1 s before Circle's first line; without it: the banner at 8.0 s, the first stats line at 11.5 s.
- `disable_splash=1` and `boot_delay=0`: no change in the timing (sdboot's line at 5.7-5.8 s with and without; the firmware's own time). What is left: the firmware 5.7 s, sdboot's window 1 s, Circle before its first line 1.0 s, the engine's init 3.5 s (then with the voice engine's boot bench, gone since 2026-11-04).
- `:wdtest` (core 0 hangs): the watchdog's reboot at 21 s, the card's engine at 25.7 s, no Mac.
- flash.sh (unchanged: flashy and its reboot magic) through sdboot: upload 22.9 s, the go 29 s after the start, the RAM kernel runs; the card's files unchanged.
- An upload killed after 12 s: `#ERR:timeout` 5 s later, the card's engine 1 s after that.
- Not measured: a cold power cycle (the user's one-step test: power the Pi off and on; the engine's banner and stats lines should come on the serial port, and `sidecar.local` once the Ethernet link is up).


## Cores

| core | does |
|---|---|
| 0 | the xHCI interrupt (Overbridge IN/OUT, OUT paced by IN, the OUT fill with the processor slots), USB plug-and-play, the Rytm's USB MIDI and the USB-A MIDI devices, the USB-C gadget, the network (Circle's scheduler: the web page, mDNS, Wi-Fi, ports 7310 and 7330, the TCP link), the console, the stats a second, the card queue (`cardq`: 51 STORE's ops, the browse's reads, the atomic writes), USB drives, the black box, the watchdog, the clean stop |
| 1, 2 | idle (`wfe`): nothing renders. Kept for a processor that needs a core of its own |
| 3 | the control loop (`cores.cpp`): the Rytm's MIDI bytes into the host link's router (`bridge.cpp` -> `core/generic`), the USB-A MIDI devices' messages (`core/devmidi`), the console's queued commands, the generic tick (the remote screen), the pairing code's popup, the card's idle saves |

**The rule for core 3: it never calls FatFs.** Every card read or write is posted to core 0's card queue (`fmt::post_*`, `cardq`), and the RAM store (`core/store`) holds what core 3 reads. A card call on core 3 waits behind core 0's card work and stalls the router.

## Options (`cmdline.txt`)

| option | default | |
|---|---|---|
| `fifo=N` | per link: usb 96, tcp 288 | the links' FIFO target in frames (24..1024), for every link when given; kept for `/status` and the TCP link's pacing |
| `cpumhz=1500\|1200\|1000` | 1500 | the CPU clock option ("CPU clock" below); `/.rytm/cpu.txt` has it too, cmdline wins |
| `idlewfe=0` | 1 | turns off the idle sleep ("Idle sleep" below); `:idlewfe=0` / `:idlewfe=1` on the console do it live |
| `tohost=0\|1` | 1 | the engine's host-link answers (browse's 0E, STORE's 51, the screen's 36) go to the Rytm's USB MIDI OUT; 0 drops them (counted) |
| `cardq=0\|1` | 1 | the card queue on core 0 (`core/msamp/cardq.h`); 0: core 3 runs the card requests itself (a diagnostic) |
| `link=usb\|tcp` | usb | the engine's host at boot (below, "rytm-emu over Ethernet") |
| `ip=` `netmask=` `emuhost=IP:PORT` | the build's | the static address and the emulator's link |
| `web=0\|1` | 1 | the web page at http://sidecar.local; 0: no HTTP daemon and no mDNS (the network stays up for `link=tcp`) |
| `sdport=0\|1` | 1 | the SD file port 7310 |
| `paircode=0\|<8 digits>` | (absent: a random code at every boot) | the pairing code that gates the network ("Pairing code" below). `paircode=0` turns the gate **off** (a development card only, and the engine says so loudly); `paircode=13572468` keeps the gate with a fixed code |
| `gadget=sidecar\|midi\|0` | the build's | the USB-C gadget: the Sidecar (UAC2 + MIDI), P1's MIDI echo, or none |
| `usbmidi=os:off\|os:emu\|os:on` | os:off | OS updates through the USB-C MIDI port ("OS upgrades" below) |
| `usbreboot=1` | 0 | the computer's `F0 7D 52 54 7E "REBOOT" F7` on the USB-C MIDI port reboots the Pi |
| `usb=mirror` | sidecar | Pi 4, own use only: the USB-C side shows the computer the attached box itself (its descriptors, audio, async channel, MIDI and requests relayed; `docs/usb-mirror.md`) instead of the Sidecar device, which comes back when the box has been gone `mirrorhold=S` seconds (10); Overbridge shows the unit's name with `mirrorname=` (default " (SC)"). Never a public default |
| `uac2test=1` `midifake=1` | 0 | the gadget's test tones and fake MIDI (`sidecar_gadget.h`) |
| `uac2lr=0\|1` | 1 | Windows' shared mode: the Sidecar device's channels 1-2 marked front left / right (bmChannelConfig 0x3), the IN terminal a line connector, and IN alt 2 = MAIN L/R only (2 channels; alt 1 stays all 12). 0: the old layout (all raw, a microphone terminal, no alt 2). T191/T194 [hw]: with the old layout and with the mask alone, shared mode gave L == R |
| `nvme=0` | 1 | skips the Pi 5's NVMe SSD (`docs/STORAGE.md`) |
| `wifi=` | (wifi.txt) | Wi-Fi's mode (`wifi.h`, "Wi-Fi settings" below) |
| `capch=N` | markers | the 4th channel of the `w` capture: IN channel N (0..11), or the MIDI note-on markers (12 or more) |
| `testsecs=S` `testcard=1` | 0 | the Pi 5's engine test mode (`docs/pi5.md`) |
| `oburbs=1..16` `obblocks=1..8` `obout=paced\|free\|off` `oboutdelay=N` `obctr=zero\|in` | 8, 1, paced, 0, zero | spike2's Overbridge host tuning, unchanged (`docs/pi-circle-spike.md`) |
| `locklead=0..4800` | 144 | frames a lock-driven route gain lands after the earliest tick of its block (`core/route/route.h`, stamped gains; T118 [hw]) |

## Console (921600 baud, the flasher's serial cable)

Keys:
- `w`: a 10 s capture of IN MAIN L/R, OUT MAIN L as sent, and the 4th channel (`capch=`), dumped as base64; `pi/circle/capture.py` writes the WAV. While it dumps, the core's log lines are held back and counted (another core's bytes inside a dump line garble it).
- `s`: the stats now.

Commands are `:<command>` and a newline, at most 47 characters. Core 0 runs its own:

| command | |
|---|---|
| `version` | the Sidecar's version and commit (`docs/versioning.md`) |
| `link=usb\|tcp\|tcp:IP:PORT` | the engine's host at run time |
| `wdtest` | watchdog test: core 0's loop hangs and the BCM watchdog reboots the Pi (no clean stop) |
| `sdf` | the SD file commands on the serial port (`sdfiles.h`, `sdboot.py`) |
| `cpumhz=1500\|1200\|1000`, `cpumhz` | the CPU clock option, saved to `/.rytm/cpu.txt`; alone: the state |
| `pair`, `pair=show`, `pair=revoke` | the pairing code's state (never the code), the code on the Rytm's screen again, every paired device dropped |
| `midibridge`, `midibridge=on\|off\|os:on\|os:off` | the Rytm MIDI bridge (below) |
| `wifi`, `wifi=...` | Wi-Fi (`wifi.h`) |
| `stor`, `stor=bench[:VOL]` | the volumes and the storage bench (`docs/STORAGE.md`) |
| the Sidecar gadget's | `mac=on\|off`, `uac2test=on`, `midifake=on`, `usbmidi=...` (`sidecar_gadget.h`) |

The control core (core 3) runs the rest:

| command | |
|---|---|
| `gen` | the router: the project's name, messages, dropped (and the last dropped command), hellos |
| `clock` | the Rytm's clock: transport, pattern, BPM, ticks |
| `proc` | the processor slots and their counts |
| `rui`, `rui=on\|off` | the remote screen; `rui=on\|off` asks the Rytm to open / close the page |
| `usba`, `usba=thru\|off` | the USB-A MIDI devices and their thru to the Rytm |
| `browse` | the card browse's counts |
| `fifo=N` | the FIFO target (as `fifo=`) |
| `ctlwait=poll\|wfe` | the control loop spins instead of `wfe` (an A/B for its gaps) |
| `idlewfe=0\|1` | the idle sleep (as `idlewfe=`) |

An unknown command logs `unknown command "..."`. A long line is dropped whole.

### The stats (a line a second)

- `engine: generic | cpu c3 X % | pass max P us | midi M B (lost L) sysex S notes N | to-host sent B B M msgs fail F dropped D | T C`
  - `cpu c3` is the control core's busy share, `pass max` its longest working pass.
  - `midi` the Rytm's USB MIDI bytes this second, `lost` the bytes a full input queue dropped; `sysex` the messages so far; `notes` the Rytm's note-ons this second (counted only).
  - `to-host sent`: the engine's bytes and messages to the Rytm's USB MIDI; `fail` the sends that failed (one turns sending off until umidi1 is found again); `dropped` the bytes not sent (no umidi1, `tohost=0`, after a failure, a full queue).
  - The last field is the SoC temperature.
- `thermal: ...` lines come once per crossing of the temperature warning (below).
- With `link=tcp`, the `tcp:` line (every 10 s). With the Rytm, spike3's `rytmob:` line follows: IN/OUT blocks a second, `gap`, `lost`.
- Every 10 s: `idle: asleep ...` ("Idle sleep" below).

## The RAM store and the card's writes

`core/store` holds card files in RAM as bytes keyed by path (`store::get` / `put` / `mark`); core 3 reads it, and the card queue fills it and writes it back. Writes are atomic (`fmt::write_atomic`: a `.tmp`, then renames, the old file kept as `.prev`; `fmt::sweep_boot` at boot restores a file a power cut left between the renames) and core 3 only posts them (`fmt::post_begin / post_end`, `fmt::post_queue`); the card core runs them (`cardq::kAtomicReq`). A write the card core has not taken after 200 ms stays posted and is counted in `held`.

**`/status` `"store"`**: `{"files":N,"bytes":M,"ready":true,"unwritten":K,"oldestMs":T,"held":H,"heapFree":F}`.
- `files`, `bytes`: what the store holds; `ready`: the last fill is complete.
- `unwritten`: writes posted and not yet on the card (the card queue's requests plus fmt's queued and running writes, each counted once); 0 when the card is caught up. `oldestMs`: the age of the oldest of those. A card that is stuck shows as a climbing `oldestMs` with `unwritten` above 0.
- `held`: posted writes the card core had not taken after 200 ms, since boot.
- `heapFree`: bytes of the low heap never yet handed out (`CMemorySystem::GetHeapFreeSpace(HEAP_LOW)`). Circle halts with "Out of memory" once an allocation would reach into the heap's last 256 KB, so this is the room left before that.

51 STORE's records (`core/kv`) live in the state folder and go through the same card queue: `docs/STORAGE.md`, `core/kv/kv.h`.

## Tests

- `make -C core test`: the core's services on the host (generic, screen, clock, proc, device, browse, devmidi, kv, locks, route, dsp, code, store, fmt).
- `make -C core test-aarch64 SCX_TGAIN=<tgain.scx>`: the same suite built for AArch64 and run under `qemu-aarch64` user mode (Linux: `g++-aarch64-linux-gnu`, `qemu-user`), where `test_code` also loads and runs the TGAIN test blob (sidecar-core `make -C blobs`) through the whole path, with real page protection (mprotect).
- `make -C pi/engine/tests` (on the Mac or Linux; `CIRCLE_BUILD=` for the FatFs tests): the engine's portable parts - the router and the audio return (`test_generic_pi`), the Overbridge host on a fake Circle (`test_ob_usb`, `test_ob_block`), the gadget's HID and wake paths, the file service (`test_fsweb`), the pairing code, Wi-Fi's settings, the MIDI bridge and its merge, OS update parsing, the supply, the temperature warning, the CPU clock, the idle sleep, the storage layer and its bench, the black box, the breadcrumb, the emulator link's protocol, the build's version stamp, `tools/rytm_bridge.py`.
- `node --test pi/engine/tests/page_*.test.mjs`: the web page against `pi/engine/tests/mock_web.py` (the tiles, pairing, Wi-Fi, the network rules, storage).
- `tools/run_portable_tests.sh`: all of the above in one run (`CXX=clang++ CC=clang` too).
- `python3 pi/engine/tests/test_sdboot.py`: `pi/circle/sdboot.py` against stand-in engines on the SD file port.
- On the unit: `pi/engine/tests/sidecar_rytm_test.py` (the Rytm's audio and MIDI reach the computer through the Sidecar), `sidecar_midi_test.py` and `gadget_midi_test.py` (the USB-C MIDI port), and the tools in `docs/HARDWARE-TESTING.md`.

## rytm-emu over Ethernet (`link=tcp`)

The Pi can be the Overbridge host of rytm-emu instead of a real Rytm. Nothing is flashed to the Rytm. In tcp mode a still-attached real Rytm gets silence and its MIDI is dropped.

- A cable from the computer's Ethernet port straight to the Pi's. There is no DHCP server, so both ends use static addresses: the Mac at `192.168.77.1/24` (System Settings > Network > the Ethernet port > Details > TCP/IP > Configure IPv4: Manually), the Pi at its `ip=` (default 192.168.77.2), or both on link-local 169.254.x.y/16.
- `link=tcp` in `cmdline.txt`, or `:link=tcp` / `:link=tcp:IP:PORT` / `:link=usb` on the console at run time. `emuhost=` (default 192.168.77.1:7301) is the emulator's link. If the network did not initialize, `link=tcp` is refused and the engine stays on usb.
- The TCP link's sending is bounded per loop pass: at most 8 messages or 5 ms of sending, the rest at the next pass, inside the 15 s watchdog.
- The protocol: rytm-emu's Overbridge link (`rytm-emu/src/oblink.rs`, `--ob-link 0.0.0.0:7301`). A host that sends the hello `H 1 flags` gets bit 0 the USB MIDI pair `M` (u16 LE length + bytes) both ways, bit 1 no `R` frames, bit 2 compact frames `a` paced by the emulator's `N` requests. The Pi's side is `pi/engine/emu_proto.*` (`tests/test_emu_proto.cpp`) and `pi/engine/tcp_link.*`.
- The TCP transport's stats line: `connected to IP:PORT, hello yes (granted 7), frames N/s in R requests, wanted W, silence S, midi in A B (lost L) out B B dropped D, connects C, ...`.

## Web page (http://sidecar.local)

The Pi serves a small page over Ethernet or Wi-Fi: status and health, the live log and a console line, pairing, Wi-Fi, the MIDI bridge and the card's files. On by default; `web=0` in cmdline.txt turns it off. Code: `pi/engine/web.{h,cpp}`, the page `pi/engine/www/index.html` (built into the kernel by `build.sh` as `hosted/web_page.h`).

- **Everything is behind the pairing code** except the page itself, `/health`, `/status`, `/log` and the pairing endpoints: see "Pairing code" below.
- Name: the hostname is `sidecar`. Circle's mDNS daemon answers `sidecar.local` and publishes `Sidecar | Expander` (`_http._tcp`, port 80); a second Sidecar becomes `sidecar-1.local`. `pi/circle/circle-mdns.patch` makes Circle's daemon answer the way macOS's resolver expects (without it, lookups waited 5 s for an IPv6 answer).
- Endpoints: `GET /` the page; `GET /health` (alive, version, uptime, temperature, streaming, the Rytm's MIDI, the supply, underruns, the gate); `GET /status` JSON (below); `GET /log?since=N` the console log from byte N (first line `START END`, then the bytes) from a 64 KB ring next to the serial port (`CLogTee`); `POST /cmd` with `c=<command>`: the console's own parser, logged as `web: command :...` (`wdtest`, `wifi=` from a URL, `usbmidi=` and `midibridge=` are refused); `/wifi/*` (below); `/pair*` (below); `/midibridge*` (below); `/fs/*` (below); `GET /blackbox?off=N` the black box's file (`blackbox_pi.h`).
- `/status`: `engine`, `ip`, `link`, `gadget`, `sidecar`, `mac`, `streaming`, `rytmMidi`, `temp`, `tempWarn`, `clockLow`, `fifoTarget`, `stall` (core 3's longest gap and its sections), `usbmidi`, `slowSx` (the SysEx that took core 3 over 1 ms), `mb` (core 0's slow firmware calls), `cpu`, `idle`, `generic` (`project`, `projectId`, messages, dropped, `foreign`: other makers' SysEx the Rytm sent, not counted as dropped), `clock`, `proc`, `route` (open points as `[point, owner, src, dst, gain]`, refusals, stamped gains `timed` `late` `resyncs` `lead` `spread`), `screen`, `devmidi`, `browse`, `store`, `aret` (the audio return's IN frames dropped and missing), the version, `power`, `wifi`, `blackbox`, the volumes, `core0`, `pair`, `midibridge`.
- The page: a Status tab (tiles: the control core's CPU, underruns, Overbridge, the Rytm, the USB-C gadget, temperature, the supply, the page's own cost; then Wi-Fi, the MIDI bridge and pairing), a Files tab (`/Samples` through `/fs/*`), a Log & console tab, and About. It polls `/status` every second and `/log` every 0.8 s.
- `/fs/*` (`fsweb.cpp`): `ls`, `get`, `put` (to `<path>.part` with its CRC-32, then renamed), `del`, `mkdir`, `mv`, under `/Samples` only (`fsweb::reachable`, deny by default: `/.rytm`, the boot files and anything else are unreachable).
- Cores: Circle's scheduler and network stack are core 0's, so the HTTP and mDNS daemons are tasks there, run while core 0's loop sleeps its 1 ms. The Overbridge stream and the USB-C gadget run in core 0's interrupts, which the tasks never mask.

### Wi-Fi without a cable: sidecar-wifi.txt (2026-10-19)

A visible first-setup file at the card root (FAT root, visible in the Mac Finder), for a Pi with no cable and no way to open the page:

```
ssid=MyNetwork
psk=MyPassword
```

Optional: `band=2g|5g|any` (default 2g), `open=1` (an open network, no psk), `password=` as an alias of `psk=`, `#` comments; CRLF, a UTF-8 BOM, spaces around `=` and quotes around values are tolerated. At boot, before Wi-Fi starts (`CWifi` constructor, `wificonf::import_card`), the file is checked like the page's join (`check_join`: SSID 1-32 bytes, passphrase 8-63 printable or 64 hex, open takes no psk) and imported exactly like "Connect and save": `/.rytm/wifi.txt` by the atomic save with `on=1` and the derived 64-hex key (never the passphrase), the previous file kept as `wifi.txt.prev`, `wifi.try` "trying", the 90 s fallback. The file is then deleted, also when it was rejected; if the delete fails its contents are overwritten and it is renamed to `sidecar-wifi.used`. A rejection writes `sidecar-wifi-error.txt` (a short reason, never the SSID or password); a success removes an old one. One log line, never the SSID or password. Tests: `tests/test_wifi_conf.cpp` `test_import`.

### Wi-Fi settings (2026-10-18)

The Status tab's **Wi-Fi** section: on/off, a network list from a scan (signal bars, a lock for secured networks, the band), "Other network…" with a name field, a password field (show/hide, never pre-filled), "open network", **Connect and save**, **Forget network**, and the current connection (Ethernet or Wi-Fi, the address; the stored network's name only here). Code: `pi/engine/wifi.{h,cpp}`, the portable part `pi/engine/wifi_conf.{h,cpp}`, the endpoints in `web.cpp`. Research and the bring-up: `docs/research/pi-wifi.md`. Wi-Fi stays off in the release (no `/.rytm/wifi.txt`); turning it on here works over Ethernet.

- Endpoints: `GET /wifi/status` (the only answer with an SSID in it: mode, chip, on, net `ethernet|wifi`, ip, the stored SSID, the last scan's networks `{ssid,rssi,ch,band,secured}` (one per SSID, the strongest, sorted by signal, hidden ones left out, at most 24), the join's state `none|restarting|trying|up|failed` with its reason and seconds left); `GET /wifi/scan` starts a scan (the chip's escan on core 0's Wi-Fi task, 100 ms sleeps, stopped after 5 s; never on the audio cores; refused while Wi-Fi is off, while a join is tried, and on Wi-Fi while it is not associated); `POST /wifi/join` with `ssid=`, `psk=`, `open=1` in the form body (never the URL); `POST /wifi/forget`; `POST /wifi/power` with `on=0|1`. Answers: `{"ok":true}` or `{"ok":false,"code":N,"error":"..."}`. `/status`'s `wifi` member has no names any more (`stored` true/false, `networks` a count, `join` the state).
- Checks (`wifi_conf.cpp` `check_join`, the page the same): SSID 1-32 bytes without control characters; a password of 8-63 printable characters or 64 hex digits; an open network only with the explicit flag and no password. The error texts never quote the input.
- Storage: wifi.txt keeps its format (one key added: `open=1`), holds the 64-hex key (PBKDF2 of the passphrase, made on the Pi), never the passphrase, and is replaced atomically: `wifi.txt.part` written, the current file renamed to `wifi.txt.prev` (no file before: a `.prev` saying `on=0`), then `.part` renamed in place; a boot after a cut between the renames finishes it.
- One network at a time (Circle's single network stack, `wifi.h`): a join cannot switch the running Pi from Ethernet to Wi-Fi, and Circle's DHCP client keeps its lease when the Wi-Fi network changes, so **Connect and save restarts the Pi** (2 s after the answer) and the new network is tried at that boot. `/.rytm/wifi.try` says `state=trying`. Not up (associated and a DHCP lease) within `CWifi::TrialSecs` (90 s; the supplicant restarted once at 40 s, the watchdog's cure for a slow first association, seen 27 s to 3 min): `wifi.txt.prev` goes back, `wifi.try` says `state=failed` with `reason=wrong_password` (wpa_supplicant's WRONG_KEY lines, read from the logger's event queue), `no_dhcp` (associated, no lease), `not_found` (never associated) or `not_started` (the chip), and the Pi restarts on the previous setting: Ethernet when Wi-Fi was off, so a typo cannot lock it out. Ethernet is not used while the Pi is on Wi-Fi; the page says so, follows the Pi at `sidecar.local` if the computer is on the new network, and shows the failure after the revert. The console's `wifi=join:` and `wifi=on` with a stored network write the same trial, tried at the next boot.
- Forget: the network (and `wifi.txt.prev`, which holds a key too) removed, `on=0`; on Wi-Fi, Ethernet from the next boot.
- Credentials: the password goes only in the POST body; the server wipes its copies (the form buffer, the decoded value) after use; it is never logged, echoed or in `/status`. The *passphrase* is never stored either — what `/.rytm/wifi.txt` keeps is the derived 64-hex key. That file is a credential at rest: until 0.3.0 `GET /fs/get?path=/.rytm/wifi.txt` served it to anyone on the network, which is now fixed twice over ("Pairing code" below: the gate, and `/fs/*` reaching content only). It is still readable by whoever holds the card — see "Threat model" there. SSIDs are not logged either: the scan line gives a count, and the log tee (`CLogTee`, serial and the page's log) blanks the name in any `SSID '...'`/`ssid="..."` line (wpa_supplicant's own). `/cmd` refuses `wifi=` commands from a URL (only a POST body) and logs `wifi=join:...` redacted as before.
- CSRF: join, forget and power are accepted only from the page itself (`wifi_conf.cpp` `same_origin`): the Host header must be one of the Pi's names (`<hostname>.local`, its IP; this also stops DNS rebinding — but only on the endpoints that call `FromPage`, which before 0.3.0 did not include `/fs/*`) and the Origin header (else the Referer) must be `http://<that host>`; a request with neither is refused. `/cmd` refuses a request whose Origin/Referer is another site's (tools without those headers still work). The headers come from `pi/circle/circle-http-origin.patch` (Circle's HTTP daemon keeps Host, Origin, Referer; applied by `build.sh`). On top of that, since 0.3.0 the Wi-Fi POSTs also need the **pairing code** (the next section): the same-origin check stops another site's page, the pairing code stops the person on the network. **Remaining risk:** the transport is plain HTTP on a LAN, so the Wi-Fi password crosses it once in the clear when it is sent, and anyone who can watch the network can take a paired client's session token as it goes past and use it. The gate is a lock on the door, not encryption: see "Pairing code" > "Threat model".
- Tests: `make -C pi/engine/tests` (`test_wifi_conf`: the checks, wifi.txt round trip, the atomic save and restore with failing writes and renames, recovery, the trial marker, same-origin, SSID redaction, the scan list); `node --test pi/engine/tests/page_wifi.test.mjs` (the page's check, and the flow against `mock_web.py`: the refusals, a scan, a wrong password failing and reverting, a good join, forget; nothing echoes the password or logs the SSID). `mock_web.py` serves fake networks; the password `wrong-pass` fails, an SSID not in the scan is not found, `MOCK_TRIAL=` sets the trial's seconds (default 6).
- Not yet on hardware: the scan and a join (the user's network).

### Pairing code (2026-10-25)

Before 0.3.0 everything the Pi served was open to anyone on the LAN. The only check was the same-origin one above, which stops a hostile web page but not a person on the network: `GET /fs/get?path=/.rytm/wifi.txt` returned the Wi-Fi key, `GET /fs/ls?path=/` the whole card, and TCP 7310 **replaces the firmware** with no credential at all (confirmed against a running unit, 2026-10-25). The pairing code closes that.

**The scheme.** The device makes an **8-digit code** at every boot (`paircode::Gate::init`: one digit per random byte from the SoC's hardware generator mixed with the timer, bytes of 250 and above drawn again so every code is equally likely). It is shown on the **Rytm's screen** (a host-drawn popup, `SIDECAR PAIR CODE` + `1234 5678`, 30 s; always on the SIDECAR page's PAIR line, UP + T10, and in SIDECAR MENU > WEB UI), and written to `/sidecar-paircode.txt` at the card's root ("No screen" below). It is in no log line, no `/status`, no `/log` and no answer of any endpoint.

**It does not persist, and does not need to.** A new code is drawn at every boot and shown on the screens; nothing stores it. A device that has paired once does not need it again — it resumes with its own secret — so there is nothing to keep. The one exception is a unit with no display at all: see "No display".

A client enters the code once, in the body of `POST /pair`. It gets a **session token** (128 bits, this run only) and, if it asked to be remembered, a **client id** (32 bits, not a secret) and a **client secret** (128 bits), of which the card keeps only SHA-256. Every later request carries the token in the **`X-Sidecar-Auth` header** — a header another site's page cannot set, and one a browser does not keep in its history. After a reboot the client trades its id and secret for a new token (`POST /pair/resume`), so the owner does not retype the code.

Over the wire: the code once, the secret once per run, the token on every request that is not in the open set below.

#### The code on the Rytm's screen

A **small modal in the middle of the 128x64 panel**, drawn over whatever is showing — a Sidecar page, the connect splash, the SRC view. A status popup defers to a page that is open, which is right for a status popup and wrong for the one credential needed to use the device at all; ``core::screen::popup`'s `over` flag is that distinction. `caps36` and `legacy` still refuse: an older mod genuinely cannot draw it, so it fails, says so in the log, and points at the touchscreen and `cmdline.txt paircode=<8 digits>`.

**It overlays, it does not replace.** The page is drawn into `work` as usual and the box goes on top, so the diff sends only the box's tiles. Coming back uses the mechanism the remote UI already has for a page change — `view.invalidate()`, and the page redraws itself from the model — so no page state lives in pixels and none can be lost. The Rytm draws each text item itself, in a box it clears, so any of the page's items under the modal are blanked while it is up and the page's own redraw puts them back. `test_ui` asserts the restored frame is **byte for byte** the one from before, slots included, and writes `build/rui_pages/pair_modal_{0_page,1_over,2_back}.png` to be looked at.

Geometry: an 80x32 box at (24,16) on the 128x64 panel, double-bordered, filled so the page does not show through, two centred lines in `kRSmall` — "SIDECAR PAIR CODE" and "1234 5678", nine characters each, inside 76 px of usable width. Text slots 62/63, above anything a page uses.

**It does not disturb playback.** It draws in the remote UI's existing tick on the control core, in the same place a page frame is already drawn: no new work on an audio core, no allocation, and about 40 tiles over MIDI when it appears and again when it goes — the cost of one page change.

**When it appears** (these three are defaults chosen here, not the user's requirement, and are meant to be easy to change):

| trigger | |
|---|---|
| boot | once, with the new code |
| the Rytm connects | each new hello (a cable, a Rytm restart) while no device is paired yet, at most once a minute (`paircode::HelloShow`) |
| `pair=show` | the explicit route, from the console |
| the page's dialog opens for real | `POST /pair/show`, sent by the page once it knows it is unpaired (no saved pairing, or a stale one), and by its **Show it on the Rytm** button — whoever asks, **at most once every 10 s** (`paircode::ShowGap`, fixed). The answer says `{"shown":true}` or the `wait` left, and the button says "Shown on the Rytm" or "Wait N s" |

**Not on a page load (fixed 2026-10-29).** It used to be `GET /`, but a page load never carries the token (it is a header the page's own `fetch` sets), so every reload of a *paired* owner's tab flashed the code over the Rytm. The page now asks for it itself, after `/pair/status` and a resume have shown that it really is unpaired; a session lost to a reboot resumes first and shows nothing.

**Kept until the Rytm is there (fixed 2026-10-29).** The boot's request used to be given up after ten refused tries, and the Rytm's first hello comes well after the engine's boot, so the modal never appeared; and a hello arriving while it was owed or up dropped it. Now a refusal before the Rytm's first hello is "no Rytm yet" and is not counted (`bridge::rytm_greeted`), and a hello keeps an `over` popup when the new link can draw it (`test_ui`: "survives a hello").

The trigger is deliberately **not** `/pair/status`: the dialog polls that every 5 s while unpaired, so it would redraw the modal over the Rytm for as long as anyone sat on the pairing screen. `GET /` happens once per load. It is rate-limited because `/` is open to anyone on the network: without that, someone could put a box over the Rytm's screen at will while the user is playing. Nothing leaks either way — the code is already permanently on the touchscreen and neither screen is visible from the network — so this is a disruption limit, not a disclosure one. A burst of reloads collapses into one modal (`Show()` counts requests, not modals).

**The gap is a fixed 10 s (2026-10-29; review L1 accepted).** It used to start at 30 s and double up to 10 min, refusing silently, so on a Sidecar whose only screen is the Rytm's the button stopped working after a few presses. Anyone on the network can now put the box up every 10 s. That is accepted: it only shows the code to whoever stands at the Rytm, which is physical access (outside the threat model), and any key on the Rytm takes it off.

**When it goes:** after **30 s**, at a key on the Rytm (with the page closed the Rytm's own splash rule; over an open page the key is eaten), or at once when any device pairs. The case is someone reading eight digits off a small screen and typing them into a browser on another device — too short is as bad as not showing it. **Fixed 2026-10-29:** it used to go after about a second — the Rytm ends a `36` splash after 1 s without a host message and the popup sent none, and its 12 s asked for 244 ticks in a 7-bit byte (116 went). It now sends a `3C` every 0.5 s and the `36` again (at most 127 ticks) before the last one runs out (`test_ui`: "held 30 s"); a key or a turn on the Rytm ends it there (the Rytm's splash rule) and, as the Rytm reports it, here too, so no `36` brings it back. It never traps the screen: the code is valid for the whole boot and `pair=show` brings it back. Pairing clears it so it does not linger once the job is done.

Not verified here: whether the `36` frame wakes a Rytm whose screen has gone to sleep. The modal pre-empts the Sidecar's own connect splash, but the Rytm's own sleep is its side of the link.

#### What is open

`web.cpp`'s `Open()` is an **allowlist**: anything not named in it needs the code, so an endpoint added later is gated unless someone deliberately opens it. (It was a gate*list* first, which is how `/fs/get` ended up in neither list.)

| open | why |
|---|---|
| `/`, `/index.html` | the page itself, so someone can load `sidecar.local` and see the dialog. It needs no other file: the CSS, the JS and the wordmark font are all built into the page. |
| `/health` | alive, the build and commit, uptime, temperature, `streaming`, `rytmMidi`, the supply's flags and warning, underruns, and whether the gate is on — enough to tell whether a unit is working, and nothing of what is on it. It is a function of its own (`CKernel::WebHealth`), not a filtered `/status`, so a field added to `/status` cannot appear in it by accident. |
| `/status` | open by the user's decision (2026-10-26: "open as specified" — seeing whether a device works on a shared network matters more than hiding its settings). It carries names and settings (host, address, the project's name, the devices) but **no credential**: no pairing code or token and no Wi-Fi key (checked against a live answer). Anything secret added to `WebStatus` would be readable by anyone on the network. |
| `/log` | deliberate, and unredacted. It is this project's whole diagnostic basis — the MIDI-gate verification, the touch-failure timeline and the underrun hunt all came out of it — and a tester must be able to send a URL. Its steady state is counters; name-bearing lines (`project "..."`, `/Samples/...`, recording names) roll out of the 64 KB ring in about 25 s. This is a judged trade, not an oversight. |
| `POST /pair`, `/pair/status`, `/pair/resume`, `POST /pair/show` | pairing has to work before you are paired. `/pair` is rate-limited (below); `/pair/resume` takes a 128-bit secret and is open because without it "remembered across reboots" cannot work at all; `/pair/show` puts the code on the Rytm's screen (never in the answer: `shown` and the `wait` left), at most once every 10 s. |

**Everything else is gated**, including all of `/fs/*` (reads included), `/wifi/*`, `/midibridge*`, `/blackbox`, `/cmd` and TCP 7310. A refusal is HTTP 200 with `401 not paired …` in the body (Circle's error pages carry no message) plus an `X-Sidecar-Pair: required` header the page watches for; the body is JSON or `"<code> <message>"` to match what that endpoint's client reads (`WantsText`).

#### What the file API can reach

Separately from the gate, and underneath it: **`/fs/*` is content, TCP 7310 is firmware, `/.rytm/` is neither and is reachable through neither.**

`fsweb::reachable()` decides whether a path exists as far as `/fs/*` is concerned, for **every** operation — `ls`, `get`, `put`, `del`, `mkdir` and both ends of a `mv` — and denies by default. The one content folder is `/Samples` (`fsweb.cpp` `kContent`; the voice engine's other folders went with it, 2026-11-04). The card's root lists it and nothing else; a USB drive is not served. Anything outside it answers `404 no such path`.

This is worth having even with the gate in front of it, because it holds against an **authenticated** attacker — a tester who paired, or anyone with a stolen token — and because it closes two specific holes: `/fs/get` served `/.rytm/wifi.txt` (the Wi-Fi key) and, on a headless unit, `/sidecar-paircode.txt`; and `read_only()`'s "below the root: only overlays/ is the firmware's" meant a **write** could land on `/.rytm/pair.txt`, where putting a chosen device hash is a way past the pairing code itself. `sdboot.py` on 7310 is where `cmdline.txt` and `engine.img` are legitimately written, and it is authenticated.zip` or a stick's `SIDECAR/` folder (`bundle_path_ok`, `stick_path_ok`), so it needed no change.

`/fs/*` also gained the same-origin layer it never had: reads strict (`same_origin`; the page sends a `Referer` on its own GETs, a DNS-rebound page's `Host` is not ours), the four writes lenient (`not_cross_site`, so a tool that sends neither header still works). A tool reading through `/fs/get` must now send `-H "Origin: http://sidecar.local"` as well as the token.

#### The page while it is unpaired

The Sidecar refuses everything but the open set, so the page's pollers would otherwise do nothing but make work on core 0 — which the audio shares — for as long as the tab is open. Measured on the device before this was fixed: `1992 not paired` in 24 minutes on a tab sitting on the dialog, about 2.5 requests a second, indefinitely, including a unit left on overnight.

While unpaired the page sends **only `/pair/status`, every 5 s** (0.2 requests a second, measured against the mock). `/status`, `/wifi/status` and the rest are not sent at all; the log poller keeps its place but fetches nothing, so the view catches up the moment the code is entered. The 5 s tick is what lets the page notice a reboot (a new code), the gate being turned off, or this browser being paired from elsewhere. Everything resumes on a successful pair, and `/time` is sent then.

The download's service worker does not retry either: a refusal is HTTP 200 with the code in the body, so without a check it would take the refusal's text for zip bytes. It looks for `X-Sidecar-Pair` and fails with a message instead.

#### Two addresses, one browser

A browser keeps `localStorage` **per origin**, and `http://sidecar.local` and `http://192.168.1.5` are different origins. A device paired at one is an unpaired browser at the other, the code has to be entered again with no explanation, and `/.rytm/pair.txt` ends up with two entries for one browser. Nothing can be done about that from inside the page — the other origin's storage is invisible to it — so the dialog says it instead: when the Sidecar remembers devices and this browser is not one of them, it names both addresses and offers the other one. `/pair/status` carries the mDNS name for that (`host`), which mDNS already broadcasts to the whole network.

If `sidecar.local` does not resolve in a browser but the IP works, that is the browser or the OS resolver, not the gate: the page itself is in the open set and never goes near the `Host` check, so a gate problem cannot look like "the site does not load". `curl http://sidecar.local/health` from a shell settles which it is in one command.

#### When nothing stays paired

`/pair/status` reports `"hdr"`: whether **that request** carried `X-Sidecar-Auth`. The page always sends it once it holds a token, so

```
curl -s "http://sidecar.local/pair/status" -H "X-Sidecar-Auth: $TOKEN" | grep -o '"hdr":[a-z]*'
```

answering `"hdr":false` means the header is not reaching `GetContent` at all — a kernel built without `pi/circle/circle-http-auth.patch`. That looks exactly like "the code is accepted and then nothing stays paired", because it is: `POST /pair` carries the code in its body and succeeds, and every request with the token it returns is refused. `web.cpp` calls `GetRequestAuth()`, so a tree without the patch does not build; a tree where `lib/net` was not rebuilt after it does.

#### Sessions, remembered devices, revoking

At most 16 sessions (the least recently used goes). Port 7310 takes none: there the connection itself is what is authenticated, and before 2026-10-29 every `sdboot.py` connection's `AUTH` made one that only pushed a browser's out. A session ends after **an hour with no gated request** and after **a day** whatever happens. An hour is long enough that nobody is interrupted mid-session; the day is a hard ceiling. A browser notices neither, because it resumes silently with its stored secret.

Remembered devices live in `/.rytm/pair.txt`, written exactly as `/.rytm/wifi.txt` is (`pair.txt.part` written, the current file kept as `pair.txt.prev`, then the rename; a failure leaves the file as it was). One line per device: `c=<8 hex id> <64 hex SHA-256 of the secret> <name>`. **What identifies a device is its secret, not its address** — an IP or a MAC is spoofable, changes with DHCP and differs between Wi-Fi and Ethernet. **Sixteen are kept** (eight until 2026-10-29): a studio has a handful of browsers, each counted twice if it is used at both `sidecar.local` and the IP, plus `sdboot.py`. When the table is full, the **least recently used browser** goes (with its sessions) rather than a dead end — **never `sdboot.py`'s record while a browser's is there to go instead**: it is used once in a while, for an install, and is how firmware is recovered, so the old oldest-first rule (and plain LRU) always picked it. "Used" is this run's last pair or resume; after a reboot the file's order (oldest first) decides. The file's size is checked at compile time (`kFileMax`), so a longer header can never make a save fail silently.

**Revoking:** the page's Status tab > Access has *Forget this device* and *Forget every device* (the second also ends `sdboot.py`'s access); the console's `pair=revoke` does the same; and deleting `/.rytm/pair.txt` from the card does it offline.

#### A saved pairing that has gone stale

A browser and `sdboot.py` both retry their saved credential on every start, so what happens when it is no longer good decides whether an ordinary user can lock themselves out. Two rules:

- **Only `pair()` counts towards the lockout.** The lockout exists to stop someone guessing eight digits, and `resume()` takes a 128-bit secret, which is not guessable — rate-limiting it protects nothing. Counting it meant a browser holding a stale entry locked its own owner out by being reloaded five times, and because the limiter is shared with port 7310 that also blocked `sdboot.py install`, the way firmware is recovered. A device must not be able to lock its owner out through its own saved state.
- **A failed resume is `kNoDevice`, not `kBadInput`.** "The pairing code is 8 digits" is a complaint about something the user typed; a stale credential is not that, and saying it sent a user hunting for a typo in a code they had entered correctly. The page clears its store and says the saved pairing is no longer valid.

A lockout does not block `resume()` either, and a successful one clears **the lockout of the address it came from** — that one only. Before 2026-10-29 it reset everyone's, so the owner's browsers resuming on every reload kept an attacker's lockouts at one minute instead of sixteen. It never refills the global backstop either.

The page checks the shape of what it stores, on write and on read (`pairId`, `pairSecret`: 8 and 32 hex). A junk entry — `undefined` written as a string when a field was missing, or anything left by an older build — is dropped locally instead of being sent. Both halves are tested: `test_paircode`'s `test_stale_device` for the rules, `page_net.test.mjs` for the store, and `page_pair.test.mjs` end to end (21 stale resumes leave pairing unlocked and the owner can still pair).

#### Rate limiting

It is a short code, so online guessing is the realistic attack. The compare is constant time (`paircode::ct_equal`, no early exit). Since 2026-10-29 the limiter is **per peer** — the client's IPv4 address, the same key on the web page and on TCP 7310 — with a **global backstop**:

- **Per peer:** at most **one attempt a second**, and **five wrong ones lock that address** for 1 minute, then 2, 4, 8 and 16 on each further run of five; a correct code from it clears both. Someone else on a shared Wi-Fi guessing locks **themselves** out, not the owner — before, one limiter for everyone let anyone on the network keep pairing (and `sdboot.py install`) locked for good. The table holds 16 addresses; a new one takes the stalest that is not locked out, and **when all 16 are locked out a new address is refused** rather than evicting one (which would start it afresh and let a 17th address walk past the limit).
- **Global backstop:** 30 wrong codes for everyone together, one back every 10 s (6 a minute). When it is empty every address waits, the owner's too, for at most 10 s at a time. An attacker's own addresses are locked while it refills, so keeping it empty for long takes about **20 addresses guessing at once**; keeping it busy bounds guessing at 6 a minute, about 8 600 a day, so a code (which changes at every boot anyway) has about a 1 in 11 600 chance a day against an attacker who can sustain that. `/status`'s `"pair":{"locked":N}` is the backstop's wait.
- **Not counted at all:** a malformed code (not 8 digits — a bare `POST /pair`, a `GET /pair`, a 7-digit typo: it cannot match, so counting it protects nothing), any resume, and the right code.

Every refused attempt is still counted for the log, including the ones the limiter turns away before any compare, and at most **one log line every 10 s** reports how many came (`take_report`) — otherwise an unauthenticated flood would push the log's tail out, and `/log` is open for the attacker to watch it work. A lockout does **not** lengthen on attempts it refuses outright, because that would let anyone keep an address locked for good.

Devices that are already paired keep working throughout, and a remembered device resumes whatever the limiter says. **What an attacker on the network can do is delay a *first* pairing** (a new browser), for as long as they keep guessing from enough addresses; they cannot touch a paired browser, `sdboot.py` once it is paired, or the serial console. Pair the devices you need before you go somewhere you do not trust. A physical "allow pairing now" button on the touchscreen / Rytm is for 0.3.1.

#### The random generator

The code, every session token and every client secret come from the SoC's hardware generator (mixed with the timer). It is **tested before it is trusted** (2026-10-29, BACKLOG F4): at boot `paircode_pi::Init` draws sixteen raw words and `Gate::init` sixteen mixed ones; if any two are alike the generator is stuck and the gate **refuses everything** for that boot — `POST /pair`, `/pair/resume` and 7310's `PAIR`/`AUTH` answer that the random number generator failed its self-test, the Rytm says `RNG FAILED`, `/health` says `"pair":"rng-failed"`, `/pair/status` `"rng":false`, the MIDI bridge will not turn on, and the log says it in capitals. A loud refusal rather than a guessable code. The serial console (and `:sdf`) still works; a restart tries again. The test catches a dead or stuck generator, not a subtly biased one — nothing this cheap can.

#### Port 7310

A line protocol, so it has its own step: `PAIR <code>` answers `OK <client-id> <secret>` (and remembers the client), `AUTH <id> <secret>` answers `OK`. Until one succeeds the connection answers only `ERR unauthenticated: …` and `BYE`, with one log line a connection so hammering it cannot fill the log.

`PAIR` and `AUTH` are limited per address like the page (above), and make no session. The port serves **one connection at a time**, so an idle one used to hold it — and every install — for good: a connection now gets 10 s of silence and 20 s in all to send `PAIR`/`AUTH`, then 60 s of silence once it has. `pi/circle/sdboot.py` does this: it reads `SIDECAR_PAIR_CODE` (or asks on the terminal, and fails with a clear message when there is no terminal), keeps what it is given in `~/.config/sidecar/pair.json` (0600, one entry per host, `SIDECAR_PAIR_FILE` moves it), and `AUTH`s from then on. The first install on a Pi:

```
SIDECAR_PAIR_CODE=12345678 uv run --with pyserial python pi/circle/sdboot.py install
```

**An engine from before 0.3.0 has no gate at all**, and upgrading one has to work: the client asks it to list the card first, and if that is answered instead of refused it goes ahead unauthenticated with one line saying so. The decision is made on the engine's own answer, never on a version string, so the only way to skip authentication is for the engine to demonstrably not ask for it. Without this, every upgrade from a pre-0.3.0 kernel stopped with "run it again with SIDECAR_PAIR_CODE=…" for a code that device does not have (found on hardware, 2026-10-25).

**The serial `:sdf` path is not gated**: whoever has the serial header has the board, and it is the way back in when the network side is locked out.

#### First pairing, for the owner (0.3.0: the gate is on by default)

A card made by `pi/circle/make-sd*.sh` has no `paircode=` in `cmdline.txt`, so the gate is **on** with a new random code at every boot. Read the code off the **Rytm**: a box in the middle of its screen at boot and when it connects, for 30 s (again whenever an unpaired page asks, or on `pair=show`). The code is also there on demand: **SIDECAR MENU > WEB UI** (UP + YES, then YES on SIDECAR, WEB UI) shows it, and the SIDECAR page (UP + T10) has it on its PAIR line. Then, once per device:

- **The web page:** open `http://sidecar.local/`, type the 8 digits, keep *remember this device* ticked, **Pair**. That browser resumes by itself from then on, across reboots. **Show it on the Rytm** in the dialog puts the code back on the Rytm's screen. Pairing at `http://sidecar.local` does not carry over to `http://<the IP>` (two sites to a browser): pair at the one you use.
- **`sdboot.py`** (installs, `ls`, `cmdline` …): the first run once with the code, `SIDECAR_PAIR_CODE=12345678 uv run --with pyserial python pi/circle/sdboot.py install`; it keeps a client id and secret in `~/.config/sidecar/pair.json` (0600) and never needs the code again for that Pi. A deploy script needs nothing else: export `SIDECAR_PAIR_CODE` for its first run against a freshly paired card, or run `sdboot.py ls` once by hand.
- **A script that uses HTTP** (`curl … /cmd`): pair once with `curl -s -d code=12345678 -d remember=1 -d name=deploy http://sidecar.local/pair` (it answers a `token`, a `client` and a `secret`: keep the last two), and at each later run trade them for a token with `curl -s -d client=<id> --data-urlencode secret@$HOME/.config/sidecar/deploy.secret http://sidecar.local/pair/resume` — the secret **from a file** (0600), not on the command line, where it lands in the shell's history and in `ps`. Send the token as the `X-Sidecar-Auth` header (`curl -H "X-Sidecar-Auth: $TOK" -d c=notes=1 http://sidecar.local/cmd`), or as `tok=` in a POST's body. **Never in the URL**: `?tok=` is refused since 2026-10-29 (BACKLOG F7) — a URL lands in history, proxy logs and `Referer` headers. A body `tok=` is not a CSRF hole: the token is not ambient (no cookie), so another site would have to know it.

**SIDECAR MENU > WEB UI** (on the Rytm): **WEB UI** ON / OFF — the web page and port 7310 from the network (OFF: every page and endpoint answers "the web UI is off on the Rytm (SIDECAR MENU > WEB UI)", port 7310 refuses every line as unauthenticated with that text; the serial `:sdf` still works), kept in `/.rytm/pair.txt` as `web=0` (default on; deleting the file turns it on again; pair.txt missing after a cut between its save's renames, `pair.txt.prev` is read and written back). With the gate off (`paircode=0`) or the RNG failed, pair.txt is never read or written, so WEB UI OFF holds for that boot only; **CODE**; **PAIRED DEVICES**; **NEW CODE** (a fresh random code for this boot; the paired devices stay paired; not on a fixed `paircode=<8 digits>`); **FORGET DEVICES** (YES twice: every device and session, as `pair=revoke`). The gate itself is not offered there: `cmdline.txt paircode=0` stays the dev card's bypass. The Rytm MIDI bridge's port is not part of WEB UI.

A development card keeps `paircode=0` (or a fixed `paircode=<8 digits>`) in its `cmdline.txt` until its owner removes it: `sdboot.py cmdline` shows the line, `sdboot.py cmdline "<the line without paircode=0>"` writes it.

#### For 0.3.1

- **F8:** the client secret sits in the browser's `localStorage` for good, with no rotation and no list of paired devices on the page (only the count, *Forget this device* and *Forget every device*).
- **TLS** (or a challenge-response that never sends a bearer): see "Threat model".
- **The physical "allow pairing now" button** (review M1): the Rytm opens a 2-minute window in which the global backstop is skipped (per-address limits still apply) and the code is drawn again.
- **L2:** `/status`'s `"pair":{"locked":N}` and `/pair/status`'s `wait` tell an attacker when the backstop refills: a free timing hint. Minor; drop or coarsen it.
- **L4:** `Ms()` is `GetUptime () * 1000` as a `u32`, which wraps after 49.7 days. The limiter's differences survive it; `Client.used`'s ordering does not (after a wrap the most recently used browser can be evicted first). Seconds, or 64 bits.
- **F5, judged not worth a change:** `/pair/status` answers `paired` for a token at any rate, outside the limiter. It is the same oracle every gated endpoint already is (a 401 or not), against a 128-bit token, so limiting this one endpoint would protect nothing; it does not keep a session alive (`check_quiet`).

#### No screen

If you are locked out, this is the paragraph you want.

The code is on the Rytm's screen, but a Rytm may be missing, or run an older mod that does not take the popups. So every boot with a random code also writes it to **`/sidecar-paircode.txt` at the card's root** (Finder-visible, like `sidecar-wifi.txt`), rewritten at every boot and at a new code. `/fs/*` cannot reach the card's root at all and every read needs the code, so it is not readable over the network; it is readable by whoever holds the card, which is the deliberate limit of this scheme (see "Threat model"). (Until 2026-11-04 the file was written only on a unit with no touchscreen; the local display is gone.)

If the Rytm will not take the popup (`core/screen` drops one when the Rytm's hello had no `36`, while its Sidecar page is open or during a splash) the engine retries for ten seconds and then says so, with what to do next.

**A unit that should never write its code down takes a fixed one instead: `cmdline.txt paircode=<8 digits>`.** That is the unattended path, and it is loud in its own right (below).

#### The development bypass

`cmdline.txt paircode=0` turns the gate off entirely. It is loud in four places, so a dev card can never be mistaken for a secured one: a `LogError` at boot ("THE PAIRING CODE IS OFF … anyone on this network can install firmware …"), a warning line **once a minute** for as long as it runs, `"pair":"off"` in `/health` and `"pair":{"gate":"off"}` in `/status`. `paircode=<8 digits>` keeps the gate but fixes the code; that is loud the same way (`fixed`), and writes no file. Anything else in `paircode=` is ignored and the gate stays on with a random code — garbage never weakens it.

#### Threat model

**Card credentials are protected against the network, not against physical possession.** That is a deliberate scope decision. The asset being defended is a unit on a shared or untrusted network; someone holding the card is out of scope.

It is also what this hardware can do. There is no secure element and no passphrase entered at boot, so any key the firmware used to decrypt the card would have to live on the card or inside the firmware image — both readable by whoever holds the card — and the Wi-Fi PSK must stay in usable form or the Pi cannot rejoin a network by itself. **Encrypting credentials at rest here is not achievable; do not attempt it.** `/.rytm/wifi.txt` therefore stays as it is, holding the derived 64-hex key (never the passphrase).

What the gate does **not** do: the transport is plain HTTP and a raw TCP socket on a LAN. Anyone who can watch the traffic can take a session token, or the secret once a run, or the code the one time it is typed, and use it. There is no TLS, no replay protection and no binding of a token to an address — `crypto.subtle` is unavailable in a non-secure context, so a challenge-response that never sends a bearer would mean hand-rolling SHA-256 in the page. The gate stops the person on the network who has not seen the screen. Treat the network as the outer lock.

#### A page left open across an install

The page is built into the kernel (`hosted/web_page.h`), so **an open tab keeps running the page from the kernel it was loaded from**. After an install it still shows the old UI until it is reloaded, which cost real time once: a validation message that had been reworded looked like a bug in the new build. It is simply a tab that was never reloaded. The pairing dialog prints the build and commit that is answering (`/health`, open) for exactly this reason: if it does not match what you installed, reload the page.

#### Code and tests

`pi/engine/paircode.{h,cpp}` (portable: the code, the compare, the limiter, sessions, the card's file, the 7310 lines), `pi/engine/paircode_pi.{h,cpp}` (the card, the hardware RNG, the boot messages, `/status`, the STATUS row, the console's `pair*`), the perimeter and `/pair*` in `web.cpp`, `/health` in `kernel.cpp`, the confinement in `fsweb.cpp`, the 7310 step in `sdfiles.cpp`, `pi/circle/circle-http-auth.patch` (Circle's HTTP daemon keeps `X-Sidecar-Auth`; applied by `build.sh` after `circle-http-origin.patch`), the dialog and the Access section in `www/index.html`.

- `make -C pi/engine/tests` (`test_paircode`): the code's shape and spread, the constant-time compare, use before init refusing rather than falling open, the limiter and its doubling lockout, the 10 s report throttle, sessions and both expiries, `check_quiet` not keeping a session alive, the card's file and its atomic save with failing writes and renames, a full file of the longest names, a device surviving a reboot, revocation, nine devices, the `cmdline.txt` parsing, the 7310 `PAIR`/`AUTH` lines, and that no answer, no error text and no file but the recourse one ever holds the code.
- `make -C pi/engine/tests` (`test_fsweb`): every denied path refused for every operation — `/.rytm/wifi.txt`, `/.rytm/pair.txt`, `/cmdline.txt`, `/config.txt`, `/engine.img`, `/overlays`, and near-misses like `/Samples2` — both ends of a `mv`, the content folders still working, and the root listing showing content only.
- `node --test pi/engine/tests/page_pair.test.mjs`: the open set read out of `web.cpp` and checked against `mock_web.py`; every route **derived from the sources** (web.cpp's own comparisons) that is not open proved to refuse an unpaired client, so a handler in neither list fails the test; the confinement holding for a **paired** client; a wrong code refused then locked; the token in the header and as `tok=` in a POST's body, and refused in the query string; a remembered device surviving a restart; revocation; `paircode=0` both ways; and the code in no answer and no log line.
- `python3 pi/engine/tests/test_sdboot.py`: `sdboot.py` against stand-in engines on the SD file port — a pre-0.3.0 one (no gate: it works, says so once, and never asks for a code), a gated one (pairs once with `SIDECAR_PAIR_CODE`, then `AUTH`s from what it stored, 0600), a gated one with no code and no terminal (it says the *device* is asking and where to read the code), a wrong code, and a stale stored secret falling back to the code.
- `make -C core test` (`test_generic`): the pairing modal over an open page: drawn on top, a key takes it off and the client never sees that key; a status popup with the page closed waits its turn.
- `node --test pi/engine/tests/page_net.test.mjs`: the token rides in `X-Sidecar-Auth` on this page's own requests only, a refusal reopens the dialog — and a refusal of a request sent *before* a pairing cannot undo it (the generation on `PAIR.tok`). That last one is the bug the user hit: "it confirms it is correct and saved, but the dialog stays on top of the page".
- Verified in a browser against the mock with `localStorage` cleared: the page loads from `/` alone, shows the dialog, says "waiting for the pairing code", `/log` and `/health` answer, everything else is refused, and typing the code brings the whole page up.
- **Not yet checked on hardware** (no Pi here): the Rytm popup actually showing the code, the hardware RNG, and a real `sdboot.py install` over 7310.

### Rytm MIDI bridge (2026-10-27, 0.3.0)

The Rytm's USB MIDI on **TCP port 7330**, for hardware testing: Iced-Cream's tools (analogrun, analogusb.py, elkflash,
elkbench) talk to a real Rytm through the Sidecar exactly as they talk to analog-emu's `--usb-midi-tcp`. How to use it:
`docs/HARDWARE-TESTING.md`. The code: `pi/engine/midibridge.h` (portable: the parser and queue, the copy, the gate and
its challenge, the blocks, the pace, the status JSON), `pi/engine/usbmidi_merge.h` (portable: the Poll that merges
everything into the Rytm's USB MIDI OUT; `CObUsbTransport::Poll` is its Circle side), `pi/engine/midibridge_pi.{h,cpp}`
(the port, its two tasks, `/status`, the page's endpoints, the console), the copy in `kernel.cpp`'s `MIDIPacketHandler`,
the Status tab's **Rytm MIDI bridge** section in `www/index.html`, and the Mac's relay `tools/rytm_bridge.py`.

**The stream.** Raw MIDI bytes both ways, no framing. Everything the Rytm sends is copied to the client, and the Sidecar
keeps getting all of it. A newer authenticated client replaces the older one on the Pi; the Mac relay lets one tool at a
time through (below).

**The merge (client to Rytm, `usbmidi_merge.h`).** The client's bytes are parsed into whole messages (running status
expanded, realtime bytes as messages of their own, a SysEx only once its F7 is in, at most 38912 bytes - the largest RPC
message, 37457, framed) and queued (256 KB; a full queue stops the reading, TCP's own flow control). Each Poll sends the
keys, the computer's MIDI and the engine's records first, then the bridge's, **paced** by a token bucket at 250 KB/s
(below the Rytm's ~300 KB/s, so a Poll does not sit in a blocking bulk transfer) and 512 bytes a Poll at most. One USB
MIDI cable means a half-sent SysEx holds everything else back: the bridge's (so a key waits 156 ms at most, the largest
SysEx at 250 KB/s - measured 149 ms in the test), the computer's, and the engine's (a song dump's 3069-byte pieces; the
keys and the computer's now wait for those too - before 0.3.0 they could land between the pieces - and an engine SysEx
whose next piece does not come within 250 Polls lets them go again, counted). **Realtime never waits:** the computer's
clock and transport (F8..FF from the DAW) go out as CIN F packets inside any held SysEx, in the Poll they arrive (USB
MIDI 1.0 allows that on one cable); only its other messages wait, in order - and once something waits only F8 and
FE still overtake it, so start / continue / stop keep their place behind a Song Position Pointer. **A failed send**
(Circle's bulk transfer fails after 3 s) - the bridge's, the engine's, the keys' or the computer's - ends that Poll and
turns all sending off for 5 s (doubling to 60 s while the retries fail too) or until a new umidi1, drops the bridge's
queue including a half-sent SysEx and the computer's deferred messages (counted): one 3 s stall per retry, never
one per chunk or per Poll, never the rest of a SysEx with a hole in it, and a Rytm that was only busy gets the Sidecar's own link back
by itself. The bridge reaches the Rytm with `link=usb` only.

**The copy (Rytm to client).** Non-blocking: the USB MIDI interrupt stages each message and publishes it to the relay
task only once whole (256 KB queue); the relay sends with a 2 ms limit. A client that stops reading loses whole messages
(`drops`), never part of one. Realtime bytes inside a SysEx are sent after its F7.

**Security.** The stream carries SysEx, which includes OS upgrades and +Drive writes, so:

- **Off at every boot**, and only with a pairing code: with `paircode=0` (cmdline.txt) it refuses to turn on at all,
  since anyone could then read the token from the page. On for this boot from the web page (Status > Rytm MIDI bridge:
  `POST /midibridge/set`, paired and behind the strict same-origin check) or the serial console (`midibridge=on`).
  `/cmd` refuses `midibridge=` (its origin check is the lenient one).
- **A challenge, not a password on the wire.** The Pi sends `SIDECAR-MIDIBRIDGE 1 <nonce>` (128 bits, new for each
  connection) and the client must answer `BRIDGE <HMAC-SHA256(token, nonce)>` within 1 s. The token is 128 bits from
  the hardware RNG, **new at every turn-on**, and shown only on the web page (`GET /midibridge`, paired, same-origin) -
  never in the log, `/status` or the console. A sniffed exchange cannot be replayed; someone who also sniffs the
  page's `GET /midibridge` (plain HTTP) has the token, and an active attacker on the network can take over the TCP
  stream after the handshake (there is no channel binding) - the pairing code's own limit. **Use it on a trusted
  network only, and turn it off when the test is done**: each turn-on makes a new token.
- **Lockouts per address, never the bridge off.** Five wrong answers from one address lock that address out for 10
  minutes (16 addresses tracked; a lock is not evicted by fresh ones). Nothing on the network can turn the bridge off. A
  first line that is not a `BRIDGE` line - an HTTP request a web page made to `sidecar.local:7330` - is refused and not
  counted, so a page in the tester's own browser cannot lock the tester's address out. The leftover bytes after a hello
  stay with that connection until it is accepted.
- **The pairing reset closes it.** *Forget every device* and `pair=revoke` turn it off, the switches off, and forget
  the lockouts. The client's address is logged at every connect and close; it is in `GET /midibridge` (paired) but not
  in the open `/status`.
- **Blocked by default, each with its own switch** (off at every boot and with the bridge, needing the bridge on;
  re-checked when a queued message would start, so a switch turned off drops what was queued under it):
  - **OS upgrades**: the MIDI RPC's OsUpgradeStart / Write / End (0x50..0x52) and the OS `.syx` image's own messages
    (`F0 00 20 3C <dev> 00 7E|7F`, as `Analog-Rytm_MKII_OS1.74.syx`). Switch: *allow OS upgrades* / `midibridge=os:on`.
  - **+Drive writes, as an allowlist**: every RPC command but the confirmed reads needs the switch, so an unknown id
    fails closed. The reads (elektroid's `src/connectors/elektron.c`): ping 01, version 02, UID 03, screenshot 04,
    storage info 05, query 09; the sample file system's read dir 10, file info 22 23, reader 30-32; the raw file
    system's read dir 14, file info 26, reader 33-35; the data store's list 53 and reader 54-56. The writes this
    blocks include the sample FS (11 12 20 21 40-42), the raw FS (15 16 24 25 43-45), the RAM slots (18 19) and the
    data store (57-5D: write, move, copy, clear, swap). Switch: *allow +Drive writes* / `midibridge=data:on`.
    **analogrun's data writes (`data put`, `roundtrip`, `clear`) need it on.**
  - The RPC command is decoded whatever the device byte, except the Rytm's own dumps and requests (device 07, id
    0x50-0x6F) and analoghealth's channel (device 7D; the stock OS drops it, which suggests the RPC parser checks the
    device byte).
  - Not filtered: kit / pattern / sound dumps (a test plan may overwrite the working kit), and the factory protocol's
    `#UPGRADE` text commands (unknown transport, `rytm-mods/re/subsystems/os-upgrade.md`).

**`/status`** has `"midibridge":{"on","osUpgrade","dataWrites","state":"off|waiting|connected","port","connects",
"replaced","refused","lockedAddrs","toRytm","toRytmMsgs","fromRytm","drops","dropBytes","blocked","blockedData",
"tooLong","junk","flushed","queued","rytmFail","rateBps"}` - no token, no address. The console's `midibridge` logs the same.

**Tests.** `make -C pi/engine/tests` (`test_midibridge`): **the real Poll** (`usbmidi_merge.h`) with a fake endpoint -
the order with keys, the computer's SysEx and the engine's song-dump pieces every Poll and no packet inside anyone's
SysEx; the pace over a second (at most 250 KB + the bucket); the largest SysEx holding a key 149 ms; a send failing in
the middle of a SysEx (one failed send, the queue and the half SysEx dropped, nothing more sent and the others dropped
until a new umidi1, then whole messages again); an engine failure; no Rytm; the computer's SysEx holding everyone; an
engine piece whose next one never comes; the retry after a failed send (5 s, then 10 s while it keeps failing); the
computer's clock during the largest bridge SysEx and an engine song dump, every F8 out in the Poll it arrived. A Rytm
that is enumerated but not reading: one send a Poll whatever is queued, including the retries, and the deferred
messages dropped and counted; a Song Position Pointer then Continue during a hold keep their order. And the parser (F9/FD, an over-long SysEx cut by a status, an OS start split at
every byte boundary, a budget under 3 while open), back-pressure, the blocks (any device byte, every write and read id,
the real `.syx` bytes, kit dumps whose 12th byte looks like a command, analoghealth's channel, a switch turned off with
messages queued, and every RPC id 00-7F and its reply id through the allowlist), HMAC-SHA256 against RFC 4231 and against Python's for the relay's answer, the hello line and its
leftover, the gate (replay with a new nonce, the per-address lockout and its expiry, non-BRIDGE lines not counted, a new
token at each enable), the status JSON (no token and no address in `/status`), and the copy. `tools/tests/test_rytm_bridge.py`
(run by the same make): the relay against a fake Sidecar speaking the challenge - raw bytes both ways, an
HTTP POST refused before it reaches the Sidecar, a silent client closed after 1 s without ever taking the slot (and,
with the opt-in `--passive`, a tool that reads first let through, its later text still refused), a wrong
token, one tool at a time and eight racing, the token from a 0600 file or the environment only, `::1`. `node --test
pi/engine/tests/page_pair.test.mjs`: `/midibridge` and `/midibridge/set` refused to an unpaired client, and their web.cpp
branch on the strict same-origin check with the switches from the body only. **Not yet checked on hardware.**

### OS upgrades over the USB-C MIDI port (usbmidi=os:)

Elektron Transfer or elkflash on the computer at the USB-C port can upgrade the Rytm's OS through the Sidecar. Off by
default: the gadget's filter drops OS upgrade SysEx (`F0 00 20 3C <dev> 00 7E|7F`, `midi_filter.h`) as before. The
wording follows the Rytm MIDI bridge's `midibridge=os:on`; the parsing is `pi/engine/usbmidi_os.h`
(tests/test_usbmidi_os.cpp).

- `usbmidi=os:off` (default), `usbmidi=os:emu` (pass only while the TCP link to rytm-emu is the host), `usbmidi=os:on`
  (pass to the real Rytm on USB too). In `cmdline.txt`, or as a serial console command; `POST /cmd` refuses it, as it
  refuses `midibridge=` (its origin check is the lenient one). `osupdate=off|0|emu|1|rytm` is the
  older name and still works (`rytm` = `os:on`, `1` = `emu`); a valid `usbmidi=` wins when both are given.
- While on: the USB-C MIDI OUT endpoint waits (NAKs) instead of dropping when the Rytm-bound queue is full, so a whole
  OS file arrives; under `link=tcp`, the computer's MIDI goes to rytm-emu as `'M'` messages and its USB MIDI comes back.
- `/status`: `"usbmidi":{"os":"off|emu|on","osUpgrade":true|false}` (`osUpgrade`: they pass right now).
- After the upgrade the Rytm reboots; `pi/circle/circle-replug.patch` has the hub enumerate it again.
- `usbreboot=1` (separate): the computer's `F0 7D 52 54 7E "REBOOT" F7` on the USB-C MIDI port reboots the Pi
  (`reboot_sysex.h`; `pi/circle/sidecar-reboot.ps1` sends it on Windows).

### The web page over USB-C (plan, not built)

Move's way: plugging the Pi into the Mac's USB-C alone gives sidecar.local. What it takes, and why it is not built yet:

1. A USB network function in our composite device (`sidecar_gadget.*`), next to UAC2 12/12 and USB MIDI: CDC-NCM (macOS's driver since 10.15; preferred: several Ethernet frames per transfer) or CDC-ECM (fallback, simpler: one frame per bulk transfer, also native on macOS). Each is an interface association with a communication interface (an interrupt IN endpoint for the link and speed notifications) and a data interface with alternate setting 0 (no endpoints) and 1 (bulk IN + bulk OUT), plus the CDC header, union and Ethernet functional descriptors and a MAC address string. NCM adds the NTB parameters request and NTH16/NDP16 framing.
2. Endpoints and FIFO RAM: the Pi 4's DWC2 has a fixed endpoint count and shared FIFO RAM; UAC2 12 in / 12 out at 24-bit high speed (isochronous, up to 1 KB+ per microframe each way) and MIDI already use 5. Three more (interrupt IN, bulk IN, bulk OUT) need the TX FIFO split to be checked against the isochronous IN's (circle-gadget.patch sizes them); this is the main risk to the audio.
3. A Circle network device bound to that function (a `CNetDevice` whose send queues a frame on the bulk IN, whose receive takes frames from the bulk OUT). Circle's `CNetSubSystem` runs one network device: Ethernet or the USB link, not both, unless the network layer is patched for two interfaces (routing by subnet). The simple first step: `net=usb` uses the USB link instead of the Ethernet.
4. A tiny DHCP server on that link (UDP 67: answer DISCOVER and REQUEST with one fixed address, e.g. the Pi 10.77.0.1, the Mac 10.77.0.2/24, no router so the Mac keeps its own internet route) - or no DHCP at all and both ends link-local (macOS self-assigns 169.254/16 on an interface without DHCP after a few seconds; the Pi then needs link-local autoconfiguration, ARP probing for a free 169.254 address).
5. mDNS on that interface: unchanged once it is the network device.

Effort: about 3-5 days (descriptor and class requests 1, the net device and NCM framing 1-2, DHCP 0.5, the endpoint/FIFO budget and a long audio run with the link busy 1). Risk: medium-high, all of it on core 0's USB-C path that carries the 12-channel audio; every step needs the `sidecar_audio_test.py` long run at 0 dropouts. Recommended order: ECM first (smaller), `net=usb` either-or, link-local on both ends (no DHCP), then NCM and two interfaces if it proves worth it.

## Temperature warning (2026-10-25)

The SoC's temperature is read once a second (the stats line's reading: one mailbox call; no other code calls it, and
`/status` and the warning use that reading, so a VideoCore stall is paid once a second at most).
`pi/engine/thermal.h` turns it into one warning state:

- **On at 80 C, off below 75 C** (75 / 70 until 2026-10-29, then briefly 78 / 73; the user: the unit does not feel hot
  and the Pi has its own throttle). 79 stays warned, 75 stays warned, 74 clears; a unit sitting at 80 does not flicker
  (tests: `pi/engine/tests/test_thermal.cpp`). A reading of 0 (the mailbox failed) changes nothing.
- **The clock Circle lowered.** Above `socmaxtemp` (78 on the card, Circle's maximum; 60 on a card with no `socmaxtemp=`, which
  is what `make-sd-pi5.sh` makes) `CCPUThrottle` drops the ARM clock to its minimum (600 MHz, a Pi 5's 1500) and raises it 3 C below, so by 80 C the clock has usually been lowered already. `thermal::clock_low_mhz` (the
  clock read back well below the setting, the SoC at 75 C or more; a lower Settings clock is not "lowered") makes it
  honest: warned, the Rytm's popup says `SOC 80C CLOCK LOWERED / TO 600 MHZ (UP+T10)` and `/status` has `"clockLow":600`
  (the web page's Temperature tile). Lowered under the warning (78-79 C): the tile only, quietly - no popup.
- **Where it shows.** `/status` `"tempWarn"` (and `"clockLow"`), the web page's Temperature tile, and the Rytm's screen
  once (below).
- **The log.** One line per crossing, not per sample: `thermal: SoC 81 C, warning on (from 80 C, off below 75 C),
  warning N this boot, ARM clock M MHz` (one mailbox call for the clock, at the crossing only) and `thermal: SoC 74 C,
  warning off (peak 81 C)`.
- **The Rytm's popup: once per crossing, not once per boot.** A unit that cooled and heated again is news again; once per
  boot would stay silent for the rest of the evening after the first crossing. The popup (`SIDECAR RUNNING HOT / SEE STATUS
  (UP+T10)`, or the clock-lowered text above, through `core/screen`) is offered every second until the Rytm's remote UI has shown it (its count
  of shown popups moves), and dropped when the warning clears first. It **does not override** a page the user is looking at
  (unlike the pairing code): nothing is lost by waiting, and a popup that wipes a page
  mid-edit for a condition that is not an emergency (the firmware throttles at 80 C, 85 C is its hard limit) is the wrong
  trade. It appears when the page closes, if the warning still stands.

## Idle sleep (2026-11-01)

Where each core waits (first written after a Pi 5 reached 79 C at 1-4 % load).

- **Cores 1 and 2:** nothing renders: they sleep in `wfe` and wake only to sleep again.
- **Control core (3):** `wfe` between passes, bound to 152 us by the event stream (a whole pass lies between its checks and
  its `wfe`, and a spin lock inside can spend the event). Woken by a MIDI byte or a command (`sev`).
- **Core 0:** the only core that really spun: `MsSleep (1)` at the loop's end leaves the CPU to Circle's scheduler, and
  `CNetTask` is always ready, so it runs the network's `Process` flat out. Now the loop yields to the tasks and sleeps in
  `wfe` until the 1 ms is over (`idlewait::spend`; no Circle change). The sleep is `sevl; wfe; wfe` (`idlewait::nap`), not one
  `wfe`: the network's round ends in `CSpinLock::Acquire` and left the event register set, so a plain `wfe` returned at once
  (Pi 5: 1.2 million wakes a second, 3 % asleep). Core 0 waits for no event, so dropping it loses nothing; the other cores keep a plain `wfe`. Interrupts (USB, audio) wake it at once, as they
  interrupted the spin. The loop and the network's polling are late by 152 us at most (the event stream). Card service and USB polling stay
  in the loop as they were:
  the sleep is only in the time the loop already gave away, not in `Yield`s inside a card or USB wait.
- **Seen in:** `/status` `"idle":{"wfe":true,"asleepPct":[c0,c1,c2,c3],"wakes":[...]}` (the last second, wakes a second), and
  every 10 s the log line `idle: asleep ...` with the clock and the SoC temperature. Core 3's pass-through wakes are in `wakes`.
## CPU clock (2026-10-25)

**1500** (the default: the board's full clock, 1500 MHz on the Pi 4; a Pi 5 keeps its own maximum), **1200** or **1000 MHz**: a console command (`cpumhz=1200`; `cpumhz` alone prints the state), stored as `cpumhz=` in `/.rytm/cpu.txt` (empty at the default; an older card's `/.rytm/display.txt` is still read), and `cpumhz=` in cmdline.txt wins. With nothing rendering it is a thermal lever, not a performance one.

- **On a Pi 5.** The firmware's range is 1500-2400. The setting 1500 means "full", so the clock is **2400**; 1200 and 1000 both come to **1500**, the firmware's minimum, held there. `/status` `cpu`: `setting`, `mhz` (what the firmware says it set), `askedMhz` (what the setting comes to here), `fullMhz`, `minMhz`, `measuredMhz` (what it measured, every 10 s: `mhz` only repeats the rate asked for, so the firmware's own throttle shows only here; a gap over 5 % is logged), `socMaxC`, `capped`.
- **Mechanism: runtime, not config.txt's `arm_freq`.** `arm_freq` needs a reboot and lives in a boot file the file API cannot write. Below the full clock the ARM clock is set through the mailbox (`SET_CLOCK_RATE`, one call and one to read it back, on core 0 when the setting changes), and the kernel does **not** call `CCPUThrottle::Update` while it is held there (it would put the maximum back whenever the SoC is cool). Back at 1500, `CCPUThrottle::SetSpeed` sets the maximum and `Update` runs again: it lowers the clock to the minimum above `socmaxtemp` and raises it 3 C below. The firmware's own limits (soft 80 C, hard 85 C; `get_throttled` bits in `/status` `power`) stay in every mode.
- **The clock read back** once a second and after each `Update` (`cpuclock::follow_mhz`): a move is logged (`cpu: ARM clock N MHz (was M, ...)`), and the temperature warning's "clock lowered" text uses it.
- **The case fan** (the GeeekPi case) is controlled only by the case board, not by the Pi, and not through `gpiofanpin=` (that Circle option would make `CCPUThrottle` drive a GPIO fan instead of the clock).

## Safety

- Nothing is sent to the Rytm except Overbridge blocks, enumeration, SET_INTERFACE and, on its USB MIDI port, the engine's host-link answers (`tohost=0` drops them), what the USB-A MIDI devices pass on (`usba=off` stops it), the MIDI bridge's traffic while a tool is connected, and the computer's MIDI from the USB-C port (OS updates only with `usbmidi=os:`).
- With every processor slot empty, Overbridge OUT is the computer's audio unchanged; a slot's output is added and saturated, never wrapped.
- An exception on any core leaves its breadcrumb (`crumb.h`, reported at the next boot) and ends in a clean stop and a reboot (`guard.cpp`).
- Reboots (the flasher's magic, a fatal error, a Wi-Fi change, another box's state folder, the SD file port, `usbreboot=1`) stop the Overbridge stream first: no resubmits, the URBs drained, alt 0 on interfaces 1 and 2.
  - A hang of core 0's loop reboots through the watchdog (15 s) without that stop.
  - A fault on core 0 only stops resubmitting before Circle's panic.
  - In both cases the Rytm may then need a power cycle (spike3).
- The Pi's SD card is written only in `/.rytm/` (the engine's state, atomically), `/Samples` (the file service, paired clients only), the boot files through the SD file port (paired) and `/sidecar-paircode.txt` on a unit with no screen.
