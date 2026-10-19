"""Draft wire protocol between patched Rytm firmware and the Teensy (v0, 2026-10-07).

Everything is SysEx with the non-commercial manufacturer ID 0x7D, which the stock
OS discards (docs/measure-emulator.md), so a patched Rytm can claim it:

    F0 7D 52 54 <cmd> ... F7          ("RT")

Teensy -> Rytm, cmd 0x01 AUDIO: one 32-sample block (one Rytm audio block,
0.667 ms at 48 kHz) for one voice, samples as 14-bit two's complement split into
two 7-bit bytes (high first):

    F0 7D 52 54 01 <voice 0..7> <seq 0..127> <32 x (hi, lo)> F7     = 72 bytes

Rytm -> Teensy, sent by the EXT machine's engine (rytm-mods 0021, ext_render;
implemented 2026-10-08, P1..P8 only):

  cmd 0x04 TRIG+PARAMS: a trig, with the parameters it plays with, so p-locks
  land on their step:

    F0 7D 52 54 04 <voice 0..7> <note> <velocity> <P1..P16> F7      = 41 bytes

  cmd 0x03 PARAMS: P1..P16 changed (knob, LFO, slide); at most once every 16
  audio blocks (about 90/s) per voice:

    F0 7D 52 54 03 <voice 0..7> <P1..P16> F7                        = 39 bytes

  The sound tag (2026-10-10, rytm-mods 0021 sound-tag-research.md): a Rytm with
  the tag appends two bytes before F7 to both, <tag hi> <tag lo> = the owner
  track's sound tag (tag >> 7, tag & 0x7F; 0x04 43 bytes, 0x03 41), for every
  machine it reports. For the user slots (0x50..0x70) it is the fingerprint
  (plugin_fp) of the plugin the sound was made for, 0 untagged. A host plays a
  user-slot voice only when the tag is 0 or the fp of the plugin it runs in that
  slot; otherwise the voice stays silent until a trig whose tag matches (or is
  0). Shorter reports (older firmware) carry no tag: 0. Where the Rytm keeps it:
  sound index 0 until 2026-10-16; since then (rytm-mods sendfx-report, M1b) the
  sound's stored slot 47, so index 0 is SEND FX 1/2 on every track (0x20 below).
  The wire format is unchanged.

  cmd 0x11 NOTE_OFF: the voice's gate closed - pad released, the sequencer
  trig's LEN elapsed, or the sequencer stopped (rytm-mods 0021-ext,
  2026-10-09; older firmware never sends it):

    F0 7D 52 54 11 <voice 0..7> F7                                  = 7 bytes

  The voice byte is the plain physical voice, no machine flags. It only
  follows a trig on that voice, never between a trig and its own retrig; a
  new trig (0x04) always reopens the voice. JUNO's LEN 127 holds until it.

  Voice (0x04, 0x03) is the physical voice (rytm-mods machines.md) in its
  low 3 bits; the machine is (voice byte & 0x78), matched exactly: 0x00
  PLAITS, 0x08 JUNO, 0x10 DAISYSP, 0x20 DSP SAMPLE, 0x40 DIGIDRUM, 0x48 6-OP
  FM (2026-10-09), 0x50 0x58 0x60 0x68 0x70 the user slots U1..U5 (2026-10-10:
  machine plugins, docs/plugins.md - the project's /.rytm/slots/<PROJECT>.txt
  names the machine each slot plays; P1..P16 go to the plugin's params(), the
  trig's note and velocity to its note_on(), 0x11 to its note_off(); a host
  without plugins plays them as PLAITS); any other value is taken as PLAITS. In detail: 0x10 when the
  track plays DAISYSP (machine 51), 0x20 for DSP SAMPLE (machine 45: P1..P8 ENG
  STR BRI DMP MIX FB TUN LEV; P9..P16 its SD SAMPLE page by word: 17 TUN 18 FIN
  19 SMP 20 FLD 21 STA 22 END 23 LOP 24 LEV, a file on the host's card ringing
  the resonator), 0x40 for DIGIDRUM (soejrd/digidrum's TRX/EFM machines,
  sim/digidrum/dd_voice.h: P1 ENG in 11 zones (DIGIDRUM_ENGINES), P2..P8 the engine's controls
  1..7, control 8 at the engine's default, P9..P16 the sound's own sample page
  - its sample layer plays under the drum; the engine's PTCH sets the pitch,
  the note is not used, the voice's AMP the level), 0x08 for JUNO (a Juno-106 voice,
  sim/juno/juno_voice.h: P1..P8 PAT CUT RES ENV ATK DEC REL LEN - PAT the
  factory patch 0..127, CUT..REL the patch's VCF FREQ, RES, ENV, A, D, R
  sliders as absolute values 0..127 (2026-10-09; before: offsets, 64 = the
  patch) - 15 puts a patch's own on the knobs -, LEN the gate (127: until the
  note-off 0x11, or the next trig); the note the trig's; when
  the project has a JUNO list - Juno-106 patch dumps from the card's /JUNO,
  added with LOAD TO PROJ - PAT steps through it instead of the factory
  patches: slot min(PAT, total - 1)), 0x48 for 6-OP FM (machine 40,
  2026-10-09: Plaits' six-op FM engine playing DX7 voices, sim/plaits/
  fm_machine.h: P1..P8 PAT TIM MOR DEC LPG TUN LEV LEN, P9..P16 unused - PAT
  the voice, entry min(PAT, total - 1) of the effective FM list (below),
  taken at a trig (a p-locked PAT too) or when the voice becomes FM: a 0x03's
  PAT waits for the next trig, as loading a voice cuts the sounding note; TIM
  Plaits' TIMBRE and MOR its MORPH, offsets on the voice as stored (64 = as
  stored); DEC and LPG the decay and colour of Plaits' low-pass gate after the
  voice, pinged by each trig (DEC 127: no LPG, the voice's own envelopes
  alone, so held notes sustain); TUN semitones from 64 (as PLAITS); LEV the
  output level; LEN the gate as JUNO's (127: until the note-off 0x11 or the
  next trig; lower a timed gate, 20 ms * 2^(LEN/14), or the note-off,
  whichever first); the note the trig's, the velocity the DX7's; no knob sync
  for FM, nothing like 15); else PLAITS, machine 44 (ENG in 17 zones:
  PLAITS_MODELS, Plaits' models without its 6-OP FMs, NOISE and drums).
  PLAITS: P1..P8
  ENG HRM TIM MOR DEC LPG TUN LEV, P9..P16 its page 2 (FM FIN AUX TMD MMD VEL
  GLD VOL). DAISYSP: P1..P8 ENG STR BRI DMP INP EXC TUN LEV (INP: INT, NAT or
  NOISE - played by the Rytm on the voice itself - or another analog voice's
  Overbridge output, channels 2..9, exciting it), P9..P16 its EXCITER page by
  sample word 17..24: XTN XDC FB OUT XP4 XLV XP5 WID (OUT >= 64: resonance to
  MAIN, else to the voice with FB of the loop kept). [Older: P9..P16 were then
  the sound's own sample page). ENG spans 0..127 in equal model zones,
  eng_zone(value, N) = min((value * N) >> 7, N - 1) on the value's integer
  part (2026-10-09, curated: N = 17 PLAITS, 11 DIGIDRUM, 3 DAISYSP and DSP
  SAMPLE - below; before: 21, 16 and 4). Each P is 14-bit, two
  bytes: the value 0..127, then its fraction in 1/128 steps (the stored
  parameter's low byte; slides and the LFO move it). LFO modulation included.

DSP SAMPLE's SD SAMPLE page picks from the project's pool (rytm-mods
0021/0022/0023, 2026-10-09): SMP (word 19, the value 0..127) is pool slot SMP -
0 OFF (no sample, silent), N slot N; an empty slot, or N beyond the pool, is
silent. FLD (word 20) is not used - except by a host with PSRAM (docs/
psram.md, 2026-10-09): there it is MOD, DSP SAMPLE's playback mode in 7
zones, (v * 7) >> 7: ONE LOOP SLICE GRAIN STRCH PLUCK WAVE (ONE = the
streamed voice's behaviour; STA END LOP mean per mode what psram.md's table
says).

Retired: the folder walk (2026-10-08) - 09 <list> <track> (names), 0A (the
host's name lists), 0B / 0C <track> 00 (YES into a subfolder, NO up). Hosts
still accept 09/0B/0C from older firmware and ignore them; 0A is not sent.

The TEENSY folder in the Rytm's own file browser (rytm-mods 0023, 2026-10-08):

    F0 7D 52 54 0D <folder hi> <lo> F7         Rytm -> host: list this folder
                                               (0 = the card's /Samples)
    F0 7D 52 54 0E <folder hi lo> <parent hi lo> <first hi lo> <total hi lo>
       { <kind> <id hi lo> <size: 4 x 7 bits, high first> <name len> <name> } F7
                                               host -> Rytm, 6 entries a message:
                                               kind 1 a subfolder (id: its folder
                                               number), 0 a file (id: position);
                                               names up to 23 chars, no .wav
    F0 7D 52 54 0F <folder hi lo> <file hi lo> F7
                                               Rytm -> host: LOAD TO PROJ on a
                                               TEENSY file - add it to the pool's
                                               first empty slot (or a new one at
                                               the end), unless it is there
                                               already; then 14
    F0 7D 52 54 10 <name> F7                   Rytm -> host: the Rytm's project
                                               (its name, up to 23 chars); the
                                               pool is the project's own
                                               (/.rytm/pools/<name>.txt: slot N is
                                               line N-1, a path under /Samples,
                                               an empty line an empty slot).
                                               Sent before each 0F / 12 and every
                                               few seconds; 14 follows the first
                                               10 and a changed project, not a
                                               repeat
    F0 7D 52 54 12 <slot> <folder hi lo> <file hi lo> F7
                                               Rytm -> host: ASSIGN - pool slot
                                               1..127 = that TEENSY file (folder
                                               and file as in 0D/0E/0F), whatever
                                               it held, even if the file is in
                                               another slot too; the pool file
                                               grows with empty lines as needed.
                                               A /JUNO patch is ignored. Then 14
    F0 7D 52 54 13 F7                          Rytm -> host: ASK - send 14 and
                                               16 now
    F0 7D 52 54 14 <replace 1/0> <count>
       { <slot> <folder hi lo> <file hi lo> <13 name bytes> } F7
                                               host -> Rytm: the pool table,
                                               non-empty slots in slot order, 6
                                               a message; replace 1 on a push's
                                               first message (the Rytm clears its
                                               table), 0 on the rest; an empty
                                               pool is one message, 1, count 0.
                                               Name: the file name without
                                               extension, ASCII 0x20..0x7e (else
                                               '_'), cut to 13, NUL padded.
                                               Folder/file 0x3fff (7f 7f) each:
                                               the file is not on the card now

JUNO (rytm-mods 0021, 2026-10-09), host -> Rytm:

    F0 7D 52 54 15 <voice 0..7> <CUT> <RES> <ENV> <ATK> <DEC> <REL> F7
        KNOBS: a JUNO voice's patch changed - its PAT (a 0x03 report) picks
        another patch, the first report or trig that makes the voice JUNO, or
        the JUNO list entry PAT points at changed; not on a 0x04 trig whose
        p-locked PAT differs. The values (0..127) are the patch's own VCF FREQ,
        RES, ENV, A, D, R (bytes 5, 6, 7, 11, 12, 14 of its 18): the Rytm
        writes them into the track's sound, so its knobs show the patch. Until
        a report carries all six as sent (or ~300 ms passed) the host ignores
        the six knobs' incoming values and plays the patch's own.
    F0 7D 52 54 16 <replace 1/0> <total> { <slot> <13 name bytes> } F7
        NAMES: the project's JUNO list, 8 entries a message; total = its
        slots (empty ones included; 0: no list - the Rytm shows the factory
        names A11..B88), entries every slot 0..total-1 in order; replace 1 on
        a push's first message (the Rytm clears its list), 0 on the rest;
        total 0 is one message, 1, 0, no entries. Name: the patch's, ASCII
        0x20..0x7e (else '_'), cut to 13, NUL padded; an empty slot 13 NULs.
        Sent with every 14 after a new project / the first 10, on 13, and
        after a 0F that added a JUNO patch to the list. The list holds at
        most 127 slots (PAT 0..126; 127 plays the last).

6-OP FM (rytm-mods 0021 machine 40, 2026-10-09):

    F0 7D 52 54 17 <replace 1/0> <total> { <slot> <13 name bytes> } F7
        host -> Rytm, FM NAMES: the effective FM list's names, layout and
        rules as 16 (8 entries a message, every slot 0..total-1, replace 1 on
        a push's first message, an empty slot 13 NULs; total <= 127). Sent
        after a new project / the first 10 (with 14 and 16), after a 0F that
        added a DX7 voice to the FM list, and on 18.
    F0 7D 52 54 18 F7
        Rytm -> host, FM ASK: send 17 now (the Rytm's PAT text asks when its
        name cache, shared between JUNO and FM, holds the other list).

The effective FM list: the project's FM list (/.rytm/fm/<PROJECT>.txt, a
line per slot: the bank's MD5 as 32 hex digits, a tab, the voice 0..31, a
tab, its name; an empty line an empty slot, silent) when it has any slot,
else Plaits' 96 ROM voices (its three DX7 banks, in order). The MD5 is the
bank's 4096 voice bytes' (without a .syx's header and checksum), so a moved or
renamed bank is found again; Plaits' ROM banks count as banks too. The card's
/DX7 holds packed 32-voice DX7 banks (4104-byte .syx, F0 43 0n 09 20 00 +
4096 bytes + checksum + F7, or a bare 4096-byte file), indexed at boot (an
MD5 and the 32 names per bank, cached in /.rytm/dx7.bin behind a hash of
/DX7's listing). In TEENSY a folder DX7 follows JUNO: its subfolders mirror
/DX7's, and each bank is a folder of its 32 voices (kind 0, size 128, named
by the voice's 10-character name); 0F on a voice adds it to the project's FM
list (its first empty slot or a new one; not again if it is there), then 17.
0F on anything else under DX7, and 12 on any JUNO or DX7 entry, is ignored.
TEENSY folder numbers: /Samples' first, then /JUNO's (folders and banks),
then /DX7's (folders and banks, breadth first in the card's order).

Folders from cards.folders() on are /JUNO's (core/presets/juno_lib.h),
shown as a folder JUNO first in TEENSY: their entries are Juno-106 patches
(kind 0, size 24); 0F on one adds it to the project's JUNO list
(/.rytm/juno/<PROJECT>.txt: 18 bytes as hex and the name per slot). A .syx
holding several patch messages (a bank, e.g. 128 dumps; 2026-10-09) is a
subfolder named after the file, its entries the patches in file order, named
by the dump's patch number and the file ("A11 Factory" .. "B88 Factory", "#N
<file>" for a 31 manual dump); 0F on one adds that patch alike.
Folder numbers: breadth first from /Samples, only folders with .wav files below,
subfolders and files in name order (the Teensy's index; the simulator alike).

Looper mode (rytm-mods 0022-looper):

  Rytm -> Teensy  cmd 0x05 MODE:  F0 7D 52 54 05 <on 0/1> <page> F7
                  cmd 0x06 PAD:   F0 7D 52 54 06 <slot> <down 1/0> F7
                                  (slot = page * 12 + pad, pads BD..CB)
  Teensy -> Rytm  cmd 0x07 SLOT:  F0 7D 52 54 07 <slot> <state> F7
                                  state 0 empty, 1 queued, 2 recording,
                                  3 playing, 4 stopped; the pad shows it
                  cmd 0x08 MUTES: F0 7D 52 54 08 <on 0/1> F7
                                  1 keep the mutes and mute all tracks (a loop
                                  took over), 0 restore them

cmd 0x02 TRIG (a trig without parameters) is unused, kept reserved.

The AUDIO direction is unused: Overbridge's own audio stream carries the voices
(docs/overbridge-voice-inputs.md).

User slots on the Rytm (2026-10-10; rytm-mods 0021's machines 35..39 as U1..U5,
docs/superpowers/specs/2026-10-10-user-slots-rytm-design.md 2.1). Every message
carries a version byte (1) right after the command; a receiver ignores a version
or a command it does not know. Text ASCII 0x20..0x7e (else '_'), NUL padded.
Generations 1..127 per slot, kept by the Teensy, bumped whenever what 1A says
changes; 0 = the Rytm knows nothing.

    19 <ver> <slot 1..5|0> <gen> <what> <arg>          Rytm -> Teensy, ASK
        what 0 the slot's metadata (1A; "unchanged" when gen matches), 1 enum
        names of knob arg 0..15 (1B list arg), 2 the slot's preset list page arg
        (16 names a page, 1B 0x20), 3 the installed machines (slot 0, 1B 0x21)
    1A <ver> <slot> <gen> <part> ...                   Teensy -> Rytm, METADATA
        part 0x7F unchanged; part 0: <state> <flags> <nknobs> <nparts> <name 10>
        <short 4> <p2 title 13> [<fp hi> <fp lo>] (the slot's plugin_fp, 2026-10-10;
        absent: 0); parts 1..nparts-1: up to 4 knobs of 24 bytes,
        <p> <look> <kind> <role> <min> <max> <default> <enum count> <long 12>
        <short 4>. State 0 EMPTY 1 READY 2 LOADING 3 MISSING 4 BAD 5 NO_ROOM
        6 MUTED; flags bit 0 page 2 = params, 1 the missing machine is on the
        card's /MACHINES, 2 presets exist; look 0 dial 1 centred 2 fader 3 list;
        kind 0 number 1 bipolar 2 enum 3 list 4 note 5 time 6 preset 7 pool;
        role 0 free 1 preset 2 offset
    1B <ver> <slot> <list> <gen> <replace> <total> {<index> <13 name bytes>}
                                                       Teensy -> Rytm, LIST NAMES
        8 a message, rules as 16/17; list 0x00..0x0F knob list's enum names,
        0x20 the slot's presets, 0x21 the installed machines (slot 0, gen = the
        store's)
    1C <ver> <voice 0..7> <slot> <n> {<knob 0..15> <value 0..127>}
                                                       Teensy -> Rytm, KNOBS
    1D <ver> <gen U1> .. <gen U5> <installed gen> [<slot> <result>]
                                                       Teensy -> Rytm, SLOTS
        result 0 READY 1 NOT INSTALLED 2 NO ROOM 3 STOP TO INSTALL 4 NOT ON CARD
        5 BAD 6 LOADING
    1E <ver> <slot 1..5> <op> <arg> <installed gen>    Rytm -> Teensy, ASSIGN
        op 0 assign entry arg of the installed list, 1 clear, 2 install the
        slot's missing machine from the card's /MACHINES

Sidecar send FX (2026-10-16, rytm-mods 0025-sendfx-report; docs/superpowers/specs/
2026-10-16-sidecar-send-fx-storage.md). 0x1F is reserved for the profile message
(2026-10-11-platform-architecture-design.md), so the next free id is 0x20:

    20 <ver 1> <track 0..11> <hi> <lo>                 Rytm -> host, SENDFX
    20 <ver 1> 12 {<hi> <lo>} x 6
        the trig array's words (after p-locks, before the LFO) as the trig loop
        last wrote them: tracks 0..11 sound index 0, track 12 (FX) FX words 0, 29,
        38, 39, 40, 41. hi = word bits 8..14, lo = bits 1..7 (a stock lock's value
        and its fine slot). Sound index 0: hi = SEND FX 1, lo = SEND FX 2 (the
        storage spec, section 6). Sent from the audio interrupt in the block a
        changed value's trig plays, and one track in turn every `rr` blocks
        (default 64, ~43 ms) so a host that attaches has every value within ~0.6 s.
    20 <ver 1> 13 <tag1 hi7> <tag1 lo7> <tag2 hi7> <tag2 lo7>
        the kit's 14-bit user-effect tags for FX 1 / FX 2 (0 = none; proposed for
        M4, non-lockable kit storage to be chosen with the M4 agent).

    Track 12's values (2026-10-16, user: the TYPE lives in the kit, so it travels
    with the kit and its patterns): each custom FX = TYPE + P1..P5, one 7-bit value
    each. FX 1: TYPE = word 29 hi, P1 = 29 lo, P2/P3 = 38 hi/lo, P4/P5 = 39 hi/lo.
    FX 2: TYPE = word 0 hi, P1 = 0 lo, P2/P3 = 40 hi/lo, P4/P5 = 41 hi/lo. TYPE and P1
    sit in the stock-stored words (option A), P2..P5 need 0025's 38..41 storage
    (B). A lo half is a lock's fine slot: a coarse-only lock leaves it 0. (Mapping
    proposed by the M2 Sidecar side; the M4 page layout to confirm.)
    TYPE ids (stable, SENDFX_TYPES): 0 off; 1..63 built in, no tag (1 CHORUS,
    2 COMB, 16 CRUSH, 17 RINGMOD, 18 HALO in every build, the Teensy too; 6 RINGS,
    8 CLOUDSEED, 15 STUTTER on the Pi; the rest reserved for planned effects - a Sidecar without one
    plays silence and reports it MISSING); 64..67 the user-FX slots UFX1..UFX4,
    as the user machines' U1..U5: the project's slot map on the Sidecar card
    (/.rytm/fxslots/<project>.txt: line n = "<tag 4 hex> <name>" or empty) says
    which effect a slot holds; the kit's tag (track 13) must match it, else that
    FX is silent and MISSING (!NAME). A TYPE change (a kit load, a p-lock) is
    never abrupt: the old effect fades out over 10 ms, the new one starts from
    a clean state and fades in over 10 ms.

    21 <ver 1> <replace> <total> {<id> <13 name bytes>}   host -> Rytm, SENDFX NAMES
        the TYPE knob's list (OFF, the built-in effects this Sidecar plays, the
        project's filled UFX slots), 8 entries a message, rules as 16/17; sent on
        a 0x23 and on a project change (0x10)
    22 <ver 1> <fx 0|1> <id> <state> <name 10>             host -> Rytm, SENDFX STATE
        FX k's TYPE as the Sidecar resolves it; state as 1A's (0 EMPTY = off,
        1 READY, 3 MISSING: the Rytm shows !NAME); sent when FX k's id or state
        changes (0x20 track 12/13), on 0x23 and on a project change
    23 <ver 1>                                             Rytm -> host, SENDFX ASK
    24 <ver 1> <id> <label 4> x 4 {<knob | show << 2 | step << 3> <lo> <base> <name 5>}   host -> Rytm, SENDFX LABELS
        TYPE id's knob labels for P2..P5 (ASCII, NUL padded, as the stock pages'
        4) and its zones (at most 12): knob 0..3 = P2..P5, a zone runs from lo to
        the next zone's lo - 1 of that knob (or 127), lo ascending; the Rytm
        shows its name, with show also v - lo + base ("MODAL 12"); a knob without
        zones shows its number. step (2026-10-18, set on every zone of a knob that
        is a pure zone selector): the Rytm (0029 sfx-enum-knobs) steps that knob one
        zone per detent, as its own enum knobs, and stores each zone's lo; without
        it the knob stays continuous (an older Rytm ignores the bit). One message a TYPE this Sidecar plays (OFF has
        none), after the 21s (a 21 with replace clears the Rytm's labels), on
        0x23 and on a project change; tables next to each effect
        (core/fx/knob_labels.h). Without any 24 the Rytm keeps P2..P5 generic.
        P1 is ROUTE on every TYPE: the Rytm shows PRE (0..63) / POST (64..127)
        itself.

Slot fingerprints (2026-10-19, rytm-mods 0021 slot-resolve; docs/user-machines-pi.md, "Which machine a slot
plays"). 0x25 is the next free id (0x1F stays reserved for the profile message, 0x20..0x24 are the send FX):

    25 <ver 1> <seq> <flags> <n> {<slot 1..5> <fp hi7> <fp lo7> <count>}   Rytm -> host, SLOT PRINTS
        the fingerprints (the sound tag, plugin_fp) the project's sounds use on each user slot: every
        sound of the 128 resident kits and the sound pool whose machine is U1..U5 and whose tag is nonzero,
        one entry per distinct (slot, fp), count = how many sounds (1..127, capped), slots ascending, at
        most 16 entries (flags bit 0: more were left out). n 0 = no tagged U-sound in the project. Sent
        right after a 10 <project> under one MIDI lock (try-lock: never waits), once a project or kit load
        has settled, and again every ~5 s with seq unchanged while nothing changed (a host that came up
        later); seq (0..127) moves on whenever the content changes. A host resolves the project's slots
        from the first 25 after a 10 and from any 25 whose content differs from the last one it used; a
        repeat is ignored. An older host ignores 0x25; an older Rytm never sends it (the host then
        resolves lazily from the trigs' tags, 0x04/0x03).

IN L/R source (2026-10-18, rytm-mods 0030-fx-in; docs/recorder-mini.md, "IN L/R"). 0x26 / 0x27:

    26 <v>      host -> Rytm, FXIN: 1 the delay + reverb return (stereo) on the Overbridge IN L/R (channels 10 / 11)
                instead of the codec inputs, 0 the stock AUDIO IN, 0x7F ask (no change)
    27 <s>      Rytm -> host, FXIN_STATE: 1 on, 0 off; answers every 26 (asks too) and is sent unasked each time the
                Rytm's USB port comes up (it resets itself to off on a disconnect: the cue to send 26 01 again).
                An older Rytm (no 0030) never answers: the host shows "not supported by this Rytm build".
                No version byte. Never send channels 10 / 11 back into the Rytm through Overbridge (feedback).
"""

