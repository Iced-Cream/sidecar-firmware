# User slots on the Rytm - sub-project 2 design (2026-10-10)

Status: design agreed in conversation with the user, 2026-10-10. The Teensy side (section 2) is built and tested
on the Teensy without the Rytm (2026-10-11, see "Status" at the end); the Rytm side is rytm-mods-us 0021. Where the
build differs from the first design, the text below says what is built.
Parent: `docs/superpowers/specs/2026-10-10-user-machines-design.md` (the umbrella; its
section 5 item 2). Related: docs/plugins.md (sub-project 1 as built), sim/protocol.py,
rytm-mods-efm173 mods/0021-ext (ext.c, design.md), 0014-hidden-machines, 0022-looper
(looper.c, the SysEx router), 0023-teensy-drive (the pool table), 0012-slice-loop (the
track-menu item), re/subsystems/machines.md, re/symbols.toml, registry/allocations.toml.

All Rytm addresses are **OS 1.73** (load base 0x40000400), read from the stock image
`build/stock_mainos.bin` and the registry on 2026-10-10 unless tagged otherwise.
Tags as in rytm-mods: **[measured]** (read in this image's disassembly/data or run in
rytm-emu), **[inferred]**, **to measure** (not yet looked at; the plan must measure it
before relying on it). machines.md's table is OS 1.74; its 1.73 counterparts are below.

## Goal

The five user slots U1..U5 that sub-project 1 plays on the Teensy (voice flags
0x50..0x70, docs/plugins.md) become real machines on the Rytm: listed on every track,
named after the machine the slot plays, their knobs labelled, ranged and texted from
the Teensy at runtime, a second page, presets that move the knobs, the project's sample
pool for machines that want it, and a menu on the Rytm to assign slots.

**This round:**
- user slots U1..U5 on the Rytm with runtime machine names, knob labels, ranges, value
  texts and enum names, page 2, and the slot's status;
- presets with knob write-back, generic (a file of knob values) and native (the
  plugin's own format);
- read access to the project's sample pool for user machines;
- a slot menu on the Rytm (FUNC + CHRO) to assign machines to slots.

**Not this round:**
- the real project-switch Y/N dialog (reverse-engineering the stock ConfirmWindow;
  the umbrella's section 2 activation prompt). Its fallback applies: status in the
  slot names plus activation from the menu;
- describing the built-in machines through the generic path. Follow-up: it frees
  ~2-2.5 KB of 0021 cave code. [measured: 0021's stub.s assembles to 8182 B of
  sections, state and the 1270 B name cache included; of that ~2.5-3 KB is per-machine
  description - descriptors ~0.8 KB, value texts ~1.4 KB, strings ~0.4 KB; the
  generic path is estimated at ~1 KB];
- porting built-in engines to plugins (code size: the arena is 32 KB, docs/plugins.md
  9.1);
- labels that depend on another knob's zone (umbrella 1, "Labels may depend on
  another knob's value"), banks/`parse()`, `time` and `list` value texts (shown as
  numbers), audio inputs, the native exciter: later rounds.

## Requirements (carried from the umbrella)

- **Everything works on a stock Teensy 4.1 without PSRAM.** PSRAM only adds headroom;
  where a feature is weaker without it (pool random access, below), the limit is
  stated and the feature still works.
- The Rytm stays safe: its side shows names and texts, writes knob values it has
  re-checked and clamped, and sends reports. A missing, slow or wrong Teensy gives
  silence or stale names on U1..U5, never a Rytm fault or a stalled UI.
- Never another machine playing with the wrong parameters: a slot that is not ready
  is silent and shown with `!`.

## Built-in machines (unchanged)

Always present, independent of the slots, in 0021 as built [measured, ext.c:54-58,
293]: **6-OP FM 40**, **JUNO 41**, **DIGIDRUM 42**, **PLAITS 44**, **DSP SAMPLE 45**,
**DAISYSP 51**. User slots come on top: machines 35..39. The built-ins keep their own
messages this round (15 JUNO knobs, 16/17 name lists, 18 FM ask); 1B and 1C below are
built so they can later absorb them.

## 1. Rytm (rytm-mods 0021)

### 1.1 Machines 35..39 become U1..U5

The hidden "2" variants BD HARD 2 (35), BD CLASSIC 2 (36), SD HARD 2 (37), SD CLASSIC 2
(38) and RS HARD 2 (39) are given up (agreed with the user) and taken over as 0021 took
40/41/42. Stock 1.73 values [measured]:

| m | slot | engine entry (0x4029037c + 4m) | names (0x401befa4 + 8m) | SRC page (0x40287cc8 + 36m) | records (0x401bf918 + 0x34 r) |
|---|---|---|---|---|---|
| 35 | U1 | 0x40290408 = 0x4010f990 | 0x401bf0bc / 0x401bf0c0 | 0x402881b4: 334 338 339 337 340 336 341 335 | 334..341 |
| 36 | U2 | 0x4029040c = 0x4010ee08 | 0x401bf0c4 / 0x401bf0c8 | 0x402881d8: 342 346 347 345 348 344 349 343 | 342..349 |
| 37 | U3 | 0x40290410 = 0x40116e58 | 0x401bf0cc / 0x401bf0d0 | 0x402881fc: 356 357 350 351 353 354 355 352 | 350..357 |
| 38 | U4 | 0x40290414 = 0x4011657c | 0x401bf0d4 / 0x401bf0d8 | 0x40288220: 358 360 359 361 363 362 364 365 | 358..365 |
| 39 | U5 | 0x40290418 = 0x401157d8 | 0x401bf0dc / 0x401bf0e0 | 0x40288244: 372 373 366 367 371 369 370 368 | 366..373 |

Per machine, registry patches as 0021's for machine 40 (registry lines 4419ff):
- **engine entry** -> `ext_render` (`from_symbol`), expect the stock value above;
- **home/mask** `ffff` (home -1, every voice) at 0x401bf17c + 2m (0x401bf1c2..0x401bf1cb,
  one 10-byte patch). 0014's `variants` patch (0x401bf1c0, 12 B) shrinks to BT
  IMPROVED only: 0x401bf1c0, 2 B, `0402`;
- **names**: the long/short pointers point at RAM buffers in a 0021 cave (1.3), not at
  strings;
- **records**: each machine's 8 records rewritten in their first 40 bytes as 0021's
  (machine field unchanged, slot 9 + (r - first) so record order = knob order, range
  0..127 (+8 min 0, +12 max 0x7f00), default 64, CC 16..23 by slot, NRPN 1:0..7). Stock
  has small ranges on 340 (0..2), 348 (0..1) and 349 (0..55) [measured], so all 40 are
  rewritten, not only the names;
- **SRC page**: the 32 bytes after the title (page + 4) list the 8 records in order
  (first..first+7), as 0x4028826c for machine 40.

Then record r of the 40 (334..373) is slot U = (r - 334) / 8 + 1, knob k = (r - 334) % 8
(P1..P8), with no table. 0020-dmach claims machines 35/38/39 too (FM4 and its names and
records, registry 2057ff): 0021 already excludes 0020, so no new conflict [measured,
registry overlap check 2026-10-10]. The per-machine RAM table 0x413db5fc[8m + slot - 9]
that `ext_param_record` reads is built at boot from the records' machine/slot fields
[inferred from machine 40, where rewriting the slots worked, measured in rytm-emu].

### 1.2 Voice flags and the engine

`ext_render_body` sets the voice byte's machine flag in a chain of five compares in
`.text` (ext.c:364-369), and `.text` is full (742 of 756 B [measured: stub.s
assembled]). The chain becomes one call to a function in another cave reading a
machine -> flag table there (machines 35..51: 35..39 -> 0x50 0x58 0x60 0x68 0x70, 40 ->
0x48, 41 -> 0x08, 42 -> 0x40, 44 -> 0x00, 45 -> 0x20, 51 -> 0x10), which frees `.text`
bytes instead of using them. Two tests in `.text` change with it:
- **ext.c:372**, `int ex = (vb & 0x10) ? dsp_excite(...) : 0;` - meant "DAISYSP", but
  0x50, 0x58 and 0x70 have bit 4 set, so U1, U2 and U5 would run DAISYSP's exciter on
  their INP knob (P5). Becomes an exact compare `(vb & 0x78) == 0x10`.
- **ext.c:385**, the sample layer is muted unless DIGIDRUM (`(vb & 0x78) != 0x40`).
  For a user slot it follows the slot's metadata: page 2 `sample` keeps the layer (as
  DIGIDRUM), `params` mutes it. One byte per slot in 0021 state, read through the same
  out-of-`.text` helper.

