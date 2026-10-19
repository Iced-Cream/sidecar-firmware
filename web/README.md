# Teensy console page

A single static page that talks to the Teensy 4.1 expander over WebSerial: connect, live log, a few controls,
free-form console lines, the user machines (install, delete, assign to U1..U5, memory fit), the live memory map
and an SD card browser (upload, download, rename, delete, new folder, free space). No build step, no external files.

## Run

```
cd web && python3 -m http.server 8000
```

Open http://localhost:8000 in Chrome or Edge (desktop). WebSerial needs a secure context; localhost counts.
Other browsers get a notice. `http://localhost:8000/?mock` runs against a fake device (no port is opened);
`mockUnplug()` / `mockReplug()` in the devtools console play a re-enumeration.

Connect picks the Teensy (PJRC vendor 0x16C0). A port granted before reconnects on page load, and when the
device re-enumerates (reboot, upload) the page reopens it.

## Commands

Sent as `:` + line + `\n`, at most 63 characters (voiceloop.ino `console_line`):

| line | what |
|---|---|
| `notes=1` / `notes=0` | the Rytm's MIDI notes play the voices (channel N = voice N-1) |
| `notes` | the state and each voice's machine |
| `notemach=V:NAME` | voice V (0..7) plays NAME: plaits juno daisy dsps grain dd fm |
| `out=main` / `out=tracks` / `out` | host build: voices on MAIN L/R or per track; `out` shows it |
| `fifo=N` / `fifo` | host build: the FIFO target in frames; `fifo` shows it |
| `cap=MS` | host build: capture MS ms of the Overbridge IN stream (free-form input only) |
| `mac=on` / `mac=off` / `mac` | Sidecar | Expander (Mini) build (`teensy/host_uac2.sh`): the computer's USB audio OUT 1..12 to the Rytm's Overbridge inputs |
| `usbmode=stereo` / `off` (/ `multi`) / `usbmode` | Sidecar Mini (`teensy/host_stereo.sh`): stereo = audio 2 in / 2 out (the Rytm's MAIN L/R both ways), off = Serial only; the 20 / 12 build (`teensy/host_uac2.sh`) also has multi (20 in / 12 out) and USB MIDI; kept across reboots; a change re-enumerates the Teensy |

| `lfo` / `lfo=N:KEY:VALUE` | the LFOs (LFO tab, `web/lfo.js`; docs/lfo.md): `lfo` answers `@{"t":"lfo","lfo":{...}}` |
| `rec` / `rec=start\|stop\|arm\|mode:stereo\|mode:stems\|bits:16\|24\|name:PREFIX` | host build: the recorder (Record tab; `rec` answers `@{"t":"rec",...}`), docs/recorder-mini.md |
| `mem` / `plugins` / `plugin=rescan\|del:ID\|set:U:ID\|clr:U` / `df` | the web console's (teensy/voiceloop/webui.h): JSON replies as `@{...}` lines, docs/webui-protocol.md |

The page is in tabs (Status, Sound, USB, User machines, Files, Log & console, About): `#files` etc. link to one,
and the last one is remembered. The 2-second stats lines are hidden in the log unless "periodic stats" is ticked.

A folder dropped on the card (or picked with "a folder") is uploaded whole: its folders made first (one that
exists is fine), then each file in order (link.js `putTree`), one row with the overall bar and the current file's
percentage, one Cancel for the lot. `.DS_Store`, `._*` and similar junk are left out.

The card browser and package installs use `'Y'` frames (CRC-32 per frame and per file, resume, abort); they do not
switch the host build's log to framed. `@` reply lines stay out of the log unless "replies" is ticked.
In `?mock`, `mockTransport(true)` plays a running transport (writes wait, the page shows "paused").

The status line is parsed from the 2-second `cpu ...`, `usb host: ob ...` and `midi notes ...` lines.

The log handles both stream shapes: plain text lines (the host build) and the framed `'L'`/`'X'`/`'A'`
protocol (the serial build, or the host build after a development tool sent a frame); SysEx and audio
frames are skipped. The page never sends frame letters, so the host build stays plain.

The USB section's Audio mode is a segmented toggle (seg.js, its own 15 s Pending kept over the disconnect): a click
sends `usbmode=...` and shows the choice pending ("re-enumerating…"); once the page has reconnected (same vendor and
product ID in every mode) it asks `usbmode`, and the answer confirms it (or reverts it). The Sidecar Mini's stereo
build answers "multi not in this build" and the Multi choice is hidden. The Computer row reads the `uac2:` stats line's
alt settings: "audio open: recording / playing" while an app streams, "idle" while none has the interface open.
USB disk mode is a disabled button until the firmware has it (docs/webui-protocol.md, Disk mode).

## Test

```
node web/stream.test.mjs
node web/webui.test.mjs     # the protocol against the mock device: plugins, memory, card, transfers, resume, abort
node web/fxin.test.mjs      # the Sound tab's IN L/R source switch (?mock&oldrytm: a Rytm without 0030-fx-in)
node web/rec.test.mjs       # the Record tab's model and the mock recorder (?mock&psram: a Teensy with PSRAM, stems)
node web/lfo.test.mjs       # the LFO tab (lfo.js, shared with the Pi's page): tables, the design, drawing, the mock
```

Files: app.js (connection, log, controls), stream.js (log parser), proto.js (frames, CRC, replies), link.js
(requests and transfers), rmp.js (package checks), manage.js (user machines, memory, card views), mockdev.js /
mock.js (the fake device).