HDR = bytes([0xF0, 0x7D, 0x52, 0x54])
AUDIO, TRIG, PARAMS, TRIG_PARAMS = 0x01, 0x02, 0x03, 0x04
NOTE_OFF = 0x11
POOL_ASSIGN, POOL_ASK, POOL_TABLE = 0x12, 0x13, 0x14
JUNO_KNOBS, JUNO_NAMES = 0x15, 0x16
FM_NAMES, FM_ASK = 0x17, 0x18
# The curated ENG lists (rytm-mods 0021, 2026-10-09): the knob's value (P's
# integer part, 0..127) in N equal zones: zone = (value * N) >> 7, at most N - 1.
# PLAITS N = 17, zone -> Plaits model PLAITS_MODELS[zone]: VA VCF, PHASE DIST,
# WAVE TERRAIN, STRING MACH, CHIPTUNE, VA, WAVESHAPE, 2-OP FM, GRANULAR,
# HARMONIC, WAVETABLE, CHORDS, SPEECH, SWARM, PARTICLE, STRING, MODAL (no 6-OP
# FM 1-3: the 6-OP FM machine; no NOISE; no drums: DIGIDRUM). DIGIDRUM N = 11:
# EFM-BD EFM-SD EFM-XT EFM-CP EFM-RS EFM-CB EFM-HH EFM-CY TRX-RS TRX-CB TRX-CL.
# DAISYSP and DSP SAMPLE N = 3: MODAL STRING RESON.
PLAITS_MODELS = (0, 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20)
DIGIDRUM_ENGINES = ("EFM-BD", "EFM-SD", "EFM-XT", "EFM-CP", "EFM-RS", "EFM-CB", "EFM-HH", "EFM-CY",
                    "TRX-RS", "TRX-CB", "TRX-CL")