Reports are unchanged (0x04 / 0x03 / 0x11, P1..P16) but for the sound tag appended to 0x04 / 0x03
(section 2.6, 2026-10-10), and the Teensy already routes
0x50..0x70 to the slot's plugin (docs/plugins.md 6).

### 1.3 Names and status

The machine list reads the name pointers (getters 0x40007256 long, 0x40007276 short
in 1.74; 1.73 to measure, not needed: the table is what is patched). Each slot has two
static RAM buffers in a 0021 cave (long 12 B, short 5 B; 85 B for five), so the list
shows something from the first boot, before any heap allocation or Teensy answer:

| slot state (1A, below) | long | short |
|---|---|---|
| no answer yet (boot, no Teensy) | `USER 1` | `U1` |
| EMPTY (no machine assigned) | `USER 1 ---` | `U1--` |
| READY | the manifest's name (`NP CLUST1`) | the manifest's short (`NPC1`) |
| anything else (MISSING, BAD, NO_ROOM, MUTED, LOADING) | `!` + name (`!ATARI`) | `!` + 3 of the short |

Buffers are rewritten only in the UI task (1.6), whole names at once. LOADING shows `!`
while the load runs (incremental passes of a few ms each, tens of ms in all,
docs/plugins.md 6) and then the name.

### 1.4 Knobs: one generic descriptor pass, one generic value text

**Looks.** Descriptors are per record at 0x419a4888 + 84 r, looked up by 0x400ff556
(records >= 469 fall back to record 0) [measured, disassembly], shared by type and
copied between records (0021 design.md). One generic pass, run at the first value
text (as `ext_descriptors`) and again for a slot whenever its metadata changes,
copies per knob the look's source record: `dial` 410 (REC_DIAL), `centred` 40
(REC_BIPOLAR, SMPL FIN), `fader` 101 (REC_LEVEL), `list` 410 with the names text (as
JUNO's PAT; a stock list-switch look is not used). Then the record's invoker
(descriptor word 8, +32) is the generic value text, and the record's name pointers
(+40 long, +44 group, +48 short) point at the slot's text cache - rewritten on every
metadata change, as `dd_name_knobs` rewrites `r[10]`/`r[12]` for DIGIDRUM's ENG.

**Ranges.** The metadata's min/max/default are written into the record (+8, +12, +16,
8.8) at the same time; the OS image runs from RAM, as `dd_name_knobs`' writes show.
To measure: that a runtime range change is honoured by the knob, p-lock and LFO paths,
and what a stored word outside the new range does (stock does not clamp at kit load
[measured, rytm-mods docs/REVIEW-2026-10-09.md]; the Rytm clamps 1C values itself).

**Value text.** One function for all 48 records (40 SRC + 8 page 2). The open point -
whether the invoker's functor identifies the record - is answered for the main path:
`param_value_text` (0x400a85e0) jumps to the invoker with `(descriptor + 20, value 8.8,
string)`, a2 = 0x419a4888 + 84 r [measured, disassembly 0x400a85fa..0x400a8664], so
record = (functor - 20 - 0x419a4888) / 84. To measure: that every other caller of the
value-text `std::function` (the transient popup path, symbols.toml line 430, and any
p-lock or LFO text) passes the same storage pointer [inferred: a `std::function`
invoker always gets its own storage]. If one does not, the fallback is 48 tiny thunks,
each loading its record number (~10 B each, ~480 B). From the record: SRC record ->
(slot, knob) as in 1.1; a page-2 record -> knob 9..16, its slot from the selected
track's machine (0x8000ea00[0x41318982], as 0023's `td_pool_ctx`). Kinds:
- `number`: the value 0..127; `bipolar`: value - 64 with a sign;
- `note`: a note name, C-1..G9 for 0..127;
- `enum`: names in equal zones `min((v * N) >> 7, N - 1)` (N <= 16, names <= 7
  characters), from the slot's text cache (asked with 19 what 1);
- `preset` (the PRE knob): entry `min(v, total - 1)` of the slot's preset list, from
  the one-list cache (1.6), `---` with no presets;
