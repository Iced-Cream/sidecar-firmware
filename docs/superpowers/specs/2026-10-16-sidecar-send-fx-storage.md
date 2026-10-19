# Sidecar send FX: p-lockable, per-pattern, project-saved storage (research, 2026-10-16)

Follow-up to `2026-10-16-sidecar-send-fx-research.md` (section 3, "Where the values live"). That note found no
stored, p-lockable place for the send FX values. This one finds one. Research only: rytm-emu and static reading of
OS 1.73 (MAIN OS sha256 28d9ef40..., load address 0x40000400). No hardware, nothing flashed, no firmware bytes in
this repo.

Tags: **[code]** read in the 1.73 disassembly; **[measured]** run in rytm-emu on stock 1.73 (cold boot on a scratch
copy of `sections/led.img`, values poked into the working state, power-off, cold boot, play); **[inferred]**
reasoned, not shown. External format reference: libanalogrytm by bsp (`kit.h`, `pattern.h`,
https://github.com/bsp2/libanalogrytm), which documents the V5 (OS 1.70+) SysEx kit (0xA32 B) and pattern
(0x332D B) layouts. Its kit offsets equal the firmware's kit storage offsets used below.

## Summary

- **The FX track has two stored, p-lockable words that nothing uses: FX index 0 and FX index 29.** No parameter
  record names them: the FX records 72..99 use FX indices 1..28 and 30..37 [code: ROM records 0x401bf918, +4].
  Stock still carries both end to end. The kit stores them in FX slots 37 and 20, at kit offsets 0x814 and 0x7F2
  [code: FX loader 0x40007f02, saver 0x40007f58, maps 0x401c5934 / 0x401c589c]. libanalogrytm calls these bytes
  `__unknown_arr3[0..1]` and `__unknown_fx_1/2`. Pattern lock types 37 and 20 on track 12 load into the live lock
  array [code 0x400075c8; measured]. The sequencer overlays them at the FX trig into the per-track trig array
  [measured]. Stock saves rebuild them [code]. libanalogrytm lists FX lock type 20 as "not assigned" and lacks type
  37, which matches.
- **Each word holds two 7-bit values, both p-lockable.** A lock is a coarse slot (7 bits, word bits 8..14) plus an
  optional `0x80 0x80` continuation slot (7 bits, word bits 1..7). The continuation is the stock fine-resolution
  path. Measured: coarse 0x33 plus fine 0x21 load as word 0x3342 and reach the trig array.
- **FX indices 38..41 are four more words.** They are already in RAM and are applied but not stored. The live kit
  bytes +1942..+1949, between the 38 FX words and the perf block, are FX words 38..41. Every kit holds zero there.
  The audio side copies them to its FX array [measured: poked 1100 2200 3300 4400 and read them back at
  0x8000fbe4]. The live lock array has slots for them, and the sequencer applies a lock there into the trig array
  [measured: idx 38 lock 0x7700 reached 0x8000e9ec]. What they lack is storage: no kit slot and no lock type. The
  stock lock loader drops any type above 41 [code 0x40009040]. Storing them takes small load/save detours.
- **Sound tracks: sound index 0 is the only stored, p-lockable spare word** (lock type 41, kit slot 41). It is
  taken by 0021's tag and 0004. The other spare room in a sound is not lockable: stored slots 42..47 (zeroed by
  every stock `sound_save`) and live `+0x6d..+0x6f` (0013 uses two of the three bytes). The live lock array has
  exactly indices 0..41 per track. Its 44-word step stride is 42 words plus a u32 lock count, so no lock word is
  hidden there.
- **Values follow the pattern.** Kits are per pattern (pattern byte 0x3325 = kit number; the active kit switches
  with the pattern [measured by 0021]). The base values sit in the kit and the locks in the pattern. All 128 kits
  and all 128 patterns are resident (kit stride 2531 B, live pattern stride 85760 B) and copied as whole blocks
  [code], so these words travel with them.
- **Stock firmware stays safe.** On stock, FX 0/29 and sound index 0 values and locks load, play nowhere and are
  saved back [code; measured for load]. New lock types 42+ are skipped at load and dropped at the next full
  rebuild, which loses them silently and corrupts nothing [code; measured].
