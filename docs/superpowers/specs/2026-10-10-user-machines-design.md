# User machines and the companion installer - umbrella design (2026-10-10)

Status: design agreed in conversation with the user, 2026-10-10; not built.
Related: docs/PLAN.md items 5, 7-9; rytm-mods 0021 (machine slots, names and
labels from the host), docs/psram.md, sim/protocol.py.

## Goal

Machines that run on the Teensy and that users choose: installed, removed and
assigned per project, loaded into the Teensy's memory only while a project uses
them, managed from a web companion over the Teensy's own USB. Inspired by
Machinedrum X.OS's USR machines (a third-party OS; Elektron allows it), but the
machines run on the Teensy, so their range is not limited by the Rytm's CPU or
code caves. Staged: our own curated catalogue first (Noise Plethora, Braids, ...),
on the same format that later opens to third parties through an SDK.

Non-goals: running user machines on the Rytm's CPU; distributing Elektron firmware
or content; a plugin store with accounts or payments.

## Requirements

- **Everything works on a stock Teensy 4.1 without PSRAM.** PSRAM only adds
  headroom. As built, a plugin's per-voice state lives in the voice's own 16 KB
  engine scratch, so a plugin keeps `state_bytes` <= 16 KB to run on every board;
  larger states load only where PSRAM is fitted (the device-info report, section
  3, says which). No machine, loader or companion feature may assume PSRAM.
- Installed machines load from flash without an SD card, but as built the per-project slot
  maps live on the card (`/.rytm/slots`), so without a card no slots are assigned and nothing
  plays; mirroring the slot maps into the flash store is later work. The card also adds the
  library (section 1, machine repository).

## 1. Architecture

### Teensy (core firmware)
- **Package store (flash, the active set)**: installed machines as files in a
  LittleFS in the Teensy's program flash (~7.8 MB free beside the firmware),
  `/machines/<id>-<version>.rmp`. Install, remove, list. Works without an SD
  card. As built: a 4 MB LittleFS (1 MB until 2026-10-10), `/machines/<id>.rmp`
  (64 KB cap), chunked install verified before an atomic rename. The store is a
  **cache** of the installed/active set; flash is kept (not loading from the card
  only) so machines work without a card and plugin loads stay off the card
  bandwidth that DSP SAMPLE streaming uses.
- **Machine repository (SD card, the whole library)**: `/MACHINES/<id>-<version>.rmp`
  holds all the user's machines and versions, written by the companion at card
  speed (flash holds only what is in use). When a project needs a machine that
  is on the card but not in flash, it is copied card -> flash (CRC verified,
  signature checked) and loaded, as part of the project-load activation the
  user confirms on the Rytm (section 2). When flash is full the
  least-recently-used packages not used by the current project are evicted; they
  stay on the card. A flash write stalls the Teensy ~10-20 ms per package
  (measured), so copies happen with the transport stopped (project load), never
  mid-pattern. The loader is source-agnostic (it streams through a read
  callback), so loading straight from the card without a flash copy remains
  possible later.
- **Package format (.rmp)**: header (magic, format version, plugin API version,
  machine id, version, compatible-with range, sizes), manifest (display name up
  to 9 characters, short name, the parameter pages and knobs and the content
  kinds - both below; CPU per voice; state bytes per voice; licence string), code
  (Thumb-2, position independent, entry table offsets, relocation list for the
  few absolute words), checksum, optional Ed25519 signature (not in sub-project 1).
  A knob of look `list` carries enum names (`rmp_knob` 22 B with
  `enum_first`/`enum_count`, a name table of 8-byte entries; format 2, the
  loader accepts 1 and 2).
- **Loader**: verifies a package (CRC before placement), copies its code into a
  reserved **code arena**, applies relocations, allocates per-voice state, and
  returns the entry table `init`, `note_on`, `note_off`,
  `params`, `render`, plus the optional `parse` and `load_entry` (presets,
  below). Unloading frees both. As built (measured, docs/plugins.md): the arena
  is 32 KB in DTCM, 8 KB-aligned, executable through MPU regions 13/14 with
  subregions (2-4 machines at once); per-voice state lives in the voice's own 16 KB
  engine scratch (so `state_bytes` <= 16 KB runs on every board, no PSRAM
  needed; larger states only with PSRAM, from rmp_state's PSRAM pool; the earlier
  24 KB OCRAM pool is gone). Packages are streamed from the store (one open
  handle per package, nothing held in RAM), and slot loads run incrementally in
  loop passes of <= ~3 ms.