- `pool`: pool slot v (0 OFF) named from 0023's pool table (the 0x14 cache, below);
  the knob is an ordinary 0..127 record, so it is stored and p-lockable. Adding samples
  to the pool stays in 0023's browser (TEENSY > ... > LOAD TO PROJ);
- `time`, `list`: shown as numbers this round.

**The pool table.** 0023 keeps the project's pool table (128 x 18 B: f, e, 14-char
name) behind `td_pool` at 0x400279c4 (`.td6`, registry) and asks for it with 13 only
from DSP SAMPLE's slot list (`td_pool_ctx` tests machine 45). For `pool` knobs 0021
needs the table on any user-slot track: 0023 exports its `td_pool_tab()` (allocate,
check the project, ask with 13 at most once in ~200 draws) through its fixed mailbox
at 0x401b9a74 (magic 'TDRV', today {magic, hook}: one more word, the claim grows from
8 to 12 B - room in cave23 to check) and 0021 calls it when the mailbox is live; with
0023 not built, `pool` knobs show numbers.

### 1.5 Page 2

`ext_page_record` (0x4003795a), `ext_page_title` (0x4003793c) and `ext_param_record`
(0x40007028) answer machines 35..39 like 44/45/51 when the slot's page 2 is `params`:
one **shared set of 8 records** for all five slots (sample words 17..24, machine field
0x36 and slots 17..24 like 0021's page-2 records, e.g. record 422 [measured,
registry]), relabelled, re-ranged and re-looked at draw time from the selected
track's slot (`ext_page_record` runs when the page draws; a descriptor change takes
effect at the next draw [measured, 0021 design.md]). With page 2 `sample` the three
lookups answer stock's page (and the layer plays, 1.2). The title is the manifest's
page-2 title.

Free records (open item 2): unclaimed in the registry and unused by 0021 [measured
2026-10-10]: **438** (LT TEST slot 12; 0021 uses 435..437), **457..460** (CB TEST),
**398..402** (BT IMPROVED) and **403..407** (FUNC. GEN.). BT IMPROVED stays listable
(0014 `variants`), so its five are not taken. Proposal: 438, 457, 458, 459, 460, 403,
404, 405, which makes 0021 need 0014's `funcgen=0` as it already needs `test=0`
(FUNC. GEN.'s page would show the user labels otherwise). Decided with the user
(2026-10-10): FUNC. GEN. goes; builds with user slots use `funcgen=0` (lp3n included).

`.ext4` (the three lookups) holds 450 of 470 B [measured]: the new branches go
through one helper in a new cave.

### 1.6 Memory and the mailbox

- **Text cache per slot**, ~1.5 KB, from the firmware's allocator `mem_new`
  (0x400829d4 [measured, symbols.toml]), allocated in the UI task on first use as
  0023's caches are: name, short, page-2 title, 16 knobs x (long 12, short 4, look,
  kind, role, range, enum count) ~24 B = 384 B, and enum names, 16 x 8 B = 128 B per
  enum knob, at most 8 enum knobs a slot (1 KB): ~1.45 KB. Five slots ~7.5 KB of heap. To measure: the allocator's free space
  after 0023's 16 KB folder cache and pool table.
- **Staging**: 1A parts arrive in the MIDI task; they are assembled in one staging
  buffer (one slot at a time; a part 0 for another slot restarts it) and committed
  only when all parts of one generation are in. The UI task then copies to the slot's
  cache and rewrites names, records and descriptors (1.4) - never mid-draw from the
  MIDI task. A 1A arriving before the UI task allocated the buffers is dropped; the
  next draw asks again (0023's rule for 0x14).
- **One-list cache** for long lists: JUNO/FM's `jn_mailbox` (0x40027a04, 1306 B in a
  1308 B claim, 127 x 10 B names [measured, registry]) generalised: its `list` byte
  becomes the key (0 JUNO, 1 FM, 0x10 + slot presets, 0x20 the installed machines),
  plus the slot's generation in one of the claim's 2 spare bytes. One list at a time,
  as now; a value text that finds another list there asks again (the throttled ask of
  `pat_text`: once in ~200 draws, only with `usb_free(4)`).
- **Router**: 0022's `looper_sysex` (`.lp4`, 378 of 444 B, 66 B slack [measured: stub.s
  assembled]) passes 0x15..0x17 to the 'JUNO' mailbox; it widens to 0x15..0x1D (one
  compare). The mailbox's hook becomes a small dispatcher in a new cave - 15..17 to
  `jn_sysex` as now (`.ext9` keeps its 14 B slack), 1A..1D to the new handlers. 19 and
  1E are Rytm -> Teensy and never routed.
- **Write-back pending per track**: 12 x (slot, a 16-bit mask, 16 values) ~240 B in
  0021 state (`.exts` has 586 of 600 B [measured], so a new RAM cave).

### 1.7 Knob write-back (1C)

In the MIDI task 1C only notes the values for the voice's owner track (0x40290dd8[v])
and the slot it was meant for. In the UI task (`ext_ui_tick`, as JUNO's 15 now):
re-check the track still plays that slot (0x8000ea00[t] == 34 + slot), clamp each value
to the record's range, write the kit word and the live copy:
- machine words P1..P8: the kit's word at 0x41255933 + 152 t + 2 k (k 0..7, int16 8.8,
  odd addresses) [measured, 0021 JUNO] and `0x4011f3fa(value 8.8, track, 9 + k)` (a knob
  turn's own call; JUNO's CUT..REL are 10..15 [measured]);
