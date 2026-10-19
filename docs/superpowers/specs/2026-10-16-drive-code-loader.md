# Mod code loader: beyond the caves (design, 2026-10-16)

Mods today are small patches placed in caves of the OS image, and the caves are full. This design adds a
resident stub (rytm-mods mod `0026-drive-loader`) that reads one mod-code file from the Rytm's own +Drive at boot,
checks it, puts it in RAM and calls it. The loaded code installs its own hooks. Larger mods (the remote-UI
terminal, send-FX pages) can then live in that file instead of in caves.

The bootstrap and the stock reflash path are not touched, so stock can always be restored by reflashing the stock
OS file. The scope is OS 1.73 (MAIN OS sha256 `28d9ef40...`, load address 0x40000400). This was done in rytm-emu
and by reading the disassembly. No hardware and nothing flashed. No firmware bytes are in this repo.

Tags: **[code]** read in the 1.73 disassembly; **[measured]** run in rytm-emu; **[inferred]** reasoned, not shown.

## 0. Decision (2026-10-16, the user): route A first, the +Drive loader later

The prior-art review (`2026-10-16-code-loading-prior-art.md`) found no shipping project that loads native code at run
time after the caches are on. Every one appends its code to the OS file and copies it out at one early-boot point,
before the OS clears .bss and enables the caches. **Route A is the chosen route. Sections 1 to 4 below (the +Drive
runtime loader, route B) are kept as a later, optional design.**

### Route A: blob appended to MAIN OS, copied at early boot into a fixed area

- **Area:** 0x47BE0000-0x47C00000 (128 KB) of DDR, linked absolute, so no relocation is needed. elekloader's canary
  verified the area on a real Rytm 1.73 [doc]. Claim it in `registry/allocations.toml`, which also records the
  collision with elekloader's core-ar2. Re-check it in rytm-emu with a write watch and a canary over the heavy
  scenarios (section A.3). The last 16 bytes hold the loader's words: the state and the one-shot flag.
- **Early-boot point [code, 1.73]:** the OS entry 0x40000870 sets the boot SP to 0x48000000 and calls 0x400014ae
  (vector fill), 0x40000690, 0x400014cc (VBR) at 0x400008c0 (`jsr abs.l`, 6 B), then 0x400007e4 (copies the SRAM
  image out of 0x402ce000..), then 0x4000083a (**.bss clear 0x402ce000..0x42fbe350**). After that it writes CACR
  0xa50ce100 / ACR0 0x4007e020 at 0x400008de..0x400008ee. The appended block loads at the 16-byte-aligned end of
  the stock image, 0x402de000, which is inside .bss. It must therefore be copied before 0x4000083a runs. Detour:
  0x400008c0. The stub does its work, re-emits `jsr 0x400014cc` and rejoins at 0x400008c6. (elekloader uses the
  same call. The site was found in our own disassembly. No elekloader code is used: GPL, approach only.)
- **Block format:** a 16-byte header (magic, key = the stock MAIN OS sha256 prefix ^ ABI, length, checksum), then
  a hook table of `{site, 6 stock bytes, 6 new bytes}` entries, then the image. Order at early boot, with caches off
  and nothing executed yet:
  1. Check the one-shot flag.
  2. Check the header and the checksum over the whole block.
  3. Check every hook site against its stock bytes.
  4. Copy the image and zero the rest of the area (.bss).
  5. Write the hook sites.
  6. Set the flag and record the state.

  Any failure leaves the image stock: no copy, no site written. Registry `[[detour]]` entries marked as
  boot-applied describe the hooks, so the existing overlap checks still apply. build.py writes them into the
  table, not into the image, and verify.py checks them against stock.
- **No cache maintenance:** the copy and the site writes happen before CACR is set and before any of that code has
  run.
