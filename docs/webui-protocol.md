# Web console protocol (Teensy)

The Teensy console page (`web/`) manages user machines, shows the memory map and browses the SD card over the same
WebSerial port as the log. Firmware: `teensy/voiceloop/webui.h`, included by `voiceloop.ino` (`RYTM_WEBUI`, on by
default, `-DRYTM_WEBUI=0` leaves it out). Page side: `web/proto.js` (frames, CRC, replies), `web/link.js`
(requests and transfers), `web/rmp.js` (package checks), `web/manage.js` (the views), `web/mockdev.js` (a fake
device for `?mock` and the tests).

Shared shapes. The JSON objects `plugin`, `plugins`, `slot` and `mem`/`mem_end` are shared with the Pi engine's user
machines (`GET /um`, branch pi-user-machines, `docs/user-machines-pi.md`). The Pi adds `plugin.file`, `plugin.fp`,
the `mem` kinds `code`/`state`/`heap`, and AArch64 packages (file magic `RMPA`). The Teensy reports `target:"arm32"`
and refuses `RMPA` packages. Merge this file with the Pi's when both are on one branch. Nothing here changes `core/`.

## Replies

Every reply is a log line that starts with `@`. It goes through the normal log writer, so the host build's log stays
plain text and a framed link (`'L'`) carries the replies too.