DAISY_MODELS = ("MODAL", "STRING", "RESON")


def eng_zone(value, n):
    """A P value (0..127.99) -> its ENG zone 0..n-1."""
    v = max(0, min(127, int(value)))
    return min((v * n) >> 7, n - 1)


# the machine: voice byte & MACHINE_MASK, matched exactly
MACHINE_MASK = 0x78
PLAITS, JUNO, DAISYSP, DSP_SAMPLE, DIGIDRUM, FM = 0x00, 0x08, 0x10, 0x20, 0x40, 0x48
USER_SLOTS = (0x50, 0x58, 0x60, 0x68, 0x70)   # U1..U5: machine plugins (teensy/voiceloop, RYTM_PLUGINS)
JUNO_KNOB_BYTES = (5, 6, 7, 11, 12, 14)   # CUT RES ENV ATK DEC REL in a patch's 18
BLOCK = 32


def audio_block(voice: int, seq: int, samples) -> bytes:
    """samples: 32 ints in -8192..8191 (14-bit)."""
    out = bytearray(HDR)
    out += bytes([AUDIO, voice & 7, seq & 0x7F])
    for s in samples:
        v = max(-8192, min(8191, int(s))) & 0x3FFF
        out += bytes([v >> 7, v & 0x7F])
    out.append(0xF7)
    return bytes(out)