- **One-shot flag:** a magic word set at early boot and cleared by the blob once it has run healthy (N UI ticks). If
  the flag is still set at the next boot (a crash, then a warm or watchdog reset), that boot runs stock once and
  clears it. A power cycle probably leaves DDR undefined, so the flag protects warm resets only [inferred]. Recovery
  stays the untouched bootstrap: reflash stock.
- **Run-time watchdog:** each hook checks a `disabled` word first and then runs only the displaced stock
  instructions. The blob checksums its own code a chunk per tick against the boot-time sum and sets `disabled` on
  a mismatch. No code is rewritten at run time.
- **Budgets:** the container must end below flash 0x300000 (a second OS store sits at 0x300000-0x380000), and the
  bootstrap's in-place depack gap applies (stock 401,261 B, elekloader's repack 533,073 B) [doc]. verify.py reports
  both sizes. The boot cost of the copy, checksum and zeroing is about 128 KB of word operations, a few ms at
  ~200 MIPS [inferred]. Measure it in rytm-emu.
- **Changing mods means reflashing.** That is the trade for having no cache risk and no install path.

### A.1 Test hook for the emulator milestone

The UI task's message loop: 0x400a4684 `pea 0x41943f54` (6 B, re-entered by the `bcs` at 0x400a469a, no branch
into its middle), about 62 times a second at idle [measured: `--count 0x400a4692`, 124 in 2 s, hwus1nl]. The test
blob counts ticks there, clears the one-shot flag after N ticks and runs the watchdog.

### A.2 Heap: why a fixed area, and the cap for route B

At rest after boot the buddy heap (16 MB arena at 0x41ac1340) has 13 880 896 B free on hwus1nl, and its lowest
point over 2 s of idle play was 13 880 816 B [measured with rytm-emu's new `DTEMU_HEAP` low-water probe, which walks
the free lists whenever the allocator holds its lock]. Route A does not use the heap at all. If route B is built,
its permanent block is capped at a fixed **32 KB**, allocated once at boot. Larger blobs are refused. The margin
under heavy scenarios (the user's 128-pattern project "ISLAND 2026 02", sample loads, SAVE PROJECT, SysEx dumps,
pattern/kit switches; stock vs hwus1nl vs sfx1b) is **still being measured**. Record it here with the CPU numbers.

### A.3 CPU and time budgets (pending measurements)

The user's real Rytm became unresponsive with a nearly full project with many trig conditions. That points at CPU
(sequencer + UI task + audio ISR), not heap, because patterns and kits sit at fixed addresses. Budgets for the
early-boot stub, per-tick hook cost, and future UI/remote-UI code will be set from the heavy-project measurements:
UI latency and render-tick slip, audio-ISR share per block, and the executed/idle ratio. Until then, the rule is
that hooks in the UI loop do O(1) work per message, and nothing runs in the audio ISR.

### Decision: elekloader (2026-10-16, the user)

Route A is built with [elekloader](https://github.com/irpina/elekloader) (GPL-2.0-or-later), not with a boot
stub of our own. Its core-ar2 0.1 does exactly this route on 1.73: the same boot site 0x400008c0, the same area
0x47BE0000-0x47C00000, the copy before the .bss clear and before CACR. elekloader has verified it on a unit.
rytm-mods mod `0027-elekloader` (branch `elekloader`, `mods/0027-elekloader/design.md`) has build.py emit every
rytm-mods mod as a standard format-2 `.elemod`, so our mods stay combinable with other elekloader Rytm mods and
Modwerk. The registry, `expect` guards and verify.py stay in force, and both tools' stock hashes must agree. It
has been measured in rytm-emu, not on hardware:
- the area is untouched (watch + canary) through play, kit save/reload, MIDI, the USB host, power off and reboot;
- a test mod runs from the area through core's `ev_tick` and through a registry detour (A.1's 0x400a4684);
- 0022's SysEx router has moved into the area;
- the hwus1nl suites pass and the cache model reports no stale fetches;
- `.boot` costs about 425 instructions.

The block format, one-shot flag and watchdog above are not part of elekloader's core. They are optional later
work as an area mod.