- `@{...}`: one JSON object with a type `t`. Names and paths are byte strings: printable ASCII as is, and `"`, `\`
  and every other byte as `\u00XX`. The page maps those code units back to bytes and decodes UTF-8, because the log
  parser drops bytes >= 0x7f.
- `@d <id> <off> <crc32 hex> <base64>`: download data. The CRC covers the line's own bytes. A line with no data
  ends the file.

A line is at most ~1.1 KB on the plain log. On a framed link it is at most 255 bytes, so download lines carry 168
bytes there instead of 768.

## Console lines (`:` + text + `\n`, at most 63 characters)

| line | reply |
|---|---|
| `mem` | `{"t":"mem","k":K,"size":N,"used":N,...}` for each region, then `{"t":"mem_end"}` |
| `plugins` | `{"t":"plugin",...}` for each package, `{"t":"slot",...}` for U1..U5, then `{"t":"plugins",...}` |
| `plugin=rescan` | `{"t":"pl","op":"rescan","st":"ok"}`: the store generation moves on (the Rytm's slot menu list is rebuilt) and MISSING slots look again |
| `plugin=del:ID` | `{"t":"pl","op":"del","id","st":"ok"\|"err","why"}`: removes **one** package. Refused while the transport runs, an install runs or a slot is loading |
| `plugin=set:U:ID` | `{"t":"pl","op":"set","slot":U,...}`: U1..U5 of the current project plays ID at its installed version (the slot map file is saved, then the slot loads) |
| `plugin=clr:U` | the same reply: U is emptied (explicitly: neither inheritance nor the sounds' fingerprints fill it) |
| `plugin=put:U:ID:a.b.c[:NAME]` | `{"t":"pl","op":"put","slot":U,"id",...,"installed":bool}`: an imported slot list's entry; ID `-` = explicitly empty; the package need not be installed (MISSING by name until it is). A name that would make the line longer than 63 characters is left out (web/slotlist.js) |
| `plugin=empty` | `{"t":"pl","op":"empty"}`: "start this project empty", U1..U5 explicitly empty |

The `slot` lines add `src` (none, saved, sounds, inherited, empty, imported), `updated`, `collision`, `unknown`,
`mixed`, `want_fp`, `want_n`, `want_id`, `want_ver`, `want_name` and `fp`, and `why` when the slot is not READY (in words: "no room: needs 23288 B, 584 B free", "muted: timed out"; docs/plugins.md 10.1). The `plugins` line adds `prints` (rytm,
waiting or trigs) and `fresh`. Both follow docs/user-machines-pi.md, "Which machine a slot plays".
| `df` | `{"t":"df","st":"ok","total_kb","free_kb","type","ms"}`: one scan of the FAT or the exFAT bitmap. Cached afterwards and lowered after each write |

`mem` regions:

| k | size / used | extra |
|---|---|---|
| `itcm` | `_itcm_block_count` x 32 KB / `.text.itcm` | `blocks` |
| `dtcm` | 512 KB - ITCM / `.data` + `.bss` | `stack_free` (SP - `_ebss` now), `stack_size` |
| `ocram` | 512 KB / DMAMEM statics + heap | `static`, `heap` |
| `flash` | 7936 KB / the program image | |
| `store` | the plugin store (LittleFS, 4 MB) / its used blocks | `mounted`, `pkg_max` (64 KB) |
| `arena` | the code arena (32 KB, DTCM, executable) / loaded images | |
| `state` | what one voice's plugin state may use (16 KB of voice memory, or PSRAM) | `voice`, `psram` |
| `psram` | PSRAM MB << 20 | |
| `sd` | the card; `size_kb`, plus `free_kb` / `used_kb` once `df` has run | `type` |

`plugin`: `{"t":"plugin","id","size","ver","name","api","image","bss","code","state","cpu","target":"arm32","slots":[U..],"fit":bool,"status":"ok"|"no fit"|"bad header","why"}`.
`code` = image + bss, which lives in the code arena while the package is loaded. `state` is the RAM per voice that
plays it. `cpu` is the declared per-voice CPU in 1/1000 of the Teensy. `fit` uses the loader's own limits:
API <= the firmware's, code <= the arena, state <= the voice memory (or PSRAM).

`slot`: `{"t":"slot","slot":1..5,"id","ver","state":"EMPTY|READY|MISSING|BAD|NO_ROOM|MUTED|LOADING","name"}`.

`plugins`: `{"t":"plugins","n","more","project","arena":[size,used],"state_limit","store":[size,free],"api","safe_mode","transport"}`.

## Frames (`'Y'`)

`'Y' <n u16 LE> <payload> <crc32 u32 LE of the payload>`, with n <= 1030. CRC-32 is IEEE (zlib's). `'Y'` is not one
of the development frame letters, so a `'Y'` frame does not switch the host build's log to framed. A frame with a
bad CRC is answered `{"t":"frame","st":"crc"}` and otherwise ignored. The sender resends it after its timeout.

| op | payload after the op byte | reply |
|---|---|---|
| `o` open upload | kind (`f` card file, `p` plugin package), id, size u32, crc32 u32 of the whole file, path (UTF-8, `f` only) | `{"t":"up","x":id,"st":"open","off":N,"max":1024}`. The same kind/id/size/crc again resumes at `off` |
| `w` write | id, off u32, data (<= 1024) | `ack` with the next `off`, `want` with the offset it needs, `busy` (transport running: resend later), or `err` |
| `c` commit | id | `done` (`size`, `ms`, `kbs` as the Teensy measured them) or `err` (CRC mismatch: nothing kept) |
| `a` abort | id | `aborted`: the `.part` file or the store install is dropped |
| `g` open download | id, path | `{"t":"dn","x":id,"st":"open","size":N,"max":768}` |
| `r` read | id, off u32, n u32 | `@d` lines, one per loop pass, until n bytes or the end of the file |
| `l` list | path | `{"t":"ls","n","s","d":0\|1,"m":"YYYY-MM-DD hh:mm"}` per entry (16 a pass, at most 4000), then `{"t":"ls_end","p","st","n","more"}` |
| `d` delete | path | `{"t":"fs","op":"d","st","p","why"}`: a file or an **empty** folder (the page deletes trees itself) |
| `m` mkdir | path | `fs` reply, op `m` |
| `n` rename | path NUL newpath | `fs` reply, op `n`. Refused if the new name exists |

How the firmware behaves:

- **Card files.** An upload is written to `<path>.part` and renamed over `<path>` only when every byte is in and the
  whole-file CRC matches. Every chunk has its own frame CRC as well.
- **Plugin packages.** These go through the store's existing install (`plug_install_begin/write/commit`, the same as
  the development `'I' 'J' 'K'` frames). The first chunk's header is checked against the limits above and refused
  with the reason if it will not fit. The page checks the same before sending (`web/rmp.js`). The commit is
  reported as done only if the store then holds exactly that header.
- **Never the whole store.** There is no wipe here. `'F' "WIPE"` stays a development-tool frame and the page never
  sends it.
- **Protected paths.** `/`, `/Samples`, `/.rytm` and anything under `/.rytm/` cannot be deleted, renamed or
  written. These hold the card index, slot maps and pools.
- **Audio first.** Each loop pass does at most one card or store operation, and only between requests (the host
  FIFO at its target), after the plugin passes and before the sample streams' top-up. Two operations are at least
  300 us apart, or 20 ms while the transport runs.
  - Writes, deletes, renames, `df` and store installs **wait for the transport to stop** (STOP TO INSTALL). A chunk
    sent while the transport runs is answered `busy`, and the page shows "paused" and resends.
  - Reads and listings go on during play, slowed to about 50 operations a second (~37 KB/s).
- **Timeout.** A transfer with no frame for 30 s is aborted, and its `.part` file is removed.
- **Resume.** If the device goes away (an unplug, or a usbmode re-enumeration) and comes back within 30 s, the page
  re-opens the same upload and continues from the Teensy's offset.

## Throughput

Nothing has been measured on hardware yet: the Teensy was in use for hardware tests, so nothing was uploaded. The
page shows the speed of every transfer, and the Teensy's `done` reply carries its own `ms`/`kbs`. Estimate, with the
transport stopped:

- **Upload.** Stop-and-wait, 1 KB per round trip. The round trip is one loop pass plus the SD write of 2 sectors
  (~0.3-1 ms; a card's occasional busy period is longer) plus WebSerial's turnaround (~1-2 ms in Chrome). That gives
  roughly **300-600 KB/s**.
- **Download.** 768 bytes a pass, 300 us apart, streamed in 32 KB windows. That gives roughly **0.5-1.5 MB/s**,
  bounded by the base64 lines and the page's parser.

These are expectations. Fill in the measured numbers from the first hardware session (`log`: `-- uploaded ... KB/s`).

## RAM and ITCM

All code is `FLASHMEM`, and the strings are `PSTR`. There is no `memchr`/`strstr` (libc's would be linked into
ITCM). Host build, measured with arduino-cli and Teensyduino 1.62:

| | before | with webui.h |
|---|---|---|
| ITCM | 129048 B, 4 blocks, 2024 B headroom | 129048 B, 4 blocks, **2024 B headroom (unchanged)** |
| RAM1 locals (stack) | 88480 B | 83520 B (the input buffer grew 300 -> 1100 B; the transfer buffers ~4 KB) |
| RAM2 free for malloc | 18112 B | 18112 B (unchanged) |
| flash code | 308812 B | 322500 B |

The Sidecar | Expander (Mini) build (`teensy/host_uac2.sh`, `RMP_CORE=uac2`): ITCM unchanged (130280 B, 792 B
headroom). Locals drop from 47264 B to 43104 B. `run_sketch.sh` fails a build under 40 KB of locals, so about 2 KB
of RAM1 is left before that check trips. The serial build and `-DRYTM_PLUGINS=0` compile as well.

Names longer than 120 bytes are cut in listings (the reply line's worst case, every byte escaped, has to fit).

## Disk mode (next step, not in this firmware)

The goal is a console button that switches the micro-USB port to a USB disk for the SD card, faster than the
browser, and back. Options in Teensyduino 1.62:

- **MTP** (`MTP_Teensy`, in the 1.62 core: `cores/teensy4/MTP_Teensy.cpp`, `usb_mtp.c`). MIT-style licence (the
  PJRC core's). Supported, but marked "Experimental" in the board menu.
- **Mass storage (MSC) as a device.** Teensyduino 1.62 has none: USBHost_t36's MSC is the host side. No maintained
  device-side MSC library exists for the Teensy 4 core. MSC would also hand the computer raw blocks, so the Teensy
  would have to unmount the card: no sample streaming, no slot maps or pools until switched back.

**Choice: MTP.** It is the one supported option, and it works at file level through the Teensy's own SdFat volume, so
the engine keeps a consistent view of the card. The cost: macOS has no built-in MTP client (it needs a tool such as
OpenMTP or Android File Transfer). Windows and Linux have one.

Why it is not in this round:

1. **Descriptor work in the patched core.** The runtime `:usbmode` switch lives in `teensy/uac2/` (the
   `USB_SERIAL_UAC2` descriptor, built per mode by `usb_uac2_config_desc`). Disk mode needs a fourth mode,
   `UAC2_MODE_DISK`, whose descriptor swaps the audio function (it is last) for an MTP interface. That means:
   - MTP's endpoints, possibly reusing UAC2's numbers, since only one function is configured at a time;
   - `usb_mtp_configure()` called only in that mode;
   - `MTP.loop()` called from `loop()`;
   - an `FS` adapter over `cards.sd()` (so MTP and the engine share one SdFat volume and its cache, never two
     mounts).
2. **RAM and ITCM (measured).** A minimal sketch with `MTP.addFilesystem(SD)`, compiled as USB type Serial and as
   Serial + MTP (Teensyduino 1.62, `-O2`), shows MTP adds:
   - **56.5 KB of ITCM code** (45816 -> 102328 B; the core's MTP code is not `FLASHMEM`);
   - **16.4 KB of RAM2** (12416 -> 28800 B);
   - 5 KB of RAM1 variables.

   The host builds have 0.8-2 KB of ITCM headroom and 18 KB of RAM2. As it stands, MTP would take two more ITCM blocks
   (64 KB less stack: the uac2 build would fail its 40 KB locals check) and nearly all of RAM2. It needs MTP_Teensy's
   code moved to flash (FLASHMEM, or a linker rule like `teensy/cold_ld.sh`'s for LittleFS/SdFat) and its buffers
   shrunk first.
3. **Test risk.** Changing the USB descriptors cannot be tested without the hardware, and a broken descriptor makes
   the Teensy disappear from the computer, so it can only be recovered with the button.

The page already has the button, disabled, with the warning text it will need. The steps when this is picked up:

- Add `UAC2_MODE_DISK` to `usb_uac2.h/.c` and the patch, plus `usbmode=disk` in `usb_host.h`. Do **not** keep it
  across reboots: a boot always comes up in audio.
- While in disk mode:
  - pause the web console's card operations and the engine's card writes (slot-map saves, pool saves, idle_save);
  - keep reading samples that are already open;
  - keep the Rytm host side running (it is unaffected: the host port is separate).
- `usbmode=multi` (or the page's "Back to audio" button) re-enumerates back, and the page reconnects as it does for
  any usbmode change.
- Measure: RAM2 after linking MTP, the MTP throughput, and xruns during a copy.