def parse(msg: bytes):
    """-> (cmd, payload bytes) for an RT message, else None."""
    if len(msg) < 6 or msg[:4] != HDR or msg[-1] != 0xF7:
        return None
    return msg[4], msg[5:-1]


def decode_note_off(payload: bytes):
    """NOTE_OFF's payload -> the voice 0..7."""
    return payload[0] & 7


def decode_audio(payload: bytes):
    voice, seq = payload[0], payload[1]
    data = payload[2:]
    samples = []
    for i in range(0, len(data), 2):
        v = data[i] << 7 | data[i + 1]
        samples.append(v - 0x4000 if v & 0x2000 else v)
    return voice, seq, samples


def pool_assign(slot: int, folder: int, file: int) -> bytes:
    """12 ASSIGN: pool slot 1..127 = TEENSY folder's file."""
    return HDR + bytes([POOL_ASSIGN, slot & 0x7F, (folder >> 7) & 0x7F, folder & 0x7F,
                        (file >> 7) & 0x7F, file & 0x7F, 0xF7])


def decode_pool_table(payload: bytes):
    """14's payload -> (replace, [(slot, folder, file, name)])."""
    replace, count, out = payload[0], payload[1], []
    for k in range(count):
        e = payload[2 + 18 * k:2 + 18 * (k + 1)]
        name = bytes(e[5:18]).rstrip(b"\0").decode("ascii")
        out.append((e[0], e[1] << 7 | e[2], e[3] << 7 | e[4], name))
    return bool(replace), out


def name13(name: str):
    """A name for 14 / 16: ASCII 0x20..0x7e (other bytes of its UTF-8 '_'),
    cut to 13 bytes, NUL padded."""
    b = [c if 0x20 <= c <= 0x7E else ord("_") for c in name.encode()[:13]]
    return b + [0] * (13 - len(b))


def juno_knobs(voice: int, patch18) -> bytes:
    """15 KNOBS: a patch's CUT RES ENV ATK DEC REL to the Rytm's voice."""
    return HDR + bytes([JUNO_KNOBS, voice & 7] + [patch18[i] & 0x7F for i in JUNO_KNOB_BYTES] + [0xF7])


def decode_juno_knobs(payload: bytes):
    """15's payload -> (voice, [CUT, RES, ENV, ATK, DEC, REL])."""
    return payload[0], list(payload[1:7])


def machine(voice_byte: int) -> int:
    """A 0x04/0x03 report's voice byte -> its machine (PLAITS, JUNO, DAISYSP,
    DSP_SAMPLE, DIGIDRUM, FM); any other value PLAITS."""
    m = voice_byte & MACHINE_MASK
    return m if m in (JUNO, DAISYSP, DSP_SAMPLE, DIGIDRUM, FM) else PLAITS


def juno_names(names, cmd=JUNO_NAMES) -> list:
    """16 NAMES messages for a JUNO list (cmd FM_NAMES: 17 for the FM list):
    names[slot] a name or None (empty). An empty list is one message,
    replace 1, total 0."""
    total = len(names)
    msgs = []
    for first in range(0, max(total, 1), 8):
        m = bytearray(HDR) + bytes([cmd, 1 if first == 0 else 0, total])
        for slot in range(first, min(first + 8, total)):
            m += bytes([slot] + (name13(names[slot]) if names[slot] is not None else [0] * 13))
        m.append(0xF7)
        msgs.append(bytes(m))
    return msgs


def fm_names(names) -> list:
    """17 FM NAMES messages for the effective FM list (as juno_names)."""
    return juno_names(names, FM_NAMES)