# Route B (later, optional): the +Drive runtime loader

## 1. Firmware facts

### +Drive file API [code; open/read/close measured]

| what | address | notes |
|---|---|---|
| `fs_open(path, mode, File*)` | 0x4007d4dc | Returns 0 or -1. Fails at once when not mounted or when the path is over 255 chars. Absolute paths only. Mode `"r"`. |
| `fs_read(buf, n, File*)` | 0x4007d222 | Returns bytes read and advances File+4. Returns -1 when the drive is not mounted or the file is not open. |
| `fs_close(File*)` | 0x4007d49a | Returns 0 or -1. |
| `File` | 16 B | +0 node id, +4 position, +8 writing, +12 open |
| `fs_mounted` | 0x41843c70 | Non-zero while the drive is mounted. Every FS call checks it. |
| FS mutex | 0x419f5320 | One mutex for every FS call. The calls block, so they may run in task context only, never from an ISR. |

### Mount point [code + measured]

- `fs_mount` 0x4007f300 is called once at boot, from 0x40100966 in the **init task** (prio 6). In rytm-emu this
  happens at about 193M instructions (about 1 s). It sets the flag (0x4007f392) and returns 0.
- At that point 5 tasks exist. The sample loader, storage and UI tasks have not been created yet, and the audio
  vector is not installed. The heap already works. Only the init task touches the FS, so the loader cannot
  contend with anything.
- **Detour:** 0x4010096e `move.l d0,0x419ae2bc`, which stores the mount result. It is 6 bytes (`23c0 419a e2bc`),
  no branch lands inside it (0x4010093e/4a/62 all go to 0x40100974), and execution rejoins at 0x40100974. The
  stub re-emits the store, then loads only if d0 == 0 (mounted).

### Skip key [measured, emulator only]

`is_key_held(0x45)` (NO) at 0x40081bbc. A key held from power-on is in the key bitmap (0x418d1340 byte 4 bit 0)
by about 20M instructions, well before the mount point. **Holding NO at power-on skips the loader.** Whether a key
held through power-on on real hardware is latched by then is **unknown**. Fallback if the skip fails: reflash
stock, delete the file in the stock OS, then reflash the mod.

### RAM [measured]

The buddy heap has about 13.2 MB free in a 16 MB arena at 0x41ac1340. The loader takes one permanent block with
`mem_new` (0x400829d4) at boot. The address is not guaranteed, so **the blob is relocatable** (see 2.3).

### Caches [code + datasheet; not modelled by the emulator]

The MCF5441x has a 16 KB I-cache and a 16 KB D-cache, both 4-way. DDR is data-cached in copyback mode (ACR0 =
0x4007e020, CACR = 0xa50ce100). Code written through the D-cache is not seen by the I-cache. After the code is
placed (and after every hook site is written), the loader must push the D-cache and invalidate both caches: a
`cpushl bc` on every set and way, with interrupts masked. **rytm-emu does not model caches, so cache correctness
is a hardware-only risk.** It gets its own hardware test step (section 4).

### Space [code]

hwus1nl's cave2 block for 0003 (0x402cdddc, 440 B) is free in the user-slot builds, which exclude 0003. The stub
must fit well inside it.

## 2. Design

### 2.1 Stub (in the image, small, does as little as possible)

In order:
1. Re-emit the displaced store. If the mount failed, rejoin.
2. If NO is held: state = skip, rejoin.
3. Open `/incoming/sidecar.bin` (absolute). If it is missing: state = no file, rejoin.
4. Read the 32-byte header and check it: magic, key, sizes. Allocate with `mem_new(alloc)`, read the image,
   close the file.
5. Check the checksum over the whole image.
6. Flush the caches (push D, invalidate I and D).
7. Call `entry(base, flush)` with the C ABI. The stub records the result in `dl_state`. It does not undo
   anything: the blob checks every hook site before writing (2.4), so a refused blob has written nothing.