- **Recommendation:** FX-page values in FX words 0, 29 (stock-plumbed) and 38..41 (with detours). That gives 12
  lockable 7-bit values for the two FX. Per-track SEND FX 1/2 in sound index 0 (high half SEND 1, low half SEND 2),
  with 0021's tag kept on user-machine tracks only (section 5). A new-lock-type scheme with a mod-owned step table
  is feasible but not recommended (section 3).
- **Update (user, 2026-10-16):** SEND FX 1/2 in sound index 0 on **all** tracks, user slots included; 0021's tag moves
  to stored slot 47 (design below, M1b). The send FX mod is optional (Pi-focused). M0 (stock, measured) and M1 (the
  reporter, rytm-mods 0025) are done: sections "M0 results" and "M1" below.

## 1. Kit (question 1)

Kit storage v6 (`kit_save` 0x400088d6 writes version 6 at +0; `kit_load` 0x4000863e rejects others), 0xA32 B,
matching libanalogrytm's V5 SysEx layout byte for byte. Live kit 0x9E3 = 2531 B. Kit k is resident at
`0x412558e3 + 2531 k`. The working state's stored copy (project copy) is at `0x407658e5` [measured].

| storage | live | content | stock load / save | tag |
|---|---|---|---|---|
| 0x7C6 long | - | FX block version, must be 0 | load checks, save clears | [code] |
| 0x7CA + 2 s, s = 0..37 | +1866 + 2 i, i = 0x401c5934[s] | the 38 FX words | all 38 through the maps | [code] |
| **0x7F2 (s 20)** | **+1924 (FX i 29)** | **no record; free** | carried | [code; measured load] |
| **0x814 (s 37)** | **+1866 (FX i 0)** | **no record; free** | carried | [code; measured load] |
| **0x816..0x829** (20 B) | - | not read, not written by the FX saver | untouched | [code] |
| 0x82A..0x831 (8 B) | - | zeroed by the FX saver | zeroed | [code] |
| **0x832..0x841** (16 B) | - | perf block reads from +16 (0x842) | untouched | [code] |
| **0x902..0x916** (21 B) | - | between perf (to 0x901) and scenes (0x917) | not read | [code; writer not checked] |
| - | **+1942..+1949** | **FX words 38..41**, zero in every kit | no storage | [measured] |
| 0x9D7.. 0x9DE | +2430.. | scene id, two clamped words, retrig mask (libanalogrytm's `__unknown_09D9` is used) | used | [code] |
| 0xA0F..0xA11 | +2491, +2495 | used (libanalogrytm `__unknown_arr6`) | used | [code] |

- The per-param "pad" bytes after each FX value in libanalogrytm are the LSB (fine) halves of 16-bit words. They
  are not free.
- **Persistence measured** (working state, power-off, cold boot): words poked into the stored kit at slots
  37/20 came back as live FX words 0/29 in kit 0 (`2a54`, `3b66`). Marker bytes at 0x816, 0x836 and 0x902 survived
  in the state but nothing loads them into the live kit. Live-only pokes do not reach the state: the project copy
  is written per parameter record by `kit_param_changed` (0x4003355c) or by a full `kit_save`. FX words 0/29
  have no record, so a mod that edits them writes both the live kit and the project copy, as 0021's `us_defaults`
  does for index 0.
- **Kit save paths.** One path zeroes the whole 0xA32 buffer before `kit_save` (0x4000f2a2). That erases the gap
  bytes but not FX words 0/29, which `kit_save` writes back from live. Others re-serialize into an existing buffer
  (0x4018c4f8, load then save) [code]. Copies inside the RAM bank are 0x9E3 memcpy (`kit_reload` 0x400af0d2)
  [code], which carries +1866..+1949 whole. Kit init and factory reset: the FX defaults routine 0x40011770
  was not read, so whether it zeroes FX words 0/29/38..41 is [inferred, zero expected]. OS upgrade: the FX block's
  version long and the kit version 6 are checked; nothing converts these words [code].
- Not tested in the emulator: SAVE KIT, SAVE PROJECT, RELOAD KIT, a SysEx kit dump, track copy/paste. Track
  copy/paste copies sounds (0x98 B), not the FX track. Expected from the code: FX words 0/29 survive all of them.

## 2. Sound (question 2)

Unchanged from rytm-mods `re/subsystems/sound-storage.md` and 0021's `sound-tag-research.md`:

- **Index 0** (live `+0x14`, stored slot 41 at `+0x6e`): carried by every path, including a stock cold boot
  [measured by 0021]. Lockable as type 41 on tracks 0..11. `0x401c5a74[41] = 0`, and the loader accepts types
  0..41 [code]. Taken by 0021's tag (14 bits) and by 0004.
- **Slots 42..47** (stored `+0x70..+0x7b`, 12 B): never loaded, zeroed by every stock `sound_save` [measured 0021].
  0013 keeps its HPF in slot 42 through load/save detours.
- **Live `+0x6d..+0x6f`**: not serialized, heap garbage after a load; 0013 uses `+0x6d/+0x6e`.
- Not lockable: the lock path is indexed by sound index 0..41 only (next section).

## 3. Pattern and p-locks (question 3)

**Storage** (v5, 13101 B, `pattern_load` 0x40009146 checks version 5; libanalogrytm `ar_pattern_t`):

- 13 tracks of 641 B, then 72 lock slots of 66 B at +0x2091: `{type, track, value[64]}`. A value is 0..127 or
  -1 for no lock on that step. `type = 0xFF` marks a free slot.
- `type` is the parameter's **storage slot**, not a parameter id. Tracks 0..11 take types 0..41: sound index =
  `0x401c5a74[type]`, and type 41 is index 0. Track 12 takes types 0..37: FX index = `0x401c5934[type]`, with type
  20 = FX 29 and type 37 = FX 0 [code 0x400075c8]. The reverse maps 0x401c59cc / 0x401c589c serve the save.
- A slot `{0x80, 0x80, lsb[64]}` right after a slot is its fine continuation: `word |= lsb << 1` [code 0x400090c4;
  measured].
- Spare per-track bits: storage +0x279 bits 2..6 and +0x277 bits 3..6 are ignored by the load and lost on save
  (rytm-mods symbols). No spare whole bytes (libanalogrytm V5 shows none either).

**Live** (`pattern_load` target; working copy at `0x407c89e3`, bank pattern i at `0x407c89e3 + 85760 (i+1)`, all
resident [code 0x4000ce94 loop; measured]):

- Locks at +11921: per track 5674 B = 64 steps x (42 words + u32 lock count) + 42 fine flags. Word =
  `(msb << 8) | (lsb << 1)`, `0xffff` = no lock; lookup `0x400ac51c` bounds the index to 0..41 on every track.
- At a trig, `0x4009a8e8` turns the step's 42 words into lock events. The audio side writes them into the per-track
  trig array `0x8000e5b0 + 84 t` (post-lock, pre-LFO). The LFO-modulated copy `0x8000f7a8 + 84 t` follows for
  words 0..37 [measured on the FX track: delay TIME 0x6000, FX 0 0x3342, FX 29 0x5500, FX 38 0x7700 in the trig
  array; the modulated array shows 0/29/9 locked, 38 not].
- **Read the trig array, not the modulated one:** the FX LFO with no destination writes into FX word 0 of the
  modulated array (write at 0x4009eaa2) [measured; that a nonzero depth changes it is inferred].

**Load and save** [code; measured where noted]:

- Load (`0x40008f98`, one caller): types above 41 are skipped (0x40009040). For track 12, types 38..41 pass the
  check but map to FX index 0, so an extension must not use them there. Measured: slots with type 0x2A (track 0)
  and 0x60 (track 12) left no lock.
- Incremental save (`0x4000940c`, from the lock-edit path 0x400ac262): updates or appends one (track, index) slot
  and leaves other slots alone, including unknown ones.
- Full rebuild (`0x40009216`, from the full pattern save 0x400093f2, from 0x400ac280, and from the incremental save when a
  (track, index) has no locks left): sets all 72 slots free and re-encodes from the live array. **It drops unknown types** [measured: a trig
  placed with the FX track selected rebuilt bank 0's slots and the 0x2A / 0x60 slots were gone]. Placing a trig on
  a step also cleared that step's existing locks [measured].
- An unknown-type slot followed by a `0x80 0x80` continuation is unsafe: stock keeps the previous valid slot's
  (track, index) and would OR the bytes into it. A mod must not write continuations for its own types.

**Can a mod add parameter ids beyond 41?** Yes, but every path then needs a detour:

- the load skip, kept in a mod table;
- the full rebuild, adding the mod's slots after it;
- the lock edit, clear and display;
- a step table per live pattern: 129 patterns x 12 tracks x 2 x 64 B is about 200 kB outside the 85760-B
  patterns. Every whole-pattern memcpy (working <-> bank, copy/paste, clear) would have to copy it too.

Stock firmware stays safe. Cost and data-loss risk inside the mod are high. Not recommended. The FX words 38..41
plan below reuses the stock live array instead and needs only the type mapping.

## 4. Per-pattern behaviour (question 4)

- A pattern stores its kit number (0x3325). Changing to a pattern with another kit sets the active kit
  (`0x800067b8`), and the audio interrupt runs `kit_loaded` [measured by 0021: A02 -> kit 1]. Each trig copies
  its track's sound from the active resident kit (`0x40120670`). For the FX track, the trig array is reset from
  the kit (memcpy) and then locked [measured].
- So kit values (FX words, sound index 0) follow the pattern's kit, and locks follow the pattern. A pattern
  switch to another kit was not run with these words [inferred from the above].

## 5. Reusing used-but-idle parameters (question 5)

- **Sample layer on 0021 EXT tracks** (words 17..24, muted by 0021): stored, lockable, LFO-able, per kit, with no
  storage work. It only exists on EXT tracks, and the analog voices are the ones that need sends. Not general.
- **Machine words unused by a machine:** which words are idle differs by machine, and a machine change rewrites
  or reinterprets them. Fragile.
- **Fine LSB of AMP DEL/REV** (words 40/41): stored and lockable, but a stock knob turn or a stock lock writes the
  LSB too, so it couples the sends to the stock sends. Rejected.
- **Sound index 0 shared with 0021's tag:** the tag only matters on user-machine tracks (35..39), whose voices run
  on the Sidecar. There the spec's M5 already puts the send as a slot knob applied at the source. So: index 0 =
  tag on machines 35..39, index 0 = SEND 1 (bits 8..14) / SEND 2 (bits 1..7) on every other machine. 0021's
  choice routes already write index 0 on a change to 35..39. Changing back needs one clear, and the AMP page 2
  knobs are hidden on 35..39. Stock behaviour is unchanged: it carries and saves index 0 and its type-41 locks.
  0004 stays excluded, as it already is with 0021.

## 6. Ranking and recommendation (question 6)

| option | values | lockable | stock on load / save | Rytm cost | risk |
|---|---|---|---|---|---|
| **A** FX words 0, 29 | 4 (2 words x 2 halves) | yes, stock path | carried and kept | UI only (writer to live kit, project copy and lock array) | low |
| **B** FX words 38..41 | 8 | yes | locks dropped, base erased on a zeroing save: harmless loss | load type detour (0x40009040/0x400075c8), save map detours (0x40009454, rebuild at 0x400092b2), FX load/save detours (0x40007f02 / 0x40007f58) for storage 0x816..0x81D: ~6 hooks, ~250 B [inferred] | medium |
| **C** sound index 0 per track | 24 (SEND 1/2 x 12) | yes, stock path (type 41 + continuation) | carried and kept | 0021 coexistence rule + UI | low-medium (0021 tag interplay) |
| D new lock types + mod step table | any | yes | dropped: harmless | 6+ detours, ~200 kB RAM, every pattern copy path | high |
| E reuse idle params | few, per machine | yes | n/a | small | fragile |
| F kit gaps 0x81E..0x829, 0x832..0x841, 0x902..0x916 | 49 B | no | erased by a zeroing save | load/save detours + a live home (none left in the kit) | medium |

**Recommendation: C for the per-track levels, A then B for the FX pages.**

- **AMP page 2:** SEND FX 1 = index 0 bits 8..14, SEND FX 2 = bits 1..7. Base in the kit's sound, locks as type
  41 + continuation.
- **SEND FX 1 page:** FX word 29 and words 38, 39. **SEND FX 2 page:** FX word 0 and words 40, 41. Each word holds
  two knobs, so 6 lockable knobs per FX. Ship A (2 knobs per FX) first; B adds 4 per FX.
- **FX type per project** on the Sidecar, as before. It needs no lock and no Rytm storage.
- **The Sidecar reads:** the mod sends the values from the trig array `0x8000e5b0 + 84 t` (word 0 for t = 0..11;
  words 0, 29, 38..41 for t = 12) at each trig and on kit load. Those are post-lock values, so p-locks are
  sample-exact to the trig.
- **Writing a lock from the mod's knob:** live lock word + fine flag + step count, then the stock incremental
  writer (0x4000940c via the 0x400ac230 path). Base edits write the live kit and the project copy.

**On stock**, a project saved with A and C loads and saves unchanged and plays exactly as stock. B's values and
locks are dropped silently.

## M0 results (stock 1.73 in rytm-emu, no mod, 2026-10-16)

Setup: stock 1.73 cold-booted on a scratch copy of `sections/led.img` (dialogs YES NO NO NO), values poked into the
stored working state (project kit copy `0x407658e5`, bank pattern A01 at `0x405c8f38 + 13101`, lock slots from 14 on:
led.img's A01 uses 0..13), power-off, cold boot. Trigs poked into A01's trig bits (64 x 14 bits big-endian at
+4 + 641 t; bit 0 = trig): BD steps 1/5/9/13, FX steps 1/9. Values read with `dtpeek` and, while playing, with
`DTBOOT_PEEK` every 5M instructions on `0x8000e5b0` (BD word 0) and `0x8000e9a0` (FX words 0/1, 28/29). Every row
**[measured]**. Note: the cold boot loads the **bank** pattern (A01), not the stored working pattern at
`0x405c8f38`; pokes there did not reach the live locks.

| test | result |
|---|---|
| BD type 41 (sound index 0) + `0x80 0x80` fine slot: step 1 0x33/0x21, step 5 0x44/0x05, step 9 0x10, kit word 0 0x1100 | live locks 3342 / 440a / 1000; trig array word 0 per step **3342, 440a, 1000, 1100** (the kit's on step 13), every bar |
| FX type 37 (FX 0) 0x55 on step 1, type 20 (FX 29) 0x66 on step 9, kit FX 0/29 0x2a54/0x3b66 | trig array step 1 FX0 5500 FX29 3b66, step 9 FX0 2a54 FX29 6600 |
| SAVE KIT (FUNC + PLAYMODE, SAVE KIT, YES YES): live markers 2200 / 4c76 / 5d08 poked first | project copy slot 41 = 2200, FX slots 37/20 = 4c76/5d08 (the save serializes from live; `kit_save` 0x400088d6 ran twice) |
| live zeroed, then RELOAD KIT | 2200 / 4c76 / 5d08 back in the live kit (`kit_load` 0x4000863e not called: the 0x9e3 copy) |
| SAVE PROJECT AS 003 (GLOBAL, PROJECT, SAVE PROJECT AS, 003, YES YES) | 129 full lock rebuilds (0x40009216), 129 `kit_save` (counted) |
| live kit, project copy, live locks **and** A01's stored lock slots wiped, then LOAD PROJECT 003 | kit 2200 / 4c76 / 5d08, locks BD 3342 / 440a / 1000, FX 5500 / 6600 all back; A01's stored slots re-encoded in the file's order (type 41 + fine at slot 0, 37 at 2, 20 at 9): the file carried them; playing gives the same per-step values |
| two patterns on two kits: A02 (kit 1) with BD type 41 0x70 + fine 0x11 (step 1), 0x08 (5), FX type 37 0x0c (9), type 20 0x0d (1); kit 1 BD 0x6600, FX 0/29 0x0a0a/0x0b0b (live) | playing A01, then BANK_A + T2: active kit 0 -> 1 at the pattern change; trig array BD 7022, 0800, ..., 6600; FX step 1 0a0a/0d00, step 9 0c00/0b0b. Values follow the pattern and its kit |
| SysEx dump out (DIN, `--midi` requests `F0 00 20 3C 07 00 <id> 01 01 <nr> 00 00 00 05 F7`) | kit request 0x62 nr 0 -> 0x52 (the stored kit: zeros, not yet saved), kit X (0x62 nr 0x81, or 0x68) -> 0x58 (2998 B, 0xA32 decoded): BD slot 41 at 0x2E + 0x6E = 1100, FX 0 at 0x814 = 2a54, FX 29 at 0x7F2 = 3b66; pattern X (0x6A) -> 0x5A (13101 B decoded): type 41 track 0 + fine, type 37 and type 20 track 12, the values as poked. The checksum (sum of the 7-bit payload & 0x3FFF) and length (payload + 5) re-encode both dumps byte for byte |
| SysEx dump in: kit X with BD 4400, FX 0/29 1357/2468; pattern X with BD step 1 0x2a/0x0b and FX type 20 step 9 0x77 | kit X applied at once (live and project copy); the pattern X at the next cycle: trig array BD 2a16 on step 1, FX29 7700 on step 9 |
| a trig placed on FX step 3 (FX, RECORD, T3, RECORD) | no rebuild ran (0x40009216 / 0x4000940c not called); locks unchanged; power-off and cold boot: all kept |

So on stock: sound index 0 and FX words 0/29, their base values and their p-locks (incl. the fine slot), survive SAVE
KIT / RELOAD KIT, SAVE PROJECT / LOAD PROJECT (with the full rebuild of all 129 patterns), the pattern/kit switch, and a
SysEx kit/pattern dump round trip.

**New [code] finding - FX words 38..41 locks must not reach a stock save.** The FX reverse map 0x401c589c has 38
entries; the incremental save (0x40009454) and the full rebuild (0x400092b2) both index it with the lock's FX index, so
for FX 38..41 they read the next table (0x401c5934 = 9, 10, 11, 12) and write lock **types 9..12 on track 12**, which
load as FX 18..21 locks (0x401c5934[9..12]): a live-only FX 38..41 lock corrupts real FX locks at the next save. Stock
never creates such locks; a mod that does needs the hooks below first. (On load, track 12 types 38..41 map to FX 0
through 0x400075c8's bound, as section 3 says.)

## M1 (rytm-mods `sendfx-report`, 0025-sendfx-report, 2026-10-16)

A read-only reporter on branch `sendfx-report` (off `user-slots`): every audio block, from the block driver after
`fx_generate` (detour 0x401213a0), it compares `0x8000e5b0 + 84 t` word 0 (t = 0..11) and words 0, 29, 38..41
(t = 12) with what it last sent and sends a changed track as host-link SysEx **0x20 SENDFX** (`sim/protocol.py`):
`F0 7D 52 54 20 01 <t> <hi> <lo> F7`, track 12 with six pairs; hi = word bits 8..14, lo = bits 1..7. Round robin: one
track resent every 64 blocks. Sent only when the USB MIDI sender is safe from the interrupt (0021's tests, plus the
port configured). 616 B in 0002's cave block. Design and tests: rytm-mods `mods/0025-sendfx-report/design.md`.
**[measured]** (`tools/emu/sfx_emu.py sfx1`, DTEMU_STRICT=1): the host stand-in received BD 3342 440a 1000 1100 per
step and FX 5500/3b66, 2a54/6600, words 38..41 as written; the BD and FX reports of one step in the same 5M window.

## Design: the sound tag out of sound index 0 (M1b)

Goal: sound index 0 = SEND FX 1 / 2 on **every** track (user slots included); 0021's plugin tag in a stored,
non-lockable place.

- **Stored:** sound storage slot 47 (`+0x7a..+0x7b`, 14-bit tag big-endian). Slots 42..47 are serialized by every
  stored-format copy and never read by `sound_load` [code, symbols]; 47 keeps clear of 0013's slot 42.
- **Live:** sound `+0x6e..+0x6f` (spare, not serialized). Raw 0x98 copies (RELOAD KIT, pool, copy/paste) carry it
  [measured by 0021]. Taken from 0013 (`+0x6d/+0x6e`), which 0021 already excludes.
- **Keep-on-save hook:** `sound_save` after its memset of the stored block, detour **0x40008492** (`lea 0x401c59cc,%a0`,
  expect `41f9401c59cc`; a2 stored, a3 live; 0013's site): `stored+0x7a = live+0x6e`, then re-emit, rejoin.
- **Load hook:** `sound_load` after its memset, detour **0x4000826a** (`lea 0x401c5a74,%a0`, expect `41f9401c5a74`;
  a2 live, a3 stored; 0013's site): `live+0x6e = stored+0x7a` (0 if above 0x3fff). Migration of 0021-tagged sounds: if
  slot 47 is 0, the machine (`+0x7c`) is 35..39 and slot 41 is nonzero, the tag comes from slot 41 and live word 0 is
  cleared (the sends start at 0). Every sound path that deserializes (kit, project, +Drive, pool, SysEx/Overbridge
  sound transfer) goes through `sound_load` [inferred; `sound_save` has ten callers, symbols.toml].
- **0021 changes:** `us_defaults` writes the tag to live `+0x6e` and the project copy's slot 47 (`0x40765913 + 162 t +
  0x7a`) instead of word 0 / slot 41; `ext_render` and the `!NAME` check read the tag from the track's kit sound
  (`0x4125590d + 2531 k + 152 t + 0x6e`, k = `0x8000fc58[t]`, the kit last copied) instead of `0x8000e5b0` word 0. A
  sound-locked step keeps the kit sound's tag (the pool sound's is not looked up; accepted limitation).
- **Stock:** `sound_load` ignores slot 47 (the track shows DISABLE on 35..39 anyway); the next stock `sound_save` zeroes
  it, so the sound comes back **untagged** = plays: harmless. Index 0 keeps the sends, which stock carries.
- **Cost [inferred]:** two detour stubs ~30 B each, migration ~30 B, three 0021 call sites changed (~20 B net), all in
  the 796 B left in 0002's block. Tests: us_emu's tag tests moved to slot 47, the M0 matrix again (SAVE/RELOAD KIT,
  SAVE/LOAD PROJECT, pool, power-off, stock cold boot) for slot 47 and index 0 independently.

## Design: persistent locks for FX words 38..41 (M1b)

- **Lock types:** on track 12, types **42..45** = the hi halves (bits 8..14) of FX 38..41 and **46..49** = the lo halves
  (bits 1..7), each a plain 64-step slot; never a `0x80 0x80` continuation (unsafe after an unknown type, section 3).
  Stock skips 42+ at load (0x40009040) and drops them at the next full rebuild: harmless loss [measured for 0x2A/0x60].
- **Load** (`0x40008f98`): detour at **0x40009040** (`cmpl %d4,%d6` / `bltw 0x40009128`, d4 type, d5 track, d2 track
  signed, a3 the live lock base, a2 the slot): for track 12 and type 42..49 merge the 64 values into live index
  38 + (type - 42) % 4, hi or lo half (`0xffff` start = 0), count a step's lock once, set the index's fine flag for a
  lo slot; then continue at 0x40009128 (next slot).
- **Full rebuild** (`0x40009216`, outer loop k = 0..41, inner track 0..12): detour at **0x400092b2** (`movel
  %a5@(0,%d3:l),%d4`, d3 = 4k, d1 track, d0 the next slot): for track 12 and k >= 38 write the hi slot {42 + k - 38,
  12, msb[64]} and, if any step has a lo, the lo slot {46 + k - 38, 12, lsb[64]}, advance d0, jump to 0x40009330
  (never the stock encode, which would write types 9..12). k 38..41 come last, so a full pattern (72 slots) drops
  them first.
- **Incremental save** (`0x4000940c`): detour at **0x40009454** (the map read `movel %a1@(0,%a0:l:4),%d5`): for track
  12 and index >= 38, call the full rebuild instead (stock already does that when a (track, index) has no locks left).
- **Base values (kit):** FX loader 0x40007f02 / saver 0x40007f58: storage **0x816..0x81D** <-> live +1942..+1949
  (the four words). The zeroing save path (0x4000f2a2) is followed by `kit_save`, which writes them back.
- **Copies:** the live lock array has indices 38..41 inside every 85760-B pattern, so pattern copies carry them
  [code]; a SysEx pattern dump carries the slots raw.
- **Cost [inferred]:** 5 detours, ~250-300 B; up to 8 of a pattern's 72 lock slots. Risk: medium (pattern storage).
  Order: these hooks before any writer of FX 38..41 locks (the finding above).

## M1b results (implemented, rytm-mods `sendfx-report`, 2026-10-16) [measured in rytm-emu, DTEMU_STRICT=1]

Both designs above are built (`sfx1b` = us1 + 0025, `hwsfx1b` = hwus1 + 0025; build.py / verify.py PASS) with two
corrections to the design: the rebuild hook sits at the track test **0x400092aa** (reimplemented), not 0x400092b2
(tracks 0..11 branch back to 0x400092b6, inside a jmp placed there), and the lock-load hook reimplements its
pc-relative branch. Details: rytm-mods `mods/0021-ext/design.md` ("The tag moves out of sound index 0") and
`mods/0025-sendfx-report/design.md` ("M1b-b").

- **Tag move (0021, `.ust` 302 B):** detours 0x40008492 (save), 0x4000826a (load + migration), 0x4011f58a (the trig's
  sound copy -> `us_tag[t]`, which the 0x04/0x03 reports carry; sound locks keep their pool sound's tag). The whole
  user-slot suite (`us_emu.py sfx1b all`, its tag tests on the new location) passes. A sound tagged the old way
  (slot 41 = fp, slot 47 = 0) is migrated at the cold boot (project copy rewritten in place: slot 47 = fp, slot 41 = 0;
  live index 0 = 0) and keeps playing on its plugin (0x04 tag = fp, no `!`). SEND p-locks (type 41 + fine) on a U1
  track reach the host per step (4020, 20fe, base) while every 0x04 keeps the tag. On stock the U track shows DISABLE
  and slot 47 stays raw in the project copy; back on the build the tag is back.
- **FX 38..41 storage (0025, +452 B):** kit storage 0x816..0x81D and lock types 42..49 load, play (host receives FX
  38..41 per step), survive SAVE PROJECT (129 full rebuilds) / wipe / LOAD PROJECT, a stock incremental save (a UI
  p-lock on the FX track) and a power-off. On **stock 1.73** with the mod's card: cold boot and LOAD of the mod's
  project leave no FX 18..21 or 38..41 lock and no type 9..12 slot; stock's own SAVE PROJECT + LOAD drop types 42..49
  and write no 9..12.
- Not exercised: the incremental-save hook's FX index >= 38 branch (no writer of those locks exists yet).

## Next steps

1. Done: M1b-a and M1b-b (above).
2. Before M4's FX pages write FX 38..41 locks: an emulator test of the incremental-save branch for index >= 38 (the
   writer drives it).
3. Done: M2 (Sidecar): core/fx + the Pi engine (research spec "M2"). The FX TYPE is now a kit value (FX 1: word 29
   hi; FX 2: word 0 hi; P1..P5 the other halves), user effects UFX1..4 with a kit tag (storage to choose in M4).
4. M4 (Rytm UI): AMP page 2 (SEND FX 1/2 knobs writing index 0's halves: live kit + project copy, locks through the
   stock lock path as type 41 + fine), then the FX pages (FX 29/0 first, 38..41 after M1b-b).
5. Hardware (after the firmware review): 0025 alone on a stock-based image first, then with the user-slot build.

Debug aid: the stored working state holds the working pattern at `0x405c8f38` and bank pattern i at
`0x405c8f38 + 13101 (i+1)`. Pokes into the bank pattern (and the project kit copy) plus `--power-off` and a cold boot
reach the loader, as the tests above did; the cold boot loads the bank pattern, not the working one [measured
2026-10-16]. Poking the live arrays does not reach the state.