def decode_juno_names(payload: bytes):
    """16's (or 17's) payload -> (replace, total, [(slot, name)])."""
    replace, total, out = payload[0], payload[1], []
    for k in range((len(payload) - 2) // 14):
        e = payload[2 + 14 * k:2 + 14 * (k + 1)]
        out.append((e[0], bytes(e[1:14]).rstrip(b"\0").decode("ascii")))
    return bool(replace), total, out


# ---- user slots (2026-10-10): 19..1E, see the docstring ----
ASK, META, LIST, KNOBS, SLOTS, ASSIGN = 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E
PROTO_VER = 1
ASK_META, ASK_ENUM, ASK_PRESETS, ASK_INSTALLED = 0, 1, 2, 3
LIST_PRESETS, LIST_INSTALLED = 0x20, 0x21
ST_EMPTY, ST_READY, ST_LOADING, ST_MISSING, ST_BAD, ST_NO_ROOM, ST_MUTED = range(7)
ST_NAMES = ("EMPTY", "READY", "LOADING", "MISSING", "BAD", "NO_ROOM", "MUTED")
FLAG_PARAMS, FLAG_ON_CARD, FLAG_PRESETS = 1, 2, 4
RES_READY, RES_NOT_INSTALLED, RES_NO_ROOM, RES_STOP_TO_INSTALL, RES_NOT_ON_CARD, RES_BAD, RES_LOADING = range(7)
OP_ASSIGN, OP_CLEAR, OP_INSTALL = 0, 1, 2
LOOKS = ("dial", "centred", "fader", "list")
KINDS = ("number", "bipolar", "enum", "list", "note", "time", "preset", "pool")
ROLES = ("free", "preset", "offset")
META_UNCHANGED = 0x7F


def _msg(cmd, body):
    return bytes(HDR) + bytes([cmd, PROTO_VER]) + bytes(body) + b"\xf7"


def text_bytes(s, n):
    """n bytes of text: ASCII 0x20..0x7e (else '_'), cut, NUL padded; a NUL ends it."""
    out = []
    for c in s.encode()[:n]:
        if c == 0:
            break
        out.append(c if 0x20 <= c <= 0x7E else ord("_"))
    return out + [0] * (n - len(out))


def _text(b):
    return bytes(b).split(b"\0")[0].decode("ascii", "replace")


def _ok(payload, n=None, at_least=None):
    """A payload of our version and of length n (or at least at_least)."""
    if not payload or payload[0] != PROTO_VER:
        return False
    return len(payload) == n if n is not None else len(payload) >= at_least


def _ix(table, v):
    return table.index(v) if isinstance(v, str) else int(v)


def ask(slot, gen, what, arg=0):
    """19 ASK (Rytm -> Teensy)."""
    return _msg(ASK, [slot & 0x7F, gen & 0x7F, what & 0x7F, arg & 0x7F])


def assign(slot, op, arg, installed_gen):
    """1E ASSIGN (Rytm -> Teensy)."""
    return _msg(ASSIGN, [slot & 0x7F, op & 0x7F, arg & 0x7F, installed_gen & 0x7F])


def decode_ask(payload):
    if not _ok(payload, 5):
        return None
    return dict(slot=payload[1], gen=payload[2], what=payload[3], arg=payload[4])


def decode_assign(payload):
    if not _ok(payload, 5):
        return None
    return dict(slot=payload[1], op=payload[2], arg=payload[3], installed_gen=payload[4])


def meta_unchanged(slot, gen):
    """1A part 0x7F: the Rytm's generation is current."""
    return _msg(META, [slot & 0x7F, gen & 0x7F, META_UNCHANGED])


def plugin_fp(pkg_id):
    """A plugin's fingerprint (the sound tag, 2026-10-10): FNV-1a 32 of the package id's bytes as stored (case
    kept; not the version: an upgrade keeps its sounds), mod 16383, + 1 - 1..16383 (14 bits; 0 means untagged).
    The Teensy's C++ plugin_fp must match: shared vectors in tests/test_protocol_us.py (spec 2.6)."""
    h = 0x811C9DC5
    for c in pkg_id.encode():
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h % 16383 + 1


def meta_messages(slot, gen, state, flags=0, name="", short="", p2_title="", knobs=(), fp=None):
    """1A part 0 and the knob parts. knobs: dicts p, look, kind, role, min, max, default,
    enum_count, long, short (look/kind/role by name or number; defaults dial, number, free,
    0, 127, 64, 0). fp: the slot's plugin fingerprint (plugin_fp), two more bytes at the end of
    part 0 (fp >> 7, fp & 0x7F); None: the older part 0 without them."""
    knobs = list(knobs)
    nparts = 1 + (len(knobs) + 3) // 4
    slot &= 0x7F
    out = [_msg(META, [slot, gen & 0x7F, 0, state & 0x7F, flags & 7, len(knobs) & 0x7F, nparts & 0x7F] + text_bytes(name, 10) +
                text_bytes(short, 4) + text_bytes(p2_title, 13) + ([] if fp is None else [fp >> 7 & 0x7F, fp & 0x7F]))]
    for part in range(1, nparts):
        body = [slot, gen & 0x7F, part & 0x7F]
        for k in knobs[4 * (part - 1):4 * part]:
            body += [x & 0x7F for x in (k["p"], _ix(LOOKS, k.get("look", 0)), _ix(KINDS, k.get("kind", 0)),
                                        _ix(ROLES, k.get("role", 0)), k.get("min", 0), k.get("max", 127),
                                        k.get("default", 64), k.get("enum_count", 0))]
            body += text_bytes(k.get("long", ""), 12) + text_bytes(k.get("short", ""), 4)
        out.append(_msg(META, body))
    return out


def decode_meta(payload):
    """1A's payload -> dict: slot, gen, part and unchanged=True, or part 0's state, flags, nknobs,
    nparts, name, short, p2_title, fp (0 from a part 0 without it), or a knob part's knobs
    (look/kind/role as names). None when malformed (a length that does not fit the part, a
    look/kind/role out of range)."""
    if not _ok(payload, at_least=4):
        return None
    d = dict(slot=payload[1], gen=payload[2], part=payload[3])
    if d["part"] == META_UNCHANGED:
        if len(payload) != 4:
            return None
        d["unchanged"] = True
        return d
    if d["part"] == 0:
        if len(payload) not in (4 + 31, 4 + 33):
            return None
        b = payload[4:]
        d.update(state=b[0], flags=b[1], nknobs=b[2], nparts=b[3], name=_text(b[4:14]), short=_text(b[14:18]),
                 p2_title=_text(b[18:31]), fp=b[31] << 7 | b[32] if len(b) == 33 else 0)
        return d
    d["knobs"] = []
    b = payload[4:]
    if not b or len(b) % 24 or len(b) > 4 * 24:
        return None
    for i in range(0, len(b), 24):
        e = b[i:i + 24]
        if e[1] >= len(LOOKS) or e[2] >= len(KINDS) or e[3] >= len(ROLES):
            return None
        d["knobs"].append(dict(p=e[0], look=LOOKS[e[1]], kind=KINDS[e[2]], role=ROLES[e[3]], min=e[4], max=e[5],
                               default=e[6], enum_count=e[7], long=_text(e[8:20]), short=_text(e[20:24])))
    return d


def list_messages(slot, lst, gen, names, first=0, total=None, replace=True):
    """1B LIST NAMES: entries first.. (names[i] is entry first + i; None an empty entry), 8 a
    message; replace on the first message only (when replace); total defaults to first + len(names).
    An empty list is one message."""
    total = first + len(names) if total is None else total
    msgs = []
    for off in range(0, max(len(names), 1), 8):
        body = [slot & 0x7F, lst & 0x7F, gen & 0x7F, 1 if replace and off == 0 else 0, max(0, min(total, 127))]
        for i, n in enumerate(names[off:off + 8]):
            body += [(first + off + i) & 0x7F] + (text_bytes(n, 13) if n is not None else [0] * 13)
        msgs.append(_msg(LIST, body))
    return msgs


def decode_list(payload):
    if not _ok(payload, at_least=6) or (len(payload) - 6) % 14 or len(payload) - 6 > 8 * 14:
        return None
    b = payload[6:]
    entries = [(b[i], _text(b[i + 1:i + 14])) for i in range(0, len(b), 14)]
    return dict(slot=payload[1], list=payload[2], gen=payload[3], replace=bool(payload[4]), total=payload[5],
                entries=entries)


def knobs(voice, slot, pairs):
    """1C KNOBS: (knob 0..15, value 0..127) pairs for the voice's owner track's slot machine."""
    pairs = list(pairs)[:16]
    return _msg(KNOBS, [voice & 7, slot & 0x7F, len(pairs)] + [x for k, v in pairs for x in (k & 0x0F, v & 0x7F)])


def decode_knobs(payload):
    if not _ok(payload, at_least=4) or len(payload) != 4 + 2 * payload[3]:
        return None
    n = payload[3]
    return dict(voice=payload[1], slot=payload[2], knobs=[(payload[4 + 2 * i], payload[5 + 2 * i]) for i in range(n)])


def slots(gens, installed_gen, slot=0, result=0):
    """1D SLOTS: the five generations and the installed list's; slot 1..5 adds the answer pair to a 1E."""
    return _msg(SLOTS, [g & 0x7F for g in gens] + [installed_gen & 0x7F] + ([slot & 0x7F, result & 0x7F] if slot else []))


def decode_slots(payload):
    if not _ok(payload, at_least=7) or len(payload) not in (7, 9):
        return None
    return dict(gens=list(payload[1:6]), installed=payload[6],
                answer=(payload[7], payload[8]) if len(payload) == 9 else None)


# ---- the reports' sound tag (2026-10-10): the Rytm (rytm-mods 0021 ext_render) appends the owner track's
# sound word 0 - the plugin fingerprint a U-sound was saved with, 0 untagged - as two 7-bit bytes ----
def _p14(ps):
    return [b for v in ps for b in (v >> 7 & 0x7F, v & 0x7F)]


def _tag(tag):
    return [] if tag is None else [tag >> 7 & 0x7F, tag & 0x7F]


def trig_params(voice_byte, note, vel, ps, tag=None):
    """0x04 TRIG+PARAMS: ps 16 (or 8) 14-bit values; tag None: no tag bytes (older firmware)."""
    return HDR + bytes([TRIG_PARAMS, voice_byte & 0x7F, note & 0x7F, vel & 0x7F] + _p14(ps) + _tag(tag) + [0xF7])


def params_report(voice_byte, ps, tag=None):
    """0x03 PARAMS: as trig_params, without the note."""
    return HDR + bytes([PARAMS, voice_byte & 0x7F] + _p14(ps) + _tag(tag) + [0xF7])


def _report(payload, head):
    """head bytes, then P1..P8 or P1..P16 and an optional 2-byte tag -> (params, tag), else None."""
    n = len(payload) - head
    if n not in (16, 32, 34):
        return None
    b = payload[head:]
    ps = [b[i] << 7 | b[i + 1] for i in range(0, min(n, 32), 2)]
    return ps, (b[32] << 7 | b[33] if n == 34 else 0)


def decode_trig_params(payload):
    """0x04's payload -> dict voice, machine (voice byte & MACHINE_MASK), note, vel, params, tag (0 absent)."""
    r = _report(payload, 3)
    if r is None:
        return None
    return dict(voice=payload[0] & 7, machine=payload[0] & MACHINE_MASK, note=payload[1], vel=payload[2],
                params=r[0], tag=r[1])


def decode_params(payload):
    """0x03's payload -> dict voice, machine, params, tag (0 absent)."""
    r = _report(payload, 1)
    if r is None:
        return None
    return dict(voice=payload[0] & 7, machine=payload[0] & MACHINE_MASK, params=r[0], tag=r[1])


# ---- Sidecar send FX (2026-10-16): 20 SENDFX, Rytm -> host (rytm-mods 0025-sendfx-report) ----
SENDFX = 0x20
SENDFX_FX_WORDS = (0, 29, 38, 39, 40, 41)   # track 12's words, in message order
FX_TRACK = 12
TAG_TRACK = 13
SENDFX_NAMES, SENDFX_STATE, SENDFX_ASK = 0x21, 0x22, 0x23
# stable TYPE ids (core/fx/send_fx.cpp kBuiltins); BUILT = the ones this release plays
SENDFX_TYPES = {0: "OFF", 1: "CHORUS", 2: "COMB", 3: "DELAY", 4: "PINGPONG", 5: "TAPE DLY", 6: "RINGS",
                7: "S4 RESO", 8: "CLOUDSEED", 9: "PLASMA", 10: "PLATE", 11: "HALL", 12: "SHIMMER",
                13: "FLANGER", 14: "PHASER", 15: "STUTTER", 16: "CRUSH", 17: "RINGMOD",
                18: "HALO"}
SENDFX_BUILT = (1, 2, 16, 17, 18, 6, 8, 15)   # 1, 2, 16, 17, 18 everywhere (the Teensy's small set); 6, 8, 15 the Pi
SENDFX_BUILT_TEENSY = (1, 2, 16, 17, 18)       # (and the Mac): a Teensy build reports them MISSING
SENDFX_STUTTER = 15   # a master insert, not a send (core/fx/stutter_fx.h): one FX slot at a time, a second MISSING
SENDFX_USER_FIRST, SENDFX_USER_SLOTS = 64, 4
# track 12 pairs (words 0, 29, 38, 39, 40, 41) -> FX k's [TYPE, P1..P5] as (pair, half 0 hi / 1 lo)
SENDFX_KNOBS = (((1, 0), (1, 1), (2, 0), (2, 1), (3, 0), (3, 1)), ((0, 0), (0, 1), (4, 0), (4, 1), (5, 0), (5, 1)))


def sendfx(track, words):
    """20 SENDFX (Rytm -> host): track 0..11 one 16-bit word (sound index 0), 12 six (FX words 0, 29, 38..41)."""
    return _msg(SENDFX, [track & 0x7F] + [b for w in words for b in ((w >> 8) & 0x7F, (w >> 1) & 0x7F)])


def sendfx_tags(tag1, tag2):
    """20 SENDFX track 13: the kit's 14-bit user-effect tags for FX 1 and 2."""
    return _msg(SENDFX, [TAG_TRACK, (tag1 >> 7) & 0x7F, tag1 & 0x7F, (tag2 >> 7) & 0x7F, tag2 & 0x7F])


def decode_sendfx(payload):
    """20's payload -> dict track, pairs [(hi, lo)], words [(hi << 8) | (lo << 1)] (track 13: tags); None if
    malformed. Track 12 also gives knobs: [[TYPE, P1..P5] for FX 1, FX 2]."""
    if not payload or payload[0] != PROTO_VER or len(payload) < 2:
        return None
    t, b = payload[1], payload[2:]
    if t > TAG_TRACK or len(b) != (12 if t == FX_TRACK else 4 if t == TAG_TRACK else 2) or any(x > 0x7F for x in b):
        return None
    pairs = [(b[i], b[i + 1]) for i in range(0, len(b), 2)]
    if t == TAG_TRACK:
        return dict(track=t, tags=[(h << 7) | lo for h, lo in pairs])
    d = dict(track=t, pairs=pairs, words=[(h << 8) | (lo << 1) for h, lo in pairs])
    if t == FX_TRACK:
        d["knobs"] = [[pairs[p][h] for p, h in k] for k in SENDFX_KNOBS]
    return d


def sendfx_names(entries) -> list:
    """21 SENDFX NAMES: entries [(id, name)] in list order, 8 a message (the first replaces)."""
    total, msgs = len(entries), []
    for first in range(0, max(total, 1), 8):
        body = [1 if first == 0 else 0, total]
        for i, nm in entries[first:first + 8]:
            body += [i] + name13(nm)
        msgs.append(_msg(SENDFX_NAMES, body))
    return msgs


def decode_sendfx_names(payload):
    """21's payload -> (replace, total, [(id, name)])."""
    if not payload or payload[0] != PROTO_VER:
        return None
    replace, total, b = payload[1], payload[2], payload[3:]
    out = [(b[k], bytes(b[k + 1:k + 14]).rstrip(b"\0").decode("ascii")) for k in range(0, len(b) - 13, 14)]
    return bool(replace), total, out


def sendfx_state(fx, type_id, state, name):
    """22 SENDFX STATE: FX fx (0, 1), its TYPE id, the state (ST_EMPTY, ST_READY, ST_MISSING), a 10-char name."""
    return _msg(SENDFX_STATE, [fx, type_id, state] + name13(name)[:10])


def decode_sendfx_state(payload):
    """22's payload -> dict fx, id, state, name; None if malformed."""
    if not payload or payload[0] != PROTO_VER or len(payload) != 14:
        return None
    return dict(fx=payload[1], id=payload[2], state=payload[3], name=bytes(payload[4:14]).rstrip(b"\0").decode("ascii"))


def sendfx_ask():
    """23 SENDFX ASK (Rytm -> host): answered by 21, the 24s and both 22s."""
    return _msg(SENDFX_ASK, [])


SENDFX_LABELS = 0x24
LABEL_CHARS, ZONE_CHARS, MAX_ZONES = 4, 5, 12
# The core's tables (core/fx/knob_labels.h users: send_fx.h, rings_fx.h, cloudseed_fx.h, stutter_fx.h): TYPE id ->
# (labels P2..P5, [(knob 0..3, lo, base, show, name, step)]). step: the knob is a pure zone selector (STUTTER ENG and
# LEN); zones with a number inside (show) stay continuous.
SENDFX_LABEL_SETS = {
    1: (("RATE", "DPTH", "DLY", "FDBK"), []),
    2: (("TUNE", "DCY", "DAMP", "SPRD"), [(1, 0, 0, 1, "POS", 0), (1, 64, 0, 1, "NEG", 0)]),
    16: (("RATE", "BITS", "DRV", "TONE"), []),
    17: (("FREQ", "SPRD", "SHPE", "LFO"), []),
    18: (("DCY", "MOD", "TONE", "SHIM"), [(0, 127, 0, 0, "FRZ", 0)]),   # HALO: DECAY 127 = FREEZE
    6: (("PTCH", "MODL", "BRIT", "DCY"), [(1, 0, 0, 1, "MODAL", 0), (1, 32, 0, 1, "MONO", 0),
                                          (1, 64, 0, 1, "STRNG", 0), (1, 96, 0, 1, "CHORD", 0)]),
    8: (("CHAR", "DCY", "DAMP", "MOD"), [(0, 0, 0, 1, "ROOM", 0), (0, 32, 0, 1, "HALL", 0), (0, 64, 0, 1, "PLATE", 0),
                                         (0, 96, 0, 1, "AMBNC", 0)]),
    15: (("ENG", "LEN", "D/P", "GATE"), [(0, 0, 0, 0, "OFF", 1), (0, 64, 0, 0, "ON", 1), (1, 0, 0, 0, "1/64", 1),
                                         (1, 19, 0, 0, "1/32", 1), (1, 37, 0, 0, "1/16", 1), (1, 55, 0, 0, "1/8", 1),
                                         (1, 74, 0, 0, "1/4", 1), (1, 92, 0, 0, "1/2", 1), (1, 110, 0, 0, "1BAR", 1),
                                         (2, 0, 0, 0, "NONE", 0), (2, 1, 1, 1, "DCY", 0), (2, 64, 1, 1, "PTCH", 0)]),
}
ZONE_STEP = 8   # the zone byte's step bit (knob | show << 2 | step << 3)


def sendfx_labels(type_id, labels, zones):
    """24 SENDFX LABELS: labels (4 strings, P2..P5), zones [(knob 0..3, lo, base, show, name[, step])] (<= 12); a
    5-tuple is an older Sidecar's zone (no step bit)."""
    if len(labels) != 4 or len(zones) > MAX_ZONES:
        raise ValueError("4 labels, at most 12 zones")
    body = [type_id & 0x7F]
    for lab in labels:
        body += text_bytes(lab, LABEL_CHARS)
    for z in zones:
        k, lo, base, show, name = z[:5]
        step = z[5] if len(z) > 5 else 0
        if not 0 <= k <= 3:
            raise ValueError("knob 0..3")
        body += [k | (4 if show else 0) | (ZONE_STEP if step else 0), lo & 0x7F, base & 0x7F] + \
            text_bytes(name, ZONE_CHARS)
    return _msg(SENDFX_LABELS, body)


def decode_sendfx_labels(payload):
    """24's payload -> (id, [4 labels], [(knob, lo, base, show, name, step)]); None if malformed."""
    if not payload or payload[0] != PROTO_VER or len(payload) < 18 or (len(payload) - 18) % 8:
        return None
    b = bytes(payload)
    txt = lambda x: x.split(b"\0")[0].decode("ascii")   # noqa: E731
    labels = [txt(b[2 + 4 * j:6 + 4 * j]) for j in range(4)]
    zones = [(b[k] & 3, b[k + 1], b[k + 2], (b[k] >> 2) & 1, txt(b[k + 3:k + 8]), (b[k] >> 3) & 1)
             for k in range(18, len(b), 8)]
    return b[1], labels, zones


def sendfx_steps(type_id, knob, sets=None):
    """The stored values of P(knob + 2) under TYPE type_id when the Rytm steps it (its zones' lo, in order), else None
    (a continuous knob: no step bit, or no labels for the type)."""
    sets = SENDFX_LABEL_SETS if sets is None else sets
    zs = [z for z in sets.get(type_id, ((), []))[1] if z[0] == knob]
    if not zs or not any(len(z) > 5 and z[5] for z in zs):
        return None
    return [z[1] for z in zs]


def sendfx_value_text(type_id, knob, v, sets=None):
    """What the Rytm shows for P(knob + 2) = v under TYPE type_id (the 0029 rule): a zone's name (with show, then
    v - lo + base, a space when it fits 7 characters), else the number; None without labels for the type."""
    sets = SENDFX_LABEL_SETS if sets is None else sets
    if type_id not in sets:
        return None
    zs = [z for z in sets[type_id][1] if z[0] == knob and z[1] <= v]
    if not zs:
        return str(v)
    lo, base, show, name = zs[-1][1:5]
    if not show:
        return name
    num = str(v - lo + base)
    return name + (" " if len(name) + 1 + len(num) <= 7 else "") + num


def sendfx_route_text(v):
    """P1 (ROUTE) as the Rytm shows it: PRE 0..63 (Overbridge OUT 3-4, before the analog dist/comp), POST 64..127."""
    return "PRE" if v < 64 else "POST"


# ---- Remote UI (2026-10-16, rytm-mods 0026-remote-ui; docs/superpowers/specs/2026-10-16-remote-ui-research.md) ----
# The Rytm as a terminal for pages the Sidecar draws: 0x30..0x3F (clear of 0x20..0x23 send FX; 0022's router passes
# this range to 0026 with rui=1). Every message carries <ver 1> after the command.
#   host -> Rytm  38 <seq> <col0 0..15> <page0 0..7> <ncols 1..16> <npages 1..8> <packed>   framebuffer bytes of a
#                    tile rectangle into staging: column-major, byte = page + 8 x column (page 7 = the top band,
#                    glass pixel (x, y from the top) = byte (7 - y // 8) + 8 x, bit y % 8), 8 x ncols x npages bytes
#                 39 <seq>          commit staging -> the screen (when seq is the 38s'); a partial frame never shows
#                 3A <page> <on>    open (1) / close (0) the remote page
#                 3C                keepalive: the Rytm leaves after 1 s without any valid 3x
#   Rytm -> host  30 <page> <on> <reason>   0 user (TRK + YES), 1 host (3A), 2 timeout, 3 local escape (TRK + YES)
#                 31 <code >> 7> <code & 0x7F> <down>   every key while the page is on (the firmware's key codes:
#                                           YES 0x44, NO 0x45, FUNC 0x80, TRK 0x86, pads 0x90..0x9B)
#                 32 <encoder> <delta + 64> <func>   turns summed per 50 ms tick, delta -63..63
# packed: 7 bytes -> 8, groups of up to 7 bytes, each a byte of their MSBs (bit i = byte i's bit 7) then the low 7s.
RUI_MODE, RUI_KEY, RUI_ENC = 0x30, 0x31, 0x32
RUI_RECT, RUI_COMMIT, RUI_OPEN, RUI_KEEPALIVE = 0x38, 0x39, 0x3A, 0x3C
RUI_TEXT, RUI_TEXT_CLEAR, RUI_ECHO = 0x3D, 0x3E, 0x3F   # 1021: text drawn by the Rytm's own fonts, the local echo
RUI_MASK = 0x37   # 1022: the catch mask (which keys and encoders the page takes; the rest work as on the stock screen)
RUI_HELLO, RUI_SPLASH = 0x35, 0x36   # 1023: the Rytm's hello when its USB port came up; the host's connect splash
RUI_MACHINES = 0x33   # 1025: Rytm -> host <t0> <n> {<machine 0..52> <vb>} x n: the tracks' machines (n 1 a change, 12 a snapshot
                      # after each hello, the page's open, a kit / project load); vb the voice byte its 03 / 04 carry
                      # (voice | the machine flag), 0x7F not a Sidecar machine (rytm-mods 0026 design.md "1025")
RUI_NOT_OURS = 0x7F
RUI_FIRST, RUI_LAST = 0x30, 0x3F
# 1027 SRC view (rytm-mods 0026 rui1027, provisional): Rytm -> host 29 <track 0..11> <on 0/1> the stock SRC page of the
# track shown / left (again after each hello); 2A <track> <first 0..15> <n> {<int 0..127> <frac 0..127>} x n its
# P<first+1>.. as the active kit has them (P1..P8 the SRC knobs A..H = kit words 0..7, P9..P16 the SMPL page's = words
# 17..24; all 16 after each 29 t 1, then the changed span each tick: n 1 a knob's change). No host -> Rytm 29 (0022
# passes only 30..3F): the host answers a 29 t 1 with a whole frame (3E 3D.. 38.. 39; the Rytm shows it from that 39,
# while 3C keeps coming) or with nothing (no page: stock); 3A x 0 with the page closed gives stock back. The view only
# draws: no 37, no 3F, no 34; the TRK + YES page comes first (no 29 0 for it; after it the host's next commit).
RUI_SRC_VIEW, RUI_SRC_VALS = 0x29, 0x2A
# 1027d diagnostic, Rytm -> host 2B <state> <views> <vt0..vt4> <age>: state bits 0 view on, 1 covered, 2 committed,
# 3 host idle >= 1 s, 4 TRK + YES open, 5 splash, 6 the host's frame shown; the controller list's length; the top
# view's vtable (vt0 << 28 | vt1 << 21 | vt2 << 14 | vt3 << 7 | vt4: 40234cfc SamplerLedView = plain SRC); age: 50 ms ticks since it changed (127 max)
RUI_SRC_DIAG = 0x2B
# 3D <seq> <slot 0..63> <x> <y top> <font 0..16> <flags> <chars 0..32>  a text slot into staging (empty: none),
#    committed with the frame by 39; drawn by the Rytm's own text routine in its own fonts (flags bits 0-1 align:
#    0 left of x, 1 centred on x, 2 right ending at x; bit 2 inverted: clear text on a set box). Fonts: the Rytm's
#    17 font objects in boot order (core/ui/rytm_fonts.h has the measured metrics).
# 3E <seq>  every staging text slot empty (the start of a whole frame)
# 3F <enc> <slot | 7F none> <value> <lo> <hi> <flags> <prefix 0..11>  the encoder's local echo: a turn on the Rytm
#    shows prefix + the new value in that slot at its next render, 400 ms before the host's own (flags bit 0: FUNC
#    x 8, bit 1: shown as value - 64 with a sign)
# 37 <e0> <e1> <e2> <k0 .. k36>  1022: the catch mask. Encoder i (0..15) is the host's when bit i % 7 of e(i // 7) is
#    set, key code c (0..255) when bit c % 7 of k(c // 7) is (bytes after the last sent: 0). The rest go to the Rytm's
#    own UI as on the stock screen. Whatever the mask: RECORD / PLAY / STOP always the Rytm's, TRK + YES always 0026's,
#    a caught FUNC reaches both sides (stock keeps its combos); a key's release goes where its press went. Before
#    any 37 (an older host) every key and encoder is caught but the transport; the page leaving by the timeout forgets it.
# 35 <uptime >> 7> <uptime & 0x7F> <caps>  1023 (Rytm -> host): hello, 0.5 s after the Rytm's USB port came up (again
#    1 s and 2 s later while no valid host message came); uptime in 50 ms ticks since 0026 started (saturated at
#    16383); caps bit 0: 36 understood
# 36 <ticks 0..127>  1023 (host -> Rytm): show the last committed frame as a splash for ticks x 50 ms with the page
#    closed (0: end it). Every key and knob stays the Rytm's (none reaches the host); transport and pads play under it,
#    a navigation key (LEFT RIGHT UP DOWN YES NO) only ends it, any other key or a turn ends it and reaches the Rytm;
#    1 s without a host message ends it too. Ignored while the page is open or before any commit.
RUI_REASONS = ("user", "host", "timeout", "escape")
# The firmware's key codes the remote pages name (rytm-emu src/panel.rs RYTM_KEYS, measured there)
RUI_KEYS = dict(T1=0x00, T2=0x01, T3=0x02, T4=0x03, T5=0x04, T6=0x05, T7=0x06, T8=0x07, T9=0x08, T10=0x09, T11=0x0A,
                T12=0x0B, T13=0x0C, T14=0x0D, T15=0x0E, T16=0x0F, FX=0x10, MUTE=0x11, CHRO=0x12, SCNE=0x13, PERF=0x14,
                TRIG=0x30, SRC=0x31, SMPL=0x32, FLTR=0x33, AMP=0x34, LFO=0x35, LEFT=0x40, RIGHT=0x41, UP=0x42,
                DOWN=0x43, YES=0x44, NO=0x45, RECORD=0x50, PLAY=0x51, STOP=0x52, PUSH_A=0x70, PUSH_B=0x71,
                PUSH_C=0x72, PUSH_D=0x73, PUSH_E=0x74, PUSH_F=0x75, PUSH_G=0x76, PUSH_H=0x77, PUSH_LEVEL=0x78,
                FUNC=0x80, SONG=0x81, CHAIN=0x82, TEMPO=0x83, PAGE=0x84, RTRG=0x85, TRK=0x86, BD=0x90, SD=0x91,
                RS=0x92, CP=0x93, BT=0x94, LT=0x95, MT=0x96, HT=0x97, CH=0x98, OH=0x99, CY=0x9A, CB=0x9B,
                GLOBAL=0xB0, QPER=0xB1, FILL=0xB2, FIX=0xB3, SAMPLER=0xB4, PLAYMODE=0xB5, BANK_A=0xC0, BANK_B=0xC1,
                BANK_C=0xC2, BANK_D=0xC3, BANK_E=0xC4, BANK_F=0xC5, BANK_G=0xC6, BANK_H=0xC7)


def pack7(data) -> list:
    out = []
    for i in range(0, len(data), 7):
        g = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << k for k, b in enumerate(g)))
        out += [b & 0x7F for b in g]
    return out