8. Rejoin 0x40100974.

The stub does **no relocation** and does not know about hooks. If anything fails before the call, nothing in the
OS has changed and it runs as stock. A failed `mem_new` or a short read leaves at most one unused heap block.

`dl_base` and `dl_state` (2 words at the start of the cave) record the outcome, for the blob, the emulator tests
and later diagnostics. States: 0 not run (no mount), 1 loaded (entry returned 0), 2 NO held, 3 no file, 4 bad
header, 5 no memory, 6 short read, 7 bad checksum, 8 entry refused.

### 2.2 File format (big-endian)

```
+0  'SCLD'
+4  key  = first 4 bytes of the stock MAIN OS sha256, XOR the ABI number (ABI 1)
+8  size   bytes of image, > 0, a multiple of 4
+12 alloc  bytes to allocate, size <= alloc <= 256 KB (room for .bss)
+16 entry  offset of the entry from the image start
+20 checksum over the image: h = 0; for each word w: h = (h + w) * 0x01000193 (mod 2^32)
+24..31    reserved (0)
+32 image  size bytes (code, data, then the relocation list)
```

The key ties the file to the exact OS build the stub sits in. The stub is in that image, so a 32-bit prefix is
enough to reject a blob built for another OS or ABI. It is not a security measure. The checksum catches truncated
or altered files, including any transfer that is not byte-exact.

### 2.3 Relocation (inside the blob)

The blob is linked at two bases (0 and 0x10000). Diffing the two images gives the list of words to relocate,
which is stored at the end of the image. The blob's entry is a **position-independent prologue** (PC-relative
only). It finds its own base, adds the base to every listed word, flushes the caches with the `flush` pointer the
stub passed, then jumps to its real init. Everything inside the checksummed blob, the stub included, stays small.

### 2.4 Hooks (inside the blob)

The blob carries a patch table: `{site, length, stock bytes, new bytes}`. Init checks **every** site against its
stock bytes first. If any site differs, it writes nothing and returns non-zero (state 8). Otherwise it writes all
of them, then flushes the caches. Hooks therefore come with the blob and need no cave space. The image-side
detours of other mods stay as they are.

### 2.5 Install path

- Transfer's sample conversion may not be byte-exact. The checksum rejects a file it altered, and the OS then
  runs as stock.
- Preferred: the Sidecar expander writes the file onto the +Drive. 0023 today only lists and reads the Teensy's
  card (readdir/resolve/size hooks). A write path (`fs_open` with `"w"`, 0x400805f4) **still has to be designed
  and verified**. For the emulator, the test writes the file straight into the card image.

### 2.6 Restore

The bootstrap is untouched. Reflashing the stock OS file always works. A blob that hangs at boot is skipped by
holding NO, or, failing that, the fallback in section 1 (reflash stock, delete the file, reflash the mod).

## 3. Emulator milestone (rytm-emu, `DTEMU_STRICT=1`)

- A test blob on the emulated card loads, runs and has a visible effect (it counts ticks or changes one display
  string). `dl_state` = 1.
- No file leaves the OS stock (`dl_state` = 3). A bad checksum leaves it stock (7). NO held leaves it stock (2).
- hwus1nl's suites (`tools/emu/us_emu.py all`) still pass with the loader in the image. `build.py` and
  `verify.py` PASS.

## 4. Open risks

1. **Caches on hardware.** Not modelled. Test on hardware: a blob whose hook changes visible behaviour, booted
   cold several times, plus a write-after-load test (a hook installed late in init).
2. **Power-on NO hold** on real hardware may not be latched by the mount point. Measure it before relying on it.
3. **Install path.** Byte-exact writing to the +Drive (Sidecar or Transfer) is not yet shown.
4. The heap block's address varies, which relocation covers. Its lifetime is permanent: it is never freed.
5. The blob runs in the init task before the UI and audio exist. Hooks into those subsystems must be installed
   as patches, not by calling into them.