- **Host API**: one versioned table handed to `init`: sample rate, the largest block
  size, logging and a random-number function (as built; sine/exp/tanh tables and a state
  allocator were not needed: plugins bring their own maths, the host owns the per-voice
  state). Plugins see nothing else of the firmware.
- **Slot manager**: user slots U1..U5 -> `id@version`, stored per project in
  `/.rytm/slots/<PROJECT>.txt` (on the SD card, beside the pool and the JUNO/FM
  lists). Only machines assigned to slots the current project uses are loaded.
- **Voice glue**: a voice whose report carries a user-slot flag calls the loaded
  plugin, as voiceloop calls PLAITS or JUNO today.

### Parameter pages (manifest)
- **Page 1, SRC**: 8 knobs, the sound's machine words (P1..P8).
- **Page 2, SMPL**: the manifest chooses, per machine:
  - `sample` - stock's SMPL page and the Rytm's sample layer kept (as DIGIDRUM:
    a sample under the machine);
  - `params` - 8 more knobs (P9..P16, the sound's sample words 17..24, so they are
    stored, p-locked and LFO'd like stock's sample parameters; the sample layer
    muted), with its own page title - as PLAITS PAGE 2, DAISYSP's EXCITER and DSP
    SAMPLE's SD SAMPLE do today (0021's page-2 lookups), e.g. 6-OP FM's operator
    or envelope controls.
- FLTR, AMP and LFO stay the Rytm's own (the voice's analog chain).
- Per knob: long and short label, range, default, and a value-text kind:
  `number`, `bipolar`, `enum` (up to 16 names from the manifest, in equal zones as
  ENG today), `list` (names from a per-project list, below - as PAT), `note`, `time`.
- Labels may depend on another knob's value (as DIGIDRUM's engine-dependent
  labels and DSP SAMPLE's MOD): a manifest table `label[knob][zone of knob K]`.

### Presets and files (manifest + host API)
- A machine declares **content kinds**, each: a folder on the SD card
  (`/MACHINES/<id>/<kind>/`, e.g. `presets`, `banks`, `samples`, `wavetables`),
  file extensions, and whether files hold one entry or several (a bank: shown as
  a folder of its entries, as JUNO banks and DX7 banks are now).
- The plugin provides `parse(kind, file bytes) -> entries {name, bytes}` (run on
  the Teensy at listing / load time, never in the audio path), so new formats
  need no core change.
- **Browsing**: each kind appears in the Rytm's file browser under TEENSY >
  <machine name> > <kind> (0023's listing, generalised from the JUNO and DX7
  folders); LOAD TO PROJ adds the entry to the project's list for that machine
  and kind (`/.rytm/lists/<PROJECT>/<id>.<kind>`: name + entry bytes or a
  reference + hash, as the FM list stores bank MD5 + voice).