def unpack7(data) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 8):
        hi, g = data[i], data[i + 1:i + 8]
        out += bytes(b | (((hi >> k) & 1) << 7) for k, b in enumerate(g))
    return bytes(out)


def rui_rect(seq, col0, page0, ncols, npages, fb) -> bytes:
    """38: the rectangle of the 1024 B framebuffer fb (tile columns col0.., pages page0..)."""
    raw = [fb[p + 8 * x] for x in range(col0 * 8, (col0 + ncols) * 8) for p in range(page0, page0 + npages)]
    return _msg(RUI_RECT, [seq & 0x7F, col0, page0, ncols, npages] + pack7(raw))


def rui_commit(seq) -> bytes:
    return _msg(RUI_COMMIT, [seq & 0x7F])


def rui_open(page, on) -> bytes:
    return _msg(RUI_OPEN, [page & 0x7F, 1 if on else 0])


def rui_text(seq, slot, x, y, font, flags, text) -> bytes:
    t = [c for c in text.encode("ascii", "replace")[:32] if 0x20 <= c <= 0x7E]
    return _msg(RUI_TEXT, [seq & 0x7F, slot, x, y, font, flags & 0x7F] + t)


def rui_text_clear(seq) -> bytes:
    return _msg(RUI_TEXT_CLEAR, [seq & 0x7F])


