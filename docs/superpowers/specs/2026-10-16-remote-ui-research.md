# Remote UI: the Rytm as a terminal for Sidecar-drawn pages (research, 2026-10-16)

BACKLOG rows: "Remote UI", "Sidecar Manager", "Custom UI elements loaded from the expander", "Visual EQ";
the page-2 idea of `2026-10-16-sidecar-send-fx-research.md` section 3.

Method: Analog Rytm MKII **OS 1.73** stock, static disassembly of MAIN OS (`fwinfo` sections, `m68k-elf-objdump
-m m68k:cfv4e`, nothing committed) and rytm-emu (`dtboot` with `--count`/`--break`, 200 M instructions = 1 emulated
second), plus rytm-mods (`re/symbols.toml` on port-1.74, mods 0021/0022/0023 on efm-173) and the third-party
rytm-fw-re notes (github.com/ethernet8023/rytm-fw-re, OS 1.74, read only). Tags: **[measured]** run in rytm-emu,
**[code]** read in the 1.73 disassembly, **[inferred]** reasoned, **[external]** from rytm-fw-re or rytm-mods
notes not re-derived here. Addresses are 1.73 unless marked; 1.74 = 1.73 + 0x6c for the panel/display code
above 0x40080000 (rytm-emu `symbols.rs`, and rytm-fw-re's 1.74 addresses below line up exactly with that).

## Summary

- **Feasible, and small.** The stock UI already does what a remote page needs: one render function in the UI
  task draws the whole view stack into a 128x64 1-bit canvas 20 times a second, and a double-buffer diff sends
  only the changed 8x8 tiles to the panel MCU. A mod that, while a "Sidecar page" is active, replaces the view
  draw with a 1 KB memcpy from a shadow buffer gets dirty-rect transport, tearing-free double buffering and
  a clean hand-back for free: leave the mode, mark dirty, and the stock diff repaints only what differs.
- **Bottleneck is the panel link, not USB.** UART8 to the panel MCU runs at 156 250 baud (about 15.9 KB/s):
  a full-screen change costs 1281 B = ~81 ms on the wire (~12 fps); typical small changes (a meter, a knob)
  are a few tiles (~2-5 ms) and run at the UI's 20 fps. USB MIDI (High Speed) and the Rytm's SysEx input
  (1.6 MB/s measured) are two orders of magnitude above that.
- **Latency is set by the 20 Hz render tick**: encoder -> Sidecar -> pixels on glass about 30-70 ms typical,
  ~100 ms worst; the remote path adds only ~2-5 ms to what the stock UI itself takes. The backlog's 5-10 ms
  is not reachable for pixels without changing the tick (option below).
- **Recommended design:** a mod (working name `0026-remote-ui`) on top of 0022's SysEx router: 2 patches in the
  render function, 3 input taps (key/encoder/pad, as 0022 does), commands `0x30..0x3F` through a 'RUIP'
  mailbox, a 1 KB shadow (+1 KB staging) from the firmware heap, enter by a track-menu item ("SIDECAR") or a key
  combo, leave by the Sidecar, by a local escape, or by a 1 s timeout. Code ~700-900 B, which does **not** fit
  the caves left in a 0021+0022+0023 image: either drop an excluded mod or build the +Drive loader first.

## 1. The display

**Panel, resolution, pixel format.**
- 128x64, 1 bit per pixel, behind the panel MCU on UART8 (`0xEC070000`) [measured: rytm-emu `src/panel.rs`
  models the MCU and the OS draws correctly; the .syx carries a PANEL_MCU section (id 8, 159 948 B) that the OS
  re-flashes over this link when versions differ, `0x4013fe9c`, docs/RYTM-PORT.md]. rytm-fw-re calls the same
  registers an "FPGA byte-FIFO bridge" [external]; the register use is identical, the MCU reading fits the
  evidence better (its own firmware section, its bootloader protocol, key/encoder/pad messages coming back).
- Panel commands [code + measured, `panel.rs`]: `1p cc d0..d7` one tile (page p 0..7, columns cc..cc+7, 10 B),
  `B8` end of frame, `B7 x` contrast; LEDs `2g/3g` selectors, `B0..B4` slots, `B5` palette.
- **Framebuffer layout** (two 1024 B planes, pointers `fb_front` `0x40287808` / `fb_back` `0x4028780c`
  [code, profile `rytm_173`]): byte = page + 8 x column; on glass pixel (x, y from the top) is byte
  `(7 - y/8) + 8x`, bit `y % 8` [measured: rytm-emu `Machine::frame_pixels`, the main screen reads upright
  only so]. The canvas sees the same bytes as big-endian words, two per column, `bit 31 - (y & 31)` of word
  `y >> 5` (`fill_rect`, rytm-mods symbols [external, verified there]) with **y growing upward** (knob rows
  at y = 46 top and 24 bottom, `knob_draw` notes). rytm-fw-re's "MSB = top" is the same layout with y counted
  the other way. The remote protocol should use these bytes as-is so the blit is a memcpy.

**Who draws and flushes** [code]:
- `0x400a2aaa` UI render (UI task, entry `0x400a3c4c`, prio 6): if the root ViewManager (`ui + 32`) is dirty
  (`0x40079050` reads `vm + 32`; `0x4007905a` sets it):
  1. `canvas(128, 64, pixels = *0x41943d64)` (`0x4006f9c2`),
  2. `ViewManager::draw(vm, canvas)` `0x40079086`: under the manager's lock, clear the canvas, call every view's
     `vtable + 0x10` (draw) bottom to top, clear dirty,
  3. `panel_diff` `0x400959f2`, store its return (the next draw buffer) in `0x41943d64`, destroy the canvas.
- `panel_diff` `0x400959f2`: for each of the 128 tiles compares `*fb_front` (just drawn) with `*fb_back` (last
  sent) and sends a whole tile through `0x40095924` (`0x10|page`, column, 8 bytes; UART lock `0x40001bf6` /
  `0x40001c06`, byte writer `0x40001bb2`, block writer `0x400019e4`) when any byte differs; then `B8`
  (`0x4009590c`), then swaps the pointers and returns the new front. Full redraw `0x40095974`, clear
  `0x40095a8e`. (1.74: `0x40095a5e`, `0x40095990`, `0x40095978` in rytm-fw-re [external] = 1.73 + 0x6c.)
- So the drawn buffer always starts two frames old and every view redraws everything: a mod that writes the
  canvas must fill all 1024 B each time (a memcpy), not patch it.
- Other `panel_diff` callers: the boot/progress task `0x4009572a` (pends on `display_sem` `0x4191b6e8`) and
  `0x40101f76` / `0x401021a8` (another task's own screens) [code]. A remote page only touches the UI render.

**Link speed** [code]: UART8 init `0x40001c16(156250)` from `0x4010071c`; divisor `132 MHz / (32 x baud)`
written to UBG2 (26 -> ~158.6 kbaud, 8N1 ~15.9 KB/s). TX is buffered (a 1024 B software ring, writers spin while
more than 1023 B are pending, `0x40001b20`; rytm-fw-re reports a 4 KB DMA ring on the same channel
[external]). rytm-fw-re also derives 156 250 from `0x2625a` [external, agrees].

**Cadence** [measured, stock 1.73 main screen, `--count`]: `0x400a2aaa` runs exactly 20 times per emulated
second, idle or busy; `panel_diff` only when something changed (5 in 2 s idle, ~13/s while turning an encoder,
85 tiles in those 2 s). Idle screens send nothing.

**Owning the screen.** Two ways:
- *A real modal View* pushed on the ViewManager stack: draws last (on top), sees keys first; popups do this
  (`view_show_popup` `0x40079462`, `popup_ctor` `0x40076754`, 1.74 [external, verified in rytm-mods]). Needs a
  C++ view object and vtable (slots per rytm-fw-re: +0x08 key, +0x10 render, +0x50/+0x54 dtors [external])
  built in cave code; more bytes, more RE, and every view below still draws underneath.
- *Intercept the render* (recommended): patch the `jsr 0x40079086` at `0x400a2c56` (6 B) to `rui_draw(vm,
  canvas)`: inactive -> tail-call the stock draw; active -> (optionally call the stock draw for a hybrid page)
  copy the shadow (or a rectangle of it) into `canvas + 16`'s pixels, clear `vm + 32`. And patch the dirty
  test `jsr 0x40079050` at `0x400a2bee` (6 B) to `rui_dirty(vm)`: stock dirty, or (active and the shadow's
  sequence moved), plus the 20 Hz timeout counter. Everything runs in the UI task; no other task writes the
  canvas or the ViewManager. Stock views keep their state and resume as if nothing happened.
- *Hybrid pages* (send-FX page 2, a visual EQ under the stock header): call the stock draw, then blit only a
  rectangle (e.g. tile rows below the title bar).

## 2. Input

**Where it enters** [code + measured]:
- Panel MCU -> UART8 RX -> parser `0x40082084` (1.73; installed at `0x40100714`): keys as group masks
  (`2g mask`), encoders `3e delta`, pads `Ap force` / `Bp` / `9p pressure` (docs/RYTM-PORT.md). Events reach the
  UI task's event switch (`0x400a3c4c` loop), which hands each type to one dispatcher on the UI object:
  | dispatcher | event | then | evidence |
  |---|---|---|---|
  | `0x400a02ca` key_dispatch | key (code +12, flags +16 bit 0 down) | handler list `ui+1784`, then `0x40079a42` | measured (0022), counted on UP and BD |
  | `0x400a0364` | encoder (copied: +4 byte, +8, +12, +16, +20 byte; FUNC state from `is_key_held` `0x40081bbc`) | `0x4007991a` | counted on `--turn` [measured]; field meanings [inferred] |
  | `0x400a032c` | pad (velocity clamped to 127) | `0x40079be2` | counted on BD [measured] |
  | `0x400a0348` | pad pressure (value << 7) | `0x40079854` | [inferred] |
  All four clear `ui + 1864` (idle counter) and bail when `ui + 32` is null.
- Pads also play before any of this in the pad handler `0x400c7854` (sound, MIDI note, live record); 0022
  measured that taking only the key event leaves the pad sounding.
- Latency into the UI task [measured]: key and pad dispatched within 5 ms of the panel message; encoders 1-63
  ms in eight probes (deliveries quantised, cause not pinned: the OS's encoder accumulation or the emulator's
  panel pacing; check on hardware).

**Routing to the Sidecar** (the 0022 pattern, measured working there): while active,
- keys: the call at `0x400a47ce` -> `rui_key` (0022's `looper_key` site: chain, don't double-patch);
- encoders: a detour at `0x400a0364`'s entry;
- pads: `0x400c7854` (0022's `looper_pad` site, same chain) plus `0x400a032c`/`0x400a0348` for pressure;
- each event becomes an 8-10 B SysEx to the host (`MIDI_LOCK` `0x40002914`, `USB_MIDI_SEND` `0x400029e0`,
  `MIDI_UNLOCK` `0x40002924`, after 0022's `usb_free(4)` check on the request pool `0x413db390`), and is
  consumed. Encoder turns are coalesced per tick (one message per encoder per 50 ms with the summed delta).
- One local escape is never forwarded (e.g. NO held 1 s, or FUNC + NO) so a hung Sidecar can't trap the user.

**LEDs** [measured in 0022]: own the trig/pad LEDs at the panel link: drop the firmware's `B0..B4` for the
LEDs in use at the UART writer `0x400019e4` and their selector groups at the callback `0x40287a10`, paint with
`B4 id c c c c`; on leaving re-send the cached slot colours (`0x42fb6f49 + 4 x logical LED`) and last selectors
(`0x41920534`). Key -> LED map: rytm-emu `panel::RYTM_LEDS` (trig k = LED k-1, pads 72..83). A host message
`LED <led> <palette>` maps straight onto `B4`.

## 3. Transport

**Costs** (F0 7D 52 54 header, 7-bit packing 7 bytes -> 8):
| update | payload | SysEx | USB-MIDI wire (4 B per 3) | panel UART |
|---|---|---|---|---|
| full frame | 1024 B | ~1185 B | ~1580 B | 1281 B, ~81 ms |
| one 8x8 tile | 8 B | ~20 B | ~28 B | 10 B, 0.63 ms |
| 32x16 meter (8 tiles) | 64 B | ~85 B | ~115 B | 80 B, 5 ms |
| encoder/key report | - | 8-10 B | 12-16 B | - |

- USB: the MKII is High Speed [external: research-rytm-usb.md]; even FS bulk (~1 MB/s) would carry 60 full frames
  a second. The OS takes 1.6 MB/s of unknown SysEx at ~1 instruction per byte, 4000 B messages included
  [measured, measure-emulator.md] (USB controller not modelled).
- **Target:** 20 fps for partial updates (the tick), full-screen animation ~10-12 fps (UART). The Sidecar
  should diff on its side too and send only changed tiles; the Rytm's own diff then never resends unchanged
  ones. The panel link is shared with LED traffic.
- **Dirty-rect protocol** (tile-aligned, matches the framebuffer):
  - `38 <seq> <col0 0..15> <page0 0..7> <ncols 1..16> <npages 1..8> <packed bytes>`: raw framebuffer
    bytes for that rectangle, column-major (8 x ncols x npages bytes), into the **staging** buffer.
  - `39 <seq>`: commit: the MIDI task copies staging -> shadow under a seqlock (odd while copying), the UI task
    blits when even (else retries next tick). A full frame is one `38 0 0 16 8` + `39`.
  - `3A <page id> <on/off> [<flags: hybrid, rect>]`: host opens/closes a page (e.g. after the Rytm's request).
  - `3B <led> <palette>`, `3C` keepalive (any 38..3C also counts).
  - Rytm -> host: `30 <page id> <on/off> <reason: user, host, timeout>`, `31 <key> <down>`,
    `32 <enc> <delta + 64> <func>`, `33 <pad> <velocity | 0>`, `34 <pad> <pressure>`.
  - `0x30..0x3F` are free (protocol.py uses up to `0x21`; 0022's router swallows `>= 0x0a` it doesn't know).
- **Carrying it:** 0022's router entry `0x40083d6c` (data, len, interface; MIDI task; USB and DIN in one task,
  R13 measured) gets a branch `0x30..0x3F` -> a mailbox at a fixed cave address with magic 'RUIP' and a
  handler pointer, exactly as `0x0E/0x14` -> 'TDRV' (`0x401b9a74`) and `0x15..` -> 'JUNO' (`0x40027a04`).
- **ISR/task safety** [inferred, same rules 0021 proved]: the handler runs in the MIDI task (not an ISR); it
  only writes staging and the seqlocked shadow. The UI task only reads the shadow and owns all canvas, view and
  LED-cache state. One writer each; no locks shared with the audio interrupt. RAM: shadow + staging 2 KB from
  the allocator `0x400829d4` on the first UI tick, zeroed, then published to the mailbox (0021's pattern);
  before that the handler drops frames.

**Round-trip estimate** (encoder -> new pixels) [inferred from the measurements above]:
UI dispatch 1-40 ms (keys/pads < 5 ms) -> SysEx out < 1 ms -> host poll + render 1-3 ms -> SysEx in < 1 ms ->
next render tick 0-50 ms (mean 25) -> tiles 1-5 ms (81 ms full frame). Typical 30-70 ms, worst ~100-130 ms;
stock UI pays the same tick, so the remote overhead is ~2-5 ms. Lowering it below the tick would mean
waking the UI loop on commit (post the semaphore the loop waits on, not yet found) - an option for later.

## 4. Entering and leaving

- **Hooks, cleanest first:**
  1. A track-menu item "SIDECAR" (FUNC + CHRO menu), appended at 0021's detour `0x400f9e9c` (the constructor's
     tail; 0012's SLICE LOOP and 0021's U1..U5 already chain there) using the SAVE TRACK item's managers as
     0021 does. YES -> `30 <manager> 1` and remote mode on (screen shows "WAITING FOR SIDECAR" from a tiny
     built-in text until the first commit). This is the "Sidecar Manager" entry the backlog asks for.
  2. A key combo in `rui_key` (0022 uses TRK + REC): quickest for the first milestone.
  3. Page 2 of DELAY / REVERB / AMP (send-FX spec section 3): the view's page toggle selects a page id >= 12;
     the page's draw turns hybrid remote mode on, leaving the page turns it off. Later, after M4 of that spec.
- **Leaving:** the host's `3A x 0`, the local escape, or a timeout clear `active`, set `vm + 32` and restore
  LEDs; the next tick redraws the stock views and the diff sends only the tiles that differ from the
  Sidecar's last frame. Pads/keys held at the switch: swallow their releases (0022's `eat_rec_up` pattern).
- **Failure:** `rui_dirty` counts 20 Hz ticks since the last host message; no message for 1 s (20 ticks;
  the host sends `3C` every 250 ms when idle) -> leave with reason timeout and show a stock popup "SIDECAR
  LOST" (popup_ctor + view_show_popup). A host that reconnects re-opens with `3A`. A partial frame (no `39`)
  is never shown. USB requests exhausted -> reports are dropped (0022's `usb_free`), the UI never blocks.

## 5. Cost

| part | patches | code (est.) |
|---|---|---|
| render: `0x400a2bee` dirty, `0x400a2c56` draw | 2 x 6 B | ~150 B |
| input: key (chain on 0022's `0x400a47ce`), encoder `0x400a0364`, pad (chain on `0x400c7854`), pressure | 1-2 new | ~250 B |
| router branch + 'RUIP' handler (38/39/3A/3B/3C, unpack 7->8) | in 0022 | ~250 B |
| LEDs (reuse 0022's UART filter / selector callback) | 0 new | ~100 B |
| enter/leave, timeout, popup, menu item | 1 (menu chain) | ~150 B |
| **total** | **~4 new patch sites** | **~700-900 B code, ~40 B state; 2 KB heap** |

- **Caves:** the registry's 24 pools total 27 814 B; mutually exclusive mods share them, and in the 0021 + 0022 +
  0023 builds the largest remaining pools are 8 B [code: `registry/allocations.toml` on efm-173]. So the remote UI
  either takes the caves of an excluded mod or needs the **+Drive code loader** (rytm-mods docs/IDEAS.md). The
  remote UI is exactly the feature that makes further Rytm-side UI unnecessary, so it is worth spending the
  space on; the loader becomes necessary if 0021, 0022, 0023 and this must all ship together.
- **Risks:** (1) the 0022 sites are shared: build the input taps into one dispatcher or chain explicitly, never two
  patches on one site. (2) Field meanings of the encoder/pressure events are inferred: measure in rytm-emu before
  hardware. (3) Full-frame animation saturates UART8 and delays LED updates; the Sidecar must diff. (4) The
  "FPGA FIFO vs MCU UART" reading and the 4 KB DMA ring (rytm-fw-re) don't change the design, but the TX
  ring's blocking behaviour under 1281 B bursts should be checked (stock full redraws already do this). (5) Never
  tested on hardware; the panel's real refresh of a tile is not modelled. (6) Port to 1.74: the panel/display code
  moves +0x6c, re-verify every address in that image.

## 6. First milestone (emulator only)

**M1: a test pattern on a remote page, encoder turns back.**
1. rytm-mods (branch off efm-173, with 0014/0021/0022): `0026-remote-ui` with the two render patches, the
   encoder/key taps, the 'RUIP' mailbox and commands 38/39/3A/3C/30/31/32; enter by a key combo, timeout 1 s.
   No LEDs, no menu item yet.
2. rytm-teensy `sim/remote_ui.py` (beside `emu_link.py`): connect to `dtboot --ob-link` (127.0.0.1:7301) with
   HELLO_MIDI, wait for `30 .. 1`, send a checkerboard + a 1-pixel border as one full frame, then a moving 8x8
   tile at 20 Hz; print each `32` it receives and draw its value as a bar.
3. Checks (headless `dtboot` with `--press`/`--turn` and the ASCII screen, or the `rytm` window): the pattern
   appears exactly (compare the 1024 B `frame` with what was sent); `--count 0x40095924` shows only the moving
   tile's tiles per tick; `--turn` deltas arrive with the right encoder and sign; killing the script returns
   the stock screen within ~1 s, with no stuck keys; after leaving, a stock page change still redraws.
4. Measure: turn -> report and commit -> `B8` latencies (clock stamps), tiles/s under full-frame streaming.

M2: LEDs and pads (0022 code reuse), the track-menu "SIDECAR" item opening the Sidecar Manager page. M3: hybrid
pages (header kept) and the send-FX page 2. Then the Teensy/Pi renderer and hardware.


## Design intent (user, 2026-10-16)

Not animation: graphics like the Syntakt and other modern Elektron boxes. Parameter pages drawn as meaningful widgets instead of plain knob values:

- a filter curve (cutoff/resonance/type), an envelope (A/D/S/R or AD), a waveform/oscillator shape, an LFO shape, a level/pan bar, an enum list, a plain knob as fallback.
- Redrawn only when a parameter changes (a few dirty tiles), so the 20 Hz tick and the UART budget are plenty.
- Primary use: the custom machines' two pages of 8 parameters (EXT machines and user-slot plugins).
- A widget library on the Sidecar (shared core, the same for Pi and Teensy); each machine/plugin declares in its configuration (the plugin manifest / the built-in machine table) how each page is presented: which widget, which parameters feed it (e.g. FILTER widget <- P5 cutoff, P6 reso, P7 type), labels. We author these for the built-in machines; plugin authors do it for theirs.
- The Rytm-side mod stays a dumb terminal (blit + input); all layout and drawing is Sidecar code.

### Community references (seen 2026-10-16, Discord)

- A public demo page on showing fast LFO modulation on a 128x64 1-bit screen (Elektron-style "Waverider" page): five display strategies for a POS control under modulation faster than the redraw rate (blur band, brackets + speed sign, a ghost wave of swept frames, a two-tier display by rate, a slowed dot - "wrong"); 50 % dither as the panel's only grey; redraw rates 24/4/1 per second. Useful principles for our widgets (dither for ranges, show modulation envelopes rather than animating). No licence stated: ideas only.
- "octadraw": a web app to design Octatrack 128x64 screens - layers, shapes, text with pixel fonts, waveform, list, multislider, scroll and zone elements, glyph/widget libraries, versions and visibility states, preview, export ("pixels in, ColdFire out"). Model for a Sidecar page designer: design widget layouts in the browser (our web console), export a layout description (JSON) that the Sidecar's widget renderer loads per machine/plugin page. Licence/availability to check before reusing anything.

## M1 status (2026-10-16, emulator only)

Built as rytm-mods `0026-remote-ui` (branch `remote-ui`, build tag `rui` = sfxui + 0026 + 0022 `rui=1`; design and
numbers in `mods/0026-remote-ui/design.md`) and `sim/remote_ui.py` (protocol ids in `sim/protocol.py`, 0x30..0x3F,
each message with `<ver 1>`; `31` carries the key code as two 7-bit bytes). Changes against sections 2-6:
- Code in elekloader's DDR area on core-ar2's events (`ev_tick`, `ev_draw`, `ev_key`, `ev_enc`): those are exactly the
  render and input sites of section 1/2, so 0026 has **no patch site**; only an 8 B mailbox (`RUIP`, 0x401b9e5c) for
  0022's router. Shadow + staging are static `.bss` in the area (no heap).
- Enter by **TRK + YES**, leave by TRK + YES, the host's `3A x 0`, or 1 s without host input. No LEDs, pads or menu.
- Reports never wait on USB: a try-lock of the MIDI mutex (0x4000119c) and the stock sender's own wait conditions
  checked first; otherwise they wait for the next tick.
- Measured (rytm-emu, strict): the panel's OLED equals the sent frame byte for byte; 2 tiles a tick for the moving
  tile; commit -> pixels <= 1 tick (max 49 ms); turn -> report mean 21 ms, max 38 ms (coalesced per tick);
  UI cost ~350-520 instructions a tick; the emulator does not limit UART8, so the ~12 full frames/s of section 1
  stay [inferred].

## Host status (2026-10-16, emulator only; rytm-teensy branch `remote-ui-host`)

The Sidecar side, in the shared core: `core/ui/` (`core/ui/README.md`: the widget library, the page format, the host).
- **Widgets:** 1-bit canvas in the Rytm's framebuffer layout (a tile is the panel's 8 bytes), two original pixel
  fonts (3x5, 5x7; CC0), header, knob, bipolar knob, level / pan bar, enum list, filter curve, ADSR / AD / AHD envelope,
  oscillator shape, LFO shape, sample waveform with slices. Static, Syntakt-style; no animation.
- **Pages** are constant data (a cell grid matching encoders A..H, the parameters each widget reads, encoder labels);
  the built-in machine table has PLAITS (model list, HRM / TIM / MOR, the LPG decay as an AD shape, LPG colour, TUNE,
  LEVEL) and a SIDECAR status page (the 8 voices' machines, FX1 / FX2 TYPE + ROUTE, Overbridge, underruns, CPU).
- **Host:** opens on `30 on`, SIDECAR first; LEFT / RIGHT (UP / DOWN / PAGE) step pages, NO back to SIDECAR; encoders
  edit the voice's P through a `0x03` into `voice_report`, held until the Rytm's own value moves. A widget redraws
  when its inputs change; a 50 ms tick sends only the changed tiles (`38` runs + `39`), `3C` after 250 ms quiet.
- **Measured** [rytm-emu, rui1017 build, `sim/rui_e2e.py`, every run DTEMU_STRICT=1]: TRK + YES -> the core's whole
  SIDECAR frame on glass byte for byte; RIGHT -> the PLAITS page; encoder B +6 -> the voice's HRM +6 in the engine,
  1-2 tiles a frame, the panel wrote exactly the 5 tiles the core sent; TRK + YES again -> the stock screen (equal to
  a run that never opened); the core killed mid-page -> the Rytm's timeout, 20 renders a second throughout, stock back.
- **Cost** [measured, Mac]: a whole page drawn + diffed 5-9 us, one change 0.3-2 us, a sending tick at most 17 us (x
  13.2 for the Pi: < 0.25 ms); Teensy host build: 0 B ITCM, +64 B RAM1, +2.9 KB RAM2, ~14.6 KB flash.
- **Open:** an edit is not written back to the Rytm's kit (the Rytm keeps its own value; a Rytm-side parameter write
  would be a 0026 / 0029 addition), more machine pages (the JSON form in the README for plugins), the LEDs and pads
  of M2, hardware.