- sample words P9..P16 (page 2 `params`): their kit addresses and `0x4011f3fa`
  indices (17..24 [inferred]) **to measure**, the same way (search a track's live
  words in the kit, then a knob turn's call).
Any page, any subset of knobs; a later 1C for the same track replaces what is still
pending. The project copy (0x40765931) follows the kit later, as for JUNO [measured].

### 1.8 Cave space

New code this round, estimated (the generic path's ~1 KB agreed 2026-10-10, the rest
inferred): generic descriptor pass and value text ~1 KB, 1A..1D handlers and staging
~0.8 KB, write-back ~0.2 KB, page-2 helper ~0.2 KB, the slot menu and popups ~0.8 KB,
flag table and helpers ~0.1 KB:
**~3 KB code** plus ~0.4 KB static RAM (name buffers, pending write-backs, staging
pointers). Where: to find (cavescan of the 0021 build, the registry's unclaimed tails).
If short, the follow-up (built-ins on the generic path) frees 2-2.5 KB and moves
forward.

### 1.9 Kits and stock

A sound on machines 35..39 saved under the mods: stock loads it as DISABLE, keeps its
words, copies/pastes/stores it without fault - measured in rytm-emu for 40/41/42/44/
45/51 (rytm-mods docs/FLASHING.md, REVIEW-2026-10-09.md) [inferred for 35..39: the same
mask/engine refusal; to measure with the same stock test before hardware].

## 2. Protocol and Teensy (plugin API v2)

### 2.1 Messages

All `F0 7D 52 54 <cmd> ... F7` (sim/protocol.py), commands 0x19..0x1E unused today
[measured: protocol.py, voiceloop.ino]. **Every new message carries a version byte
`<ver>` (1) right after the command**; a receiver ignores a version it does not know
and any command it does not know (0022 already swallows unknown >= 0x0A). Text is ASCII
0x20..0x7e (else `_`), NUL padded. Generations are 1..127 per slot, kept by the Teensy
and bumped on every change of what 1A would say (assign, load, state change, install,
project change); 0 = "the Rytm knows nothing". A Teensy reboot (a power cycle, a
watchdog reset into safe mode) does not restart them at 1: a powered Rytm keeps its
cache, and small values would collide (a slot going from 1 to 2 MUTED that the Rytm
holds as 2 READY; a stale installed list at the same list generation letting a 1E op 0
assign by the wrong index). The Teensy seeds each slot's generation and the installed
list's at boot from its TRNG (else the cycle counter after the card mount), each
1..127, so a reboot lands on a cached value with odds of about 1 in 127 per slot
(`rmp_meta_init(m, seed)`; seed 1, the Mac tests' default, keeps every generation 1).
The `RYTM_PLUGINS=0` build stays at 1 (its slots are always EMPTY).

    19 <ver> <slot 1..5 | 0> <gen> <what> <arg>              Rytm -> Teensy, ASK
        what 0: the slot's metadata (answer 1A; "unchanged" when gen matches)
        what 1: enum names of knob arg (0..15)  (answer 1B list arg)
        what 2: the slot's preset list, page arg (0..7, 16 names a page; 1B 0x20; a page
                above 7 is ignored)
        what 3: the installed machines (slot 0; 1B 0x21; the first 32 packages in store
                order: a store with more lists 32, the rest not assignable from the Rytm)
    1A <ver> <slot> <gen> <part> ...                         Teensy -> Rytm, METADATA
        part 0x7F: unchanged (no payload; 10 bytes in all)
        part 0: <state> <flags> <nknobs> <nparts> <name 10> <short 4> <p2 title 13>
            [<fp hi> <fp lo>]  (2026-10-10, section 2.6: the slot's plugin fingerprint,
            fp >> 7, fp & 0x7F; 43 B in all; a 41 B part 0 means fp 0)
            state 0 EMPTY 1 READY 2 LOADING 3 MISSING 4 BAD 5 NO_ROOM 6 MUTED
            (docs/plugins.md 6); flags bit 0 page 2 = params (else sample), bit 1
            the missing machine is on the card's /MACHINES, bit 2 presets exist
        parts 1..nparts-1: up to 4 knobs each, 24 B a knob:
            <p 1..16> <look> <kind> <role> <min> <max> <default> <enum count>
            <long 12> <short 4>
            look 0 dial 1 centred 2 fader 3 list; kind 0 number 1 bipolar 2 enum
            3 list 4 note 5 time 6 preset 7 pool; role 0 free 1 preset 2 offset
        The Rytm commits when parts 0..nparts-1 of one gen are in.
    1B <ver> <slot> <list> <gen> <replace> <total> {<index> <13 name bytes>}
                                                             Teensy -> Rytm, LIST NAMES
        8 entries a message, rules as 16/17 (replace 1 on a push's first message,
        every index 0..total-1, total <= 127); list 0x00..0x0F enum names of knob
        list (shown <= 7 chars), 0x20 the slot's presets, 0x21 the installed
        machines (slot 0, gen = the store's). Generalises 16/17.
    1C <ver> <voice 0..7> <slot> <n> {<knob 0..15> <value 0..127>}
                                                             Teensy -> Rytm, KNOBS
        Any page; the Rytm re-checks and clamps (1.7). Generalises 15.
    1D <ver> <gen U1> .. <gen U5> <installed gen> [<slot> <result>]
                                                             Teensy -> Rytm, SLOTS
        Pushed on assign, install, load/state change and project change; with the
        optional pair it answers a 1E: result 0 READY 1 NOT INSTALLED 2 NO ROOM
        3 STOP TO INSTALL (transport running) 4 NOT ON CARD 5 BAD 6 LOADING.
        Lost 1Ds are caught up by the Rytm's throttled 19 on draw, and the Teensy sends the
        13-byte 1D at least every 5 s (`plug_1d_pass`).
        Also the answer to 10 <project> (2026-10-10, built): a Rytm that starts while the
        Teensy already runs has seen no 1D (0021's `us.seen` 0: the slot menu sends no 1E,
        `us_tick` re-asks every slot's 1A). Every 10 and every 19 what 3 (the slot menu's
        installed-list ask, so the 1D comes inside the menu's 2 s wait) makes a 1D owed,
        and any other 19 when no 1D went out for 3 s; an owed 1D is sent at most once a
        second, from loop() ahead of the reload, slot loads and saves (voiceloop
        `plug_1d_pass`: a 10 naming another project gets it before that project's slots
        load), and any 1D (a push, a 1E's answer) pays it. A build without plugins
        (`RYTM_PLUGINS=0`) answers a 10 with the 13-byte 1D too (every generation 1, at
        most once a second), a 19 what 3 with an empty installed list (total 0) and any 1E
        with NOT INSTALLED. It is the 13-byte form (no pair) and harmless: 0021's
        `us_slots_in` only stores the generations and `seen`, and takes a pair (15 B, slot
        1..5) as a 1E's answer, so the Rytm re-asks just the slots whose generation moved
        [read in rytm-mods-us mods/0021-ext/ext.c `us_slots_in`, `us_tick`, `us_wait`; rmp_e2e step 13].
    1E <ver> <slot 1..5> <op> <arg> <installed gen>          Rytm -> Teensy, ASSIGN
        op 0 assign entry arg of the installed list (refused if the gen is stale),
        1 clear, 2 install the slot's missing machine from /MACHINES to flash.
        Preceded by 10 <project>, as 12 is.

Sizes: 1A's knob parts ~106 B, 1B ~124 B; inbound only, under 0E's ~200 B [measured
working, 0023]. The Rytm sends 19 and 1E from the UI task with the sender's lock and
only with `usb_free(4)` (as `pat_text`), never from the audio interrupt.

### 2.2 Teensy side

- **Manifest on the Teensy.** The Teensy does not read manifests today (docs/plugins.md
  3: formats 1 and 2 differ only there). It now parses the installed package's manifest
  at slot load (from the store, streamed, `RMP_COLD`) into a per-slot description
  (`rmp_desc`, 484 B a slot [measured: sizeof on the Mac, the same layout on the Teensy
  inferred], RAM2) that 1A and 1B are served from.
- **Manifest format 3** (`rmp_pack.py`; the loader accepts 1..3; format 1/2 packages
  get the defaults): per knob `min`, `max`, `default` (0..127; default 0, 127, 64),
  text kinds `preset` and `pool` added to `number bipolar enum list note time`, and
  `role` `preset` / `offset` / `free` (default `free`; umbrella 1b); per package
  `page2_title` (<= 13) and `preset_ext` (the native preset files' extension).
- **Slots and generations**: the slot manager (docs/plugins.md 5/6) bumps a slot's
  generation on every change and pushes 1D; 1E op 0/1 run the existing assign/clear
  path ('U', which saves `/.rytm/slots/<PROJECT>.txt` and loads); the answer pair in
  1D follows when the slot has settled (READY, NOT INSTALLED, NO_ROOM, BAD: loaded,
  described, preset folder scanned, card lookup done - the answer's generation for the
  slot is its next 1A's, which 0021's `us_tick3` waits for before it gives the tracks
  the machine's defaults), at once
  for a stale installed list (NOT INSTALLED) and for a refused assign or a failed map
  save (BAD). As built: the 1Es run from loop() ahead of the reload, load and save
  passes (`plug_deadline_pass`; a 10 just before is reloaded first), and an answer not
  settled 1000 ms after the 1E (`kPlugAnsMs`, timed on the cycle counter: a flash
  erase stops millis()) goes out as LOADING (a load, card copy or save still running;
  a clear: READY) or BAD (the map unchanged, no save running), inside the Rytm's 2 s.
  An answer for a slot not yet fingerprinted (its description or preset scan pending, as
  in a LOADING) carries a new generation for it, not the previous machine's (the Rytm
  would take its cached 1A as current); the settled slot's own generation follows in a
  push. A LOADING answer ends the 1E: a failure after it shows only in the slot's 1A.
- **Install from the card** (1E op 2): copy `/MACHINES/<id>-<version>.rmp` into the
  store with the existing chunked install (CRC verified, atomic rename; 2 KB a loop
  pass, the first chunk's header checked against the slot's id and version) - only with
  the transport stopped (the Teensy follows the Rytm's MIDI start/stop, as the looper
  does, and counts a trig within the last second as playing): a 64 KB flash erase holds
  interrupts off for 0.25-0.28 s [measured with the cycle counter, docs/plugins.md 4;
  first written here as an 11-19 ms stall], so while playing the
  answer is STOP TO INSTALL, and a copy under way pauses while the transport runs. In
  the USB-host build the Overbridge output has a gap of about an erase during an
  install. Then load.
- **Silence when not ready**: a trig on a slot that is not READY plays nothing (as
  built).
- **The USB-host build plays user machines.** The final setup is the Teensy as the
  Rytm's USB host (`RYTM_USB_HOST=1`), so that build has the plugins too (decided
  with the user, 2026-10-10): `RYTM_PLUGINS` defaults to 1 in both builds, and this
  round makes the host build fit (it did not: RAM1 short for the stack, measured
  2026-10-10 with Teensyduino 1.62: -19872 B free for locals with plugins). A build
  made with `-DRYTM_PLUGINS=0` (an option, no longer the host default) played
  0x50..0x70 as PLAITS (protocol.py); it is now silent there and answers 19 with
  EMPTY (and a 10 with a 1D, 2.1), so no wrong machine ever plays. As built
  (2026-10-11, docs/plugins.md 6): host ITCM 128904 B in 4 blocks (headroom 2168 B),
  88896 B free for locals; serial 118216 B, 117408 B; RAM2 18112 B free in both.

### 2.3 Plugin API v2

`RMP_API 2`. **v1 plugins still load and play**: every new function sits in a
reserved slot of the existing tables (`rmp_plugin.reserved[8]`, `rmp_host.reserved[11]`,
sdk/rmp/rmp_api.h), sizes unchanged (`RMP_PLUGIN_BYTES` 72); a NULL plugin function
means the feature is off for that machine, and a v1 plugin never sees the v2 host
functions (it does not read those slots).

Plugin side (`rmp_plugin.reserved`): [0] `parse` stays reserved (banks, later);
[1] `load_entry`; [2] `voice_enter`; [3] `voice_leave`.
- `int32_t load_entry(void *state, const uint8_t *bytes, int32_t len, int16_t
  values[16])` - a native preset: the plugin parses its own format, sets its hidden
  state for this voice, and fills `values` for its `preset`-role knobs (-1 =
  untouched). Called between render calls, through the guard (0.5 ms limit).
- `void voice_enter(void *state)` - after `voice_init`, each time a voice starts
  playing this slot; `void voice_leave(void *state)` - before the voice switches to
  another machine or slot, or the slot unloads (release pool streams, pins). Today a
  state does not survive a leave (it lives in the voice's Plaits scratch,
  docs/plugins.md 6), so enter always follows init; the split lets a PSRAM-kept state
  skip init later.

Host side (`rmp_host.reserved`): [0] `pool_info`, [1] `pool_read`.
- `int32_t pool_info(int32_t slot, rmp_pool_info *info)` - frames, channels, rate,
  and where it is: PSRAM-resident, head-only, or not ready; asking about a slot this
  voice does not stream queues its prepare (a job between render requests, never in
  render) and answers not ready until it is done ("trig waits for content", umbrella
  1b).
- `int32_t pool_read(int32_t slot, uint32_t pos, float *buf, int32_t n)` - frames from
  `pos`, the stored channels, as floats; returns the frames delivered (fewer when not
  available: the plugin plays silence for the rest). The voice is the one the host is
  calling for.

### 2.4 Presets

- **Generic presets**: `/PRESETS/<id>/*.txt` on the card, one file a preset, its name
  the file name (13 characters shown ~9), listed in name order, at most 127. A file
  holds knob values, one line each, `P<n> <value 0..127>`; other lines and knobs not
  named are ignored. Read in jobs from `loop()`, never in the report path.
- **Native presets**: files with the manifest's `preset_ext` in the same folder, one
  entry a file (banks are `parse`, later). The host hands the bytes to `load_entry`.
- **PRE knob** (kind `preset`): turning it (a 0x03 whose PRE changed) or the voice
  first becoming this machine loads entry `min(PRE, total - 1)`: generic - the file's
  values; native - `load_entry`'s values; the host sends them as 1C for the
  `preset`-role knobs and holds those knobs' incoming values until the reports carry
  them (300 ms at most, as JUNO). `offset` knobs stay where they are and apply on top;
  `free` knobs are never touched.
- A **PRE p-lock on a trig** (0x04) plays that preset on the voice without write-back
  (as JUNO): the preset's values for its `preset` knobs until the next trig or a
  knob report that moves them. Native presets the same way: `load_entry`'s values
  apply as a p-lock override, with no write-back (as built: voiceloop
  plug_preset_load, `rmp_pv_load` with sync off; docs/plugins.md 13).
- The names go to the Rytm as 1B list 0x20 (asked with 19 what 2).

### 2.5 Sample pool access

The project's pool (`/.rytm/pools/<PROJECT>.txt`, slots 1..127) as DSP SAMPLE uses it;
the plugin picks slots itself (typically from a `pool` knob).
- **With PSRAM and the sample resident** (docs/psram.md, LRU, pinned while a voice
  reads it): full random access (built, 16-bit mono as the PSRAM pool holds it;
  untested: no PSRAM fitted).
- **Without PSRAM** (and before a sample is resident): the voice uses DSP SAMPLE's
  own machinery for its voice (stream_voice.h; one head and one ring per voice,
  `stream_heads[8]`, `stream_rings_a/b`): the sample's **head, `kHead` = 8192 bytes
  from the start [measured, stream_voice.h:27]**, in RAM with random access, and the
  rest **streamed sequentially** through the 16 KB ring (`kRing`), refilled 8 KB a read
  between requests. The honest limits: random access (granular jumps, reverse) only
  inside the head - 8 KB is 4096 frames of 16-bit mono (85 ms at 48 kHz), 2048 of
  16-bit stereo (43 ms), 1365 of 24-bit stereo (28 ms); a jump outside head + ring
  restarts the stream there (silence until refilled, a card read ~0.75 ms per 8 KB
  [measured, stream_voice.h]); and one streamed sample per voice at a time.
  Everything works, at that granularity.
- No pool writes from plugins; adding samples stays the Rytm's LOAD TO PROJ (0023).

### 2.6 The sound tag (2026-10-10; the user's decision, rytm-mods 0021 sound-tag-research.md)

A saved Rytm sound on U1..U5 stores the slot number and its words, not the plugin. Loaded where the slot holds
another plugin, the values would play on the wrong plugin. So sounds carry a plugin fingerprint; on a mismatch the
voice stays silent and the Rytm shows `!NAME`.

**Fingerprint.**

- `fp = FNV-1a32(id) mod 16383 + 1` (1..16383, 14 bits; 0 means untagged). FNV-1a32: `h = 0x811C9DC5`, then for
  each byte `h = (h ^ byte) * 16777619 mod 2^32`.
- `id` is exactly the package id's bytes as the package stores them (manifest `"id"`, the same bytes the store and
  the slot map name the package by): case kept, no version, no terminator. An upgrade keeps its sounds.
- Python: sim/protocol.py `plugin_fp`. On the Teensy (built): C++ `rmp_plugin_fp(const char* id)` in rmp_proto, the
  same values. Shared test vectors, asserted in Python by sim/tests/test_protocol_us.py (`SoundTag.FP_VECTORS`) and
  in C++ by extras/proto_test.cpp:

  | id | fp |
  |---|---|
  | `""` (empty) | 8768 (0x2240) |
  | `"a"` | 14699 (0x396b) |
  | `"rytm.v2_test"` | 12437 (0x3095) |
  | `"befaco.np.atari"` | 3424 (0x0d60) |
  | `"befaco.np.a"` | 11572 (0x2d34) |
  | `"RYTM.V2_TEST"` | 8663 (0x21d7) (case matters) |

**Teensy -> Rytm: 1A part 0.**

- Part 0 ends with `<fp hi> <fp lo>` (`fp >> 7`, `fp & 0x7F`) before F7: 43 bytes. The Teensy always sends the
  43-byte form. The fp bytes are the slot's `slot_fp[s]` when the slot is READY, and 0 in every other state
  (EMPTY, LOADING, MISSING, BAD, NO_ROOM, MUTED). Only a READY slot carries a fingerprint.
- `slot_fp[s]` (voiceloop `plug_fp[s]`): set to `rmp_plugin_fp(id)` when slot s becomes READY, and read only while it
  is READY (`rmp_build_meta_head` sends 0 for any other state).
- An older Rytm (it accepts len >= 41) ignores the two bytes; an older Teensy's 41-byte part 0 reads as fp 0 on the
  Rytm.

**Where the Rytm keeps it (rytm-mods 0021, built).** Sound index 0 (the kit's live word 0 and the project copy's slot
41), which kit, project, pool and +Drive saves and loads, power-off and stock 1.73 carry. Written only when the user
chooses the slot (the machine list's YES, the SOUND menu's MACHINE step, the slot menu's assign, the delayed wait for
1A), as the slot's current fp from 1A, with interrupts masked so a trig never sees half of it. Never on a load, a
plain 1A, a 1C or an install from the card (1E op 2), so a sound made for another plugin keeps its foreign tag.

**Rytm -> Teensy: the reported tag.**

- Every 0x04 and 0x03 that 0021's engine sends ends with `<tag hi> <tag lo>` before F7: the owner track's sound
  index 0 as the trig copied it (0x8000e5b0 + 84 t; a sound lock's own sound), as `w >> 7 & 0x7F`, `w & 0x7F`:
  the low 14 bits of the word. 0x04 is 43 bytes, 0x03 41 bytes. Sent for every machine; only the user slots'
  (voice bytes 0x50..0x70) mean anything.
- Decode: `tag = d[len - 3] << 7 | d[len - 2]` when the 0x04 is 43 bytes or the 0x03 is 41 bytes, else 0 (older
  firmware: 41 / 39 bytes). A word of 0x4000 or more (written by 0004-lfo-rnd in an older build) arrives as its
  low 14 bits; the Rytm's `!NAME` compares the same 14 bits.

**The Teensy's compare (built 2026-10-10, voiceloop `plug_report`).** Per voice `vi` there is one mismatch bit
(`plug_mis`).

0. **Where.** The compare runs in the report path only once the slot is READY: after `plug_report`'s READY check
   and after its slot-change `plug_leave`. A report for a slot that is not READY (LOADING, MISSING, ...) compares
   nothing and leaves the bit as it is: a LOADING slot has no fingerprint yet (its `slot_fp` would read 0) and must
   not cause mismatches. When slot s becomes READY, the bit is cleared for every voice with `x.user == s`.
1. **Mismatch.** For a 0x04 on a user-slot voice (slot `s = x.user`): mismatch iff `tag != 0 && tag != slot_fp[s]`.
2. **Untagged.** A tag of 0 always plays, also when `slot_fp[s]` is 0.
3. **Only a 0x04 sets or clears the bit.** A matching 0x04 (no mismatch) clears it and plays as now. A mismatching
   0x04 sets it and does not call the plugin (`note_on()`, `params()`, `voice_init`).
4. **A 0x03 never changes the bit.** Its tag carries no new information (the owner track's word as of the last
   trig) and is not compared. A 0x03 reaches `params()` only while the bit is clear; while it is set the 0x03 is
   dropped.
5. **Clear -> set releases the voice first, then sets the bit,** in this order:
   1. `note_off()` to the plugin, if the voice plays the slot (`plug_cur[vi] == s`);
   2. the `plug_leave(vi)` equivalent: voice_leave, its pool and preset state dropped, `plug_cur[vi] = -1`;
   3. its held or overdue note dropped: bit vi of `plug_note_wait` cleared (plug_leave does this; required even if
      the release is written another way);
   4. only then the mismatch bit is set (`plug_leave` clears it: set before the release, the release would undo it).
   So the next matching trig starts the voice afresh (voice_init), and a note held across the mismatch cannot come
   back. A 0x04 that keeps the bit set (set -> set) does nothing.
6. **Silent while set.** The voice renders nothing (zeros into its Overbridge channel): released, `plug_cur[vi]` is
   -1 and nothing but a matching 0x04 starts it again, so `plug_render` needs no check of its own (the voice is also
   marked not live).
7. **Note-offs.** 0x11 is always forwarded as now, whatever the bit; on a released voice it is harmless.
8. **Clearing without a trig.** Clear the bit in `plug_leave`, before its early return for a voice released already
   (`plug_cur[vi] == -1`), and call `plug_leave` whenever a report moves the voice to another slot (`x.user`
   changes), released or not, so a voice whose track moves to another slot does not keep it; leaving the user slots
   calls it too. Also clear it for every voice with `x.user == s` when slot s's plugin changes - a load queued (an
   assign, an install, a project change's reload: `plug_reload` runs `plug_leave` on every voice) - and when slot s
   becomes READY. The next trig decides again.
9. **Log** a mismatch once per clear -> set change (voice, slot, tag, slot fp), not on every trig.
10. **Sims.** sim/protocol.py `decode_trig_params` / `decode_params` return `tag` (0 when absent).
11. **proto_test.cpp** (teensy/libraries/RytmPlugins/extras): the part-0 asserts are `== 43`, `m[42] == 0xF7`, with
    the fp in `m[40]`, `m[41]` (0 for every non-READY state), plus the `rmp_plugin_fp` vectors above (done with the
    Teensy task; sim/tests/test_protocol_us.py's cross-check compares the 43-byte form). The build without plugins
    (`RYTM_PLUGINS=0`) answers a 43-byte part 0 too (fp 0).
12. **Tests on the hardware** (sim/rmp_e2e.py step 12, every report in the 43 / 41-byte form): a matching tag plays;
    a mismatched one is silent and logged once; a mismatch while a note is held releases it with no stuck note when
    the tag matches again; tag 0 plays; a slot reload or a project change clears the bits; a LOADING slot causes no
    mismatch.

**Display (Rytm, built).** The selected track on a slot whose kit word 0 (low 14 bits) is nonzero and not the slot's
nonzero fp shows the slot's names as `!NAME` / `!SHT`. Other tracks, and a mismatched sound-lock step, are not shown.

**Hardware checklist.** Tag survival through a track sound's copy/paste is unverified: the emulator has no key walk
for it (rytm-mods 0021 sound-tag-research.md, [inferred]: a raw memcpy of the sound record carries word 0). Check it
on the real Rytm: copy a tagged U-sound to another track, then confirm the 0x04 tag (Teensy log) and `!NAME` with a
1A for another plugin.

## 3. UI, errors, testing

### 3.1 The slot menu

- **USER MACHINES**, an item in the track menu (FUNC + CHRO; TrackMenuView ctor
  0x400f9b38) built as 0012's SLICE LOOP item: `menu_item_ctor` 0x40072f50, append
  0x40072786, the std::function destructor 0x401709c6, from a detour over the
  constructor's epilogue 0x400f9e96 [measured, symbols.toml, 0012 design.md]. 0012
  already owns that detour: research which mod appends both items (or a chain).
- It opens a list of five lines, one per slot, from the slot's status: `U1 NP CLUST1`,
  `U2 ---`, `U3 !ATARI`. Picking a slot opens the installed machines (1B list 0x21,
  asked with 19 what 3) and `---`; YES sends 1E (op 0 or op 1). On a `!NAME` slot
  whose machine is on the card (1A flag bit 1), the first line is `INSTALL FROM CARD`
  (1E op 2).
- How to show those two lists: open item 5 - building a menu of the same class
  with our items (dynamic labels through the items' label functions) and pushing it
  as the track menu is pushed. Fallback: five `U<n> <name>` items appended to the
  track menu itself, YES stepping a slot through the installed machines.
- **Feedback** by popup (`popup_ctor` 0x400766e8 with a `std::string`, a shared_ptr
  with control-block vtable 0x401ccab4, `view_show_popup` 0x400793f6 through the menu's
  +100 [measured, symbols.toml, as 0012]): READY, NOT INSTALLED, NO ROOM, and for an
  install STOP TO INSTALL, NOT ON CARD, BAD; no answer within ~2 s: NO TEENSY. The
  answer arrives in the MIDI task (1D's pair); the popup is shown from the UI task -
  research which UI-task hook runs while the menu is open (`ext_ui_tick` runs on value
  texts, which a menu may not draw).

### 3.2 Status and errors

- `!` prefix and a silent voice whenever the slot is not READY; `USER n` until the
  Teensy answers; `USER n ---` when empty (1.3).
- The Rytm asks (19 what 0) for a slot whose generation it lacks when a U page, the
  machine list or the slot menu draws, throttled (once in ~200 draws, `usb_free(4)`),
  so lost 1A/1D messages are caught up without a push.
- Without a Teensy: U1..U5 stay `USER n` and silent; nothing waits (no blocking asks;
  the asks stop when the USB pool is short).
- Version bytes on all new messages; unknown commands and versions ignored on both
  sides; v1 plugins served (2.3); a Teensy without user-slot support never sees a 19.
- Kits with U1..U5 load on stock as DISABLE, as the other mods' machines (1.9).
- A faulting or muted plugin (docs/plugins.md 7) changes the slot's state to MUTED:
  1D, then `!NAME` on the Rytm.

### 3.3 Testing

- **rytm-emu**: dtboot with the Teensy's 1A/1B/1C/1D injected as USB MIDI and OLED
  screenshots, as 0021's JUNO/FM tests: names in the machine list for each state,
  labels/looks/texts per kind, page 2 relabelled per slot on track changes, 1C writes
  (kit words, live block, the re-check on a changed machine, clamping), the slot menu
  and popups, `DTEMU_STRICT=1`; the stock-load test (1.9) for 35..39; power-off with
  user slots (rytm-emu `--power-off`).
- **rytm-mods**: `make verify` and the layout check (registry overlaps: 0014's shrunk
  patch, the new caves, `funcgen=0`).
- **Teensy, Mac unit tests** (`teensy/libraries/RytmPlugins/extras/`): message
  encode/decode (19..1E, versions, unknown commands), manifest format 3 parsing (1/2
  defaults), generations, preset file parsing.
- **Simulator** (`sim/tests/test_rmp.py`): API v2 with a test plugin - generic and
  native presets (values, roles, hold window, p-lock without write-back), `pool_info`/
  `pool_read` with and without PSRAM (head random access, streamed tail, a jump
  outside the head), `voice_enter`/`voice_leave`; a v1 plugin unchanged.
- **End to end**: rytm-emu with the real Teensy over the bridge (sim/teensy_bridge.py),
  `sim/rmp_e2e.py` extended: assign from the slot menu, names arriving, a preset
  turned and the knobs moving, a pool knob, a crash shown as `!NAME`. As built,
  `sim/rmp_e2e.py` steps 7..13 play the Rytm's side of 19..1E over the serial link
  themselves (no emulator), on both Teensy builds.
- **Real Rytm** only after review and the staged bring-up (rytm-mods docs/FLASHING.md).

## Open questions and research items

1. **Value-text functor** on paths other than `param_value_text` (popup, p-lock, LFO
   texts): same storage pointer? (else 48 thunks, ~480 B).
2. **Page-2 records**: confirm 438, 457..460, 403..405 are not read elsewhere by number
   (engines read the parameter block [inferred]). (`funcgen=0`: accepted by the user.)
3. **Sample words' kit addresses** and `0x4011f3fa` indices for P9..P16 (1C on page 2).
4. **Runtime record ranges**: honoured by knob, p-lock and LFO paths; stored words out
   of range.
5. **Slot menu view**: constructing and pushing a menu with our items; who owns the
   track-menu epilogue detour (0012 and 0021); a UI-task hook while the menu is open
   for the popups.
6. **Cave space** for ~3 KB code and ~0.4 KB RAM; heap room for ~7.5 KB of text caches.
7. **0023 export** of `td_pool_tab` through the TDRV mailbox (8 -> 12 B in cave23).
8. **Stock load of 35..39** (the 40..51 test repeated).
9. Name getters' 1.73 addresses (only if a hook on them is needed; patching the table
   is not).
10. Whether the Rytm's transport start/stop reaches the Teensy in every setup (needed
    for STOP TO INSTALL).

## Follow-ups

- Built-in machines described through the generic path (frees ~2-2.5 KB of 0021 caves);
  JUNO's 15 and 16/17/18 moved onto 1C/1B/19.
- The project-switch Y/N dialog (stock ConfirmWindow), replacing the menu-only
  activation.
- Zone-dependent labels from the manifest (DIGIDRUM-style), `time` and `list` texts,
  banks through `parse`, slot maps mirrored into flash, built-in engines as plugins
  where the arena allows.

## Status (2026-10-11)

**Built and tested** (Teensy 4.1 without PSRAM, both builds - serial and USB host; docs/plugins.md 11..14):
the messages 19..1E and their C++/Python cross-check (sim/tests/test_protocol_us.py, proto_test.cpp);
manifest format 3; descriptions, generations and pushes; the asks (enum names, preset pages, the installed
list with unique names `~2`); 1E assign, clear and install from the card with the transport rule, the 1000 ms
LOADING deadline and BAD at once on a refused or failed save; the 1D owed to a fresh Rytm (10, 19); plugin API 2
(load_entry, voice_enter/leave, pool_info/pool_read without PSRAM); presets with write-back and p-locks; the
sound tag (2.6); `RYTM_PLUGINS=0` answering EMPTY. Also on the Teensy side, beyond this design: the plugin store
grew to 4 MB at 0x603C0000 with a layout guard (no mount or format of a foreign layout; `rmp_install.py wipe
--yes`, the 'F' frame) and an export ('E', `rmp_install.py export`). Evidence: `sim/rmp_e2e.py` ALL PASS on both
builds (151 checks), `sim/stress.py` u, c and np with 0 dropouts on both, the Mac tests (docs/plugins.md 8).

**Not tested, or only inferred:**
- the real Rytm: everything so far is rytm-emu (the Rytm side) and the Teensy driven by sim/rmp_e2e.py (the
  Teensy side); the host build's runs had no Rytm attached;
- tag survival through a track sound's copy/paste on the hardware (2.6's checklist; [inferred]);
- the PSRAM paths (pool random access, a state over 16 KB);
- the store wipe's refusals (while a slot loads, an install is open or the info report runs): not run on the
  hardware, where a wrong path would wipe the store;
- open question 10 (the Rytm's transport start/stop reaching the Teensy in every setup).