def rui_echo(enc, slot, value, lo, hi, flags, prefix) -> bytes:
    t = [c for c in prefix.encode("ascii", "replace")[:11] if 0x20 <= c <= 0x7E]
    return _msg(RUI_ECHO, [enc, slot, value, lo, hi, flags & 0x7F] + t)


def rui_splash(ticks) -> bytes:
    return _msg(RUI_SPLASH, [max(0, min(127, ticks))])


def rui_keepalive() -> bytes:
    return _msg(RUI_KEEPALIVE, [])


def rui_mask(encs=(), keys=()) -> bytes:
    """37: the catch mask, encoders 0..15 and key codes 0..255 the page takes (the key bytes up to the highest code)."""
    e, k = [0, 0, 0], [0] * 37
    for i in encs:
        e[i // 7] |= 1 << (i % 7)
    for c in keys:
        k[c // 7] |= 1 << (c % 7)
    n = max([c // 7 + 1 for c in keys] or [0])
    return _msg(RUI_MASK, e + k[:n])


def decode_rui_mask(payload):
    """37's payload -> (set of encoders, set of key codes), None if malformed."""
    if not payload or payload[0] != PROTO_VER or not 4 <= len(payload) <= 41:
        return None
    b = payload[1:]
    encs = {7 * j + i for j in range(3) for i in range(7) if b[j] >> i & 1 and 7 * j + i < 16}
    keys = {7 * j + i for j in range(len(b) - 3) for i in range(7) if b[3 + j] >> i & 1 and 7 * j + i < 256}
    return encs, keys


def decode_rui(cmd, payload):
    """A Rytm -> host remote-UI payload -> dict; None if malformed."""
    if not payload or payload[0] != PROTO_VER:
        return None
    b = payload[1:]
    if cmd == RUI_MODE and len(b) == 3:   # page: 0 the default (SIDECAR), 1 the LFO page (rui1026's TRK + LFO)
        return dict(page=b[0], on=b[1], reason=RUI_REASONS[b[2]] if b[2] < len(RUI_REASONS) else b[2])
    if cmd == RUI_KEY and len(b) == 3:
        return dict(key=b[0] << 7 | b[1], down=b[2])
    if cmd == RUI_ENC and len(b) == 3:
        return dict(enc=b[0], delta=b[1] - 64, func=b[2])
    if cmd == RUI_HELLO and len(b) == 3:
        return dict(up=b[0] << 7 | b[1], caps=b[2])
    if cmd == RUI_SRC_VIEW and len(b) == 2 and b[0] < 12 and b[1] <= 1:
        return dict(track=b[0], on=b[1])
    if cmd == RUI_SRC_DIAG and len(b) == 8:
        return dict(state=b[0], views=b[1], top=b[2] << 28 | b[3] << 21 | b[4] << 14 | b[5] << 7 | b[6], age=b[7])
    if cmd == RUI_SRC_VALS and len(b) >= 5 and b[0] < 12 and b[2] >= 1 and b[1] + b[2] <= 16 and len(b) == 3 + 2 * b[2]:
        return dict(track=b[0], first=b[1], values=[b[3 + 2 * i] + b[4 + 2 * i] / 128 for i in range(b[2])])
    if cmd == RUI_MACHINES and len(b) >= 4 and b[1] >= 1 and b[0] + b[1] <= 12 and len(b) == 2 + 2 * b[1]:
        return dict(t0=b[0], tracks=[[b[2 + 2 * i], b[3 + 2 * i]] for i in range(b[1])])
    return None


def rui_src_view(track, on) -> bytes:
    """29 (Rytm -> host, 1027): the SRC page of `track` shown / left."""
    return _msg(RUI_SRC_VIEW, [track & 0x7F, 1 if on else 0])


def rui_src_vals(track, first, values) -> bytes:
    """2A (Rytm -> host, 1027): `track`'s P<first+1>.. (floats 0..127.99: the integer, then the 1/128 fraction)."""
    out = [track & 0x7F, first & 0x7F, len(values)]
    for v in values:
        iv = max(0, min(127, int(v)))
        out += [iv, max(0, min(127, int((v - iv) * 128)))]
    return _msg(RUI_SRC_VALS, out)


def rui_machines(t0, tracks) -> bytes:
    """33 (Rytm -> host, 1025): tracks [(machine, vb)] from track t0."""
    return _msg(RUI_MACHINES, [t0, len(tracks)] + [x & 0x7F for m, vb in tracks for x in (m, vb)])


# ---- slot fingerprints (2026-10-19): 25, see the docstring ----
SLOT_PRINTS = 0x25
SLOT_PRINTS_MAX = 16
SP_TRUNCATED = 1


def slot_prints(entries, seq=0, flags=0):
    """25 SLOT PRINTS (Rytm -> host): entries [(slot 1..5, fp 1..16383, count)], at most 16 (more: flags bit 0)."""
    entries = list(entries)
    if len(entries) > SLOT_PRINTS_MAX:
        entries, flags = entries[:SLOT_PRINTS_MAX], flags | SP_TRUNCATED
    body = [seq & 0x7F, flags & 0x7F, len(entries)]
    for slot, fp, count in entries:
        body += [slot & 0x7F, fp >> 7 & 0x7F, fp & 0x7F, max(1, min(127, count))]
    return _msg(SLOT_PRINTS, body)


def decode_slot_prints(payload):
    """25's payload -> dict seq, flags, entries [(slot, fp, count)], else None (wrong version or length)."""
    if not _ok(payload, at_least=4):
        return None
    n = payload[3]
    if n > SLOT_PRINTS_MAX or len(payload) != 4 + 4 * n:
        return None
    ents = []
    for k in range(n):
        e = payload[4 + 4 * k:8 + 4 * k]
        if not 1 <= e[0] <= 5:
            return None
        ents.append((e[0], e[1] << 7 | e[2], e[3]))
    return dict(seq=payload[1], flags=payload[2], entries=ents)


# ---- IN L/R source (2026-10-18): 26 / 27, no version byte (rytm-mods 0030-fx-in), see the docstring ----
FXIN, FXIN_STATE = 0x26, 0x27
FXIN_INPUTS, FXIN_FX, FXIN_ASK = 0, 1, 0x7F


def fxin(v):
    """26 FXIN (host -> Rytm): FXIN_FX the FX return on IN L/R, FXIN_INPUTS the AUDIO IN, FXIN_ASK ask (also True / False)."""
    v = FXIN_FX if v is True else FXIN_INPUTS if v is False else int(v)
    if v not in (FXIN_INPUTS, FXIN_FX, FXIN_ASK):
        raise ValueError("fxin: 0, 1 or 0x7F")
    return bytes(HDR) + bytes([FXIN, v]) + b"\xf7"


def fxin_state(on):
    """27 FXIN_STATE (Rytm -> host): on is True / False."""
    return bytes(HDR) + bytes([FXIN_STATE, 1 if on else 0]) + b"\xf7"


def decode_fxin(payload):
    """26's payload (the bytes after the id) -> FXIN_INPUTS / FXIN_FX / FXIN_ASK, else None."""
    return payload[0] if len(payload) == 1 and payload[0] in (FXIN_INPUTS, FXIN_FX, FXIN_ASK) else None


def decode_fxin_state(payload):
    """27's payload -> True (FX return on IN L/R) / False, else None."""
    return bool(payload[0]) if len(payload) == 1 and payload[0] in (0, 1) else None