- **Selecting**: a knob of kind `list` picks an entry of its list (names pushed to
  the Rytm as today's 16/17, generalised to one message `list names <slot>
  <kind> ...`); the host hands the entry's bytes to the plugin via
  `load_entry(kind, bytes)` between blocks.
- **Samples**: a plugin may declare that it uses the project's sample pool; the
  host API then gives read access to pool slots (from PSRAM when loaded, as DSP
  SAMPLE's modes) - e.g. a granular or wavetable machine on the user's own
  samples.
- The built-in JUNO and FM lists and their 16/17 messages become instances of
  this mechanism (a later refactor; kept compatible).

### Rytm (rytm-mods, 0021-style)
- Up to five user slots in the machine list: the hidden "2" variants (machines
  35..39, 8 records each) taken over as 0021 took 40..42; 0014 stops listing
  them. Each has its own voice flag.
- Each slot's machine name and knob labels are filled in at runtime from the
  Teensy (as DIGIDRUM's labels and the JUNO/FM name lists today); a slot whose
  machine is missing or muted shows `!NAME`.
- A YES/NO dialog for project switches (section 2; it also covers the card ->
  flash copy of missing machines) - research item: the
  firmware's own confirm dialog (as the slot browser's "CONFIRM? Y/N").

### Web companion
- A static page (e.g. GitHub Pages), Chrome/Edge: WebMIDI for control,
  WebSerial for bulk files, WebHID for Teensy core firmware updates. The Teensy
  enumerates as a composite USB device (MIDI + serial) on its own device port.
- Later: the SDK (API header, linker script, packager, desktop test harness).
  SDK rules: `state_bytes` <= 16 KB (the voice's engine scratch) so the machine
  runs on a board without PSRAM; a larger state is declared as PSRAM-only and
  the installer marks it as not loadable on boards without; no assumption that
  PSRAM or an SD card is present (host API capability queries, section 1b); the
  packager rejects a manifest whose declared sizes exceed the arena or the
  limits above, and reports image, bss, state and `cpu_permille`.

## 1b. What the plugin API must cover - from the six built-in machines

Inventory of PLAITS, JUNO, DAISYSP, DSP SAMPLE, DIGIDRUM and 6-OP FM as built
(2026-10-10; voiceloop.ino, their headers, rytm-mods 0021 ext.c). Each item
names the machine that needs it; the API is shaped so all six could be plugins.

**Parameters (manifest, per knob)**
- page (SRC P1..P8 / SMPL P9..P16), long/short label, look (dial, centred,
  fader, list), value text (number, bipolar, enum names in zones, list names,
  note, time); the SMPL page's title and knob order (PLAITS PAGE 2, EXCITER, SD
  SAMPLE).
- decode: zones `min((v*N)>>7, N-1)` (every ENG), absolute 0..127, offset
  around 64 (FM TIM/MOR), semitones (TUN), with or without the 7-bit fraction
  (DIGIDRUM drops it, PLAITS/JUNO use it).
- when applied: continuous, at trig (TUN, LEV), or latched until the next trig
  (FM's PAT: a load would cut the sounding note).
- labels depending on another knob's zone (DIGIDRUM ENG -> knobs 2..8, DSP
  SAMPLE MOD -> STA END LOP).
- the Rytm's sample layer: muted (most), kept (DIGIDRUM), kept for one zone of a
  knob (DAISYSP INP = NOISE).

**Events**
- trig: integer note, velocity, the full P snapshot (p-locks land on the
  step); param update (at most every 16 blocks per voice); note-off.
- a host gate helper: LEN 127 = hold until note-off, lower = timed gate
  20 ms x 2^(LEN/14), whichever first; retrigger on a held gate = a one-block
  edge (JUNO, FM).
- "trig waits for content": a trig may be held until a sample or patch is ready
  (DSP SAMPLE, measured worst 8.5 ms).

**Audio**
- output: mono to the voice channel (all), plus a MAIN L/R stereo bus (DAISYSP
  OUT=MAIN with WID); one gain convention (x0.7, soft clip where needed).
- inputs: any voice's Overbridge output by track (DAISYSP INP BD..CY/CB), the
  voice's own output with a host-provided loop canceller for feedback (DAISYSP
  OUT=VOICE, DSP SAMPLE FB), optionally AUDIO IN. (Today implemented in the sim
  only: the Teensy core must read the Rytm's per-voice return channels first.)
- a native Rytm exciter: the plugin declares a knob whose zones select "the
  track's own machine" / "UT NOISE" played on the voice by the Rytm
  (DAISYSP INP NAT/NOISE, 0021's dsp_excite) - Rytm-side code that the user
  slots provide generically, driven by the manifest; the plugin then hears its
  own voice's output as input.

**Content**
- content kinds and `parse` (section 1 above); per-project lists with files
  under `/.rytm/` (pool, JUNO list, FM list = bank MD5 + voice + name);
  indexes built at boot and cached (`/.rytm/dx7.bin`, the /JUNO walk); read-only
  data shipped inside the package and run from flash (Plaits' wavetables and
  ROM banks, the JUNO factory patches); `list_changed` re-selecting voices.
- the pool and PSRAM: pool slots by number, served from PSRAM when resident
  (LRU, pinned while a voice plays one) or streamed from the card (DSP SAMPLE).

**Messages to the Rytm**
- names lists (one generic "list names" message: replace flag, total, chunks of
  13-character names), asked for by the Rytm when its cache is stale.
- knob write-back: the plugin sets a group of its knobs on the Rytm (JUNO's 15:
  a patch's sliders), with the host's hold window until the reports carry them.
  Preset switching, generalised from JUNO:
  - the manifest marks each knob `preset` (its value comes from the preset
    and is written back when the preset changes: JUNO's CUT..REL), `offset`
    (stays where it is, applied on top: FM's TIM/MOR, 64 = as stored) or `free`
    (not touched by presets: LEN, LEV); knobs on both pages may be `preset`.
  - when a `list` knob (PAT) picks another entry through a knob turn (a 0x03
    report) or when the voice first becomes this machine, the plugin's
    `load_entry` returns the entry's values for its `preset` knobs; the host
    sends them as one generic message `knobs <voice> <n> {<knob 0..15>
    <value 0..127>}` and holds them over the incoming reports until the reports
    carry them (300 ms at most, as JUNO now).
  - a p-locked PAT on a step (a 0x04 trig) plays that entry but writes nothing
    back - a per-step lock must not rewrite the sound's knobs (as JUNO now).
  - Rytm side (generic for every user slot, replacing 0021's JUNO-only 15): in
    the UI task, after re-checking the track still has that machine, write the
    kit's word (machine words 0..7 at 0x41255933 + 152 t + 2 k, measured; the
    SMPL page's sample words: their kit addresses to be measured the same way)
    and the live copy through 0x4011f3fa (value, track, sound index), clamped to
    the record's range.

**Resources and scheduling**
- per-voice state size and placement hint (fast RAM / RAM2 / PSRAM), shared
  per-machine state (FM's reload stage, DSP SAMPLE's 32-grain budget, tables).
- the host scheduler: card opens/reads and list loads queued as jobs and run
  between render requests with priorities (a stream about to run dry first,
  then sample prepares, then PSRAM loads); never in render or the report path
  (today the project change and 0F still run inline - to move into jobs).
- per-plugin CPU accounting (the existing per-machine log line) and the time
  budget of section 4.

**Lifecycle**
- init (card ready), voice enter/leave (FM reloads the ROM bank on leave, DSP
  SAMPLE releases its pin), project change, list change; capability queries
  (PSRAM present, USB-host or bridge mode).

Not carried over: sharing one engine object between two machines (PLAITS and FM
share a plaits::Voice, DAISYSP and DSP SAMPLE a DaisyEngine, to save RAM) - with
loading on demand each plugin owns its state.

## 2. Data flow

Machines are identified by a stable id and version (`befaco.noiseplethora@1.2`),
never by slot or display name.

- **Install**: chunked SysEx with a checksum per chunk and an ack; the Teensy
  verifies and stores the package (temporary file, then an atomic rename); a
  broken transfer leaves the old version in place. Installing activates nothing.
  **Library**: the companion writes packages to `/MACHINES` on the SD card (card
  speed, WebSerial bulk) - all machines and versions; flash holds the active
  set and is filled from the card on demand (below).
- **Assign**: in the companion (later also on the Rytm) "U2 = BRAIDS": the
  current project's slot map is saved at once (as the pool), the machine is
  loaded, names and labels go to the Rytm.
- **Project change** (the Rytm's `10 <project>`): the Teensy reads the project's
  slot map and compares it with what is active:
  - same set, all installed: silent, loaded in milliseconds;
  - a different set: the Rytm asks "PROJECT USES U1 NOISE PL, U3 BRAIDS -
    ACTIVATE?". YES (default) loads the project's set; NO keeps the current one
    and the project's mismatched slots stay silent, shown `!NAME` - never another
    machine playing with the wrong parameters;
  - a machine not in flash but on the card (`/MACHINES`): part of the same
    activation prompt ("... U3 BRAIDS (FROM CARD) - ACTIVATE?"). YES copies it
    card -> flash (verified, signature checked; the least-recently-used packages
    are evicted if flash is full) with the transport stopped, then loads it; the
    card copy stays. NO leaves the slot silent, `!BRAIDS`;
  - a machine in neither: "BRAIDS 1.0 NOT INSTALLED", the slot silent and
    `!BRAIDS`; an install from the companion is picked up at once;
  - another version installed: used silently when its manifest declares the
    saved version compatible, else the same warning (both versions may be kept);
  - no slot map (new or older project): empty; the first assignment creates it;
  - the code arena too small for the project's set, or a machine whose state
    exceeds the board's per-voice limit (16 KB without PSRAM): the extra slots
    silent, with a warning.
  Without a working dialog on the Rytm (research item) the fallback is the
  status in the slot names plus activation from a menu.
- **Play**: trigs and P values on user slots arrive as 0x04/0x03 reports with
  the slot's flag, note-offs as 0x11, like every machine now.

## 3. What the companion manages

1. **Content** (data on the SD card, never code): samples - imported and
   converted in the browser (resample, mono/stereo, trim, normalise) -, JUNO
   .syx patches and banks, DX7 banks, later plugin presets; organised as folders
   and added straight to a project's pool, JUNO list or FM list. Bulk transfer
   over WebSerial (several MB/s); pulling the card stays possible.
2. **Machines**: the packages above - write them to the card library
   (`/MACHINES`), install to or remove from flash, assign per project. Flash
   shows as the active set, the card as the library; a package can be put on the
   card only, in flash only, or both.
3. **Device-info report** (so the companion/web installer knows what will run).
   The Teensy answers a query with: core firmware version, plugin API version,
   package format versions accepted, PSRAM size (0 = none), per-voice state
   limit (16384 without PSRAM), code arena size and use per project/slot, store
   size and free and the installed packages (id, version, size), the SD
   repository's packages if a card is present, and per installed machine its
   declared `cpu_permille` plus measured average and peak CPU. The installer
   combines it with each package's header and manifest (image + bss, state_bytes,
   cpu_permille; the manifest should also carry state and code sizes for the UI -
   they are in the header today and the installer can read both) to show per
   machine and per project: **fits**, **won't load here** (arena too small,
   state too big without PSRAM), or **fits but heavy** (CPU estimate at 8 voices,
   e.g. NP phasingCluster ~80 %). The CPU figure is an estimate: it depends on
   what else the project plays.
4. **Platform features** (looper, streaming, PSRAM playback modes, later the
   mixer) - not plugins:
   - Teensy core firmware always contains them; each has a runtime switch and
     settings stored on the Teensy and set in the companion. The companion also
     updates the core firmware (Teensy bootloader over WebHID).
   - Rytm-side features (looper keys, MOD, machine slots) are chosen when the
     user's own OS file is patched: the companion patches it in the browser,
     checks it against a reference build and hands out the .syx for Elektron
     Transfer (as Modded Cycles does; no firmware distributed). Each new Rytm build
     goes through the staged bring-up (rytm-mods docs/FLASHING.md).
   - Versions: Rytm mods, Teensy core and plugin API each carry one; Rytm and
     Teensy exchange protocol versions at boot and warn on a mismatch.

## 4. Safety, errors, testing

- **Load checks**: format, API version, checksum, declared sizes (code + bss
  against the arena, `state_bytes` against the board's per-voice limit) and CPU;
  the same checks run on the card -> flash copy;
  curated packages signed (Ed25519); unsigned packages only with "allow unsigned
  machines" set in the companion.
- **Memory fence**: sub-project 1 catches faults (any fault class) and overruns
  inside plugin calls and does not fence writes: curated plugins are trusted.
  The guard returns to the call site through a longjmp trampoline; a 2 kHz timer
  pends NVIC IRQ 71 for endless loops (timeouts in thread mode only). A fault or
  overrun mutes the slot (and the slots sharing its image), the Teensy runs on.
  Write-fencing (unprivileged plugins on their own stack, MPU regions for their
  state, `!NAME` reporting) comes with the open SDK, sub-project 5. Plugins reach
  the hardware only through the host API.
- **Time budget**: each render timed; three overruns in a row (default 2 x the
  declared CPU) mute the slot and warn; the other voices keep real time.
- **Watchdog and safe mode**: a hang restarts the Teensy (RTWDOG, 2 s in
  voiceloop; a slot load runs in passes of at most ~4 ms: the store's file open is the longest step) with user machines disabled for that boot (also on a key held at power-on): a bad plugin
  cannot cause a crash loop.
- **The Rytm stays safe**: its side only shows names and labels and sends
  reports; a plugin can give silence or bad audio on its voice, never a Rytm
  crash. The Rytm-side safety work (USB pool guard, strict-mode and power-off
  tests) applies to the user slots unchanged.
- **Tests**: plugins on the desktop harness (the simulator's way) with tests per
  plugin (pitch, ranges, no NaN/denormals); the loader against bad packages
  (truncated, wrong version or signature, out-of-bounds write, endless loop);
  stress.py scenarios with user machines (8 voices, project switches with other
  sets, loads mid-pattern); user slots, dialogs and power cycles in rytm-emu; the
  companion against the simulator as a virtual MIDI device first.

## 5. Sub-projects, in order

Each gets its own spec and plan:

1. Plugin loader and package format on the Teensy (store, loader, host API,
   MPU/time budget/watchdog, packager tool), with one curated machine (Noise
   Plethora, licence CC BY-NC-SA 3.0 / plugins CC BY-SA 4.0: a separate,
   attributed component) packaged and played through it.
2. User slots on the Rytm (machines 35..39, runtime names and labels, the
   project-switch dialog).
3. The web companion: machines first, then content, then features.
4. The in-browser OS patcher.
5. The SDK for third-party machines.
