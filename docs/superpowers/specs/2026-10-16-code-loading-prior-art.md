# Code beyond the OS image on Elektron boxes: prior art (research, 2026-10-16)

Input for the +Drive code loader (`2026-10-16-drive-code-loader.md`). Read from public repos and their docs only; no
hardware. GPL projects were read for their approach; no code is copied here. Markers: **[doc]** what a project's own
docs or sources state (they often mark it measured on a unit), **[inferred]** our reading.

## Summary

| Project | Device, CPU | Where extra code lives | How it gets there | Cache handling | Verification | Recovery | Licence |
|---|---|---|---|---|---|---|---|
| [elekloader](https://github.com/irpina/elekloader) (+ its cores) | DT mk1, DN mk1, DT II, M:C/M:S, OT, **Rytm MKII 1.73 (PoC, booted on a unit 11 Oct 2026)**; MCF5441x (OT: ColdFire too) | Blob appended to MAIN OS (section 3), copied to a fixed DDR area: Rytm/DT mk1 `0x47BE0000-0x47C00000` (128 KB; DT mk1 core 4.0: 256 KB from `0x47BC0000`; DN core 4.0: 1.25 MB). Optional `.fast` code in on-chip SRAM | `.boot` replaces one call in the OS entry (Rytm `0x400008C0`), runs once **before the OS clears .bss and turns caches on**; copies `.run`, zeroes `.bss`, rejoins. `.fast` copied to SRAM by the first `ev_tick` (OS zeroes SRAM after `.boot`) | None needed: copy happens with caches off; SRAM never held executed code | Mods pin the stock file by sha256 and each site's stock bytes; overlap / RAM-budget / name-claim checks; output re-read before write; repack must depack in place within the bootstrap's gap and end below `flash_limit` (Rytm `0x300000`) | Bootloader never touched: FUNC at power-on, TRIG 4, stock .syx over DIN | GPL-2.0+ |
| [digihealth](https://github.com/irpina/digihealth) FAST AUDIO | DT mk1 1.53/1.54 | Copy of the render's hot code (`0x400716c0-0x4007629a`, ~19 KB) in SRAM `0x80003360-0x80007FFF` | elekloader mod; switches on once, 2 s after the screen is up; the copy's self-references fixed up, calls go through stubs that pick copy or original | Not needed (fresh SRAM, never executed before) | Refuses if the SRAM is not empty; checksums the copy once a second and falls back to the original code for good if it changed | SETTINGS row off until next boot; FUNC at power-on | GPL-2.0 |
| [digidrum](https://github.com/soejrd/digidrum) | DT mk1 1.53 | elekloader `.run` (3.2 KB code, 352 B bss in its spike) | elekloader core 2.1; 14 patch sites | (elekloader's) | elekloader lint; emulator (digiemu) | stock OS | none in repo; `mod.json` says GPL-2.0+ |
| [Synthack OS](https://github.com/Angelo-politek/Synthack-OS) `modarea` | Syntakt 1.41, MCF5441x (two ColdFires) | **64 KB at `0x46000000`**, DDR between .bss end and the boot stack, "never referenced by the OS" | Mods appended to section 3 at `0x40348000`, compressed (own LZ); loader is a `jmp` at the start of the .bss-clear routine (called once, before caches are enabled), replays the displaced instructions | None needed (pre-cache) | Bootstrap's in-place depack margin tracked (18 KB left at v0.8.0); boot hook checked in emulation (RAM equals uncompressed area) | stock OS | MIT |
| [sy-fm4](https://github.com/Rxuting/sy-fm4) | Syntakt 1.40A, both CPUs | Main CPU: blob appended after the image, copied to `0x47000000` (above .bss end, below the stack). Voice CPU (section 7): copied out of its own .bss | Boot stub copies before the .bss clear, on each CPU | None needed (pre-cache) | Hardware load test; 1.40A addresses only | FUNC + TRIG 4, stock over DIN | MIT |
| [octabam](https://github.com/sambanks/octabam) (and [Modwerk](https://modwerk.app/), built on it) | Octatrack MKI/MKII 1.40C | ~8 KB of "ROM caves" (zero runs) shared by all; DRAM units linked into one runtime, packed, appended behind octabam's loader, depacked at boot into a **10 MB reserve carved from the sample pool** (MEMORY module, sized to what modules claim) | Boot detour at the OS's DSP-upload call | (pre-cache at boot) | A ledger refuses two modules claiming one address/hook/id/buffer; boot under a ColdFire port; `expect` bytes | FUNC at power-on (OT: TRIG 3, DIN only) | MIT (per Modwerk's notice) |
| octabam **REMIX SWITCH** | Octatrack | A whole raw OS image (`.RMX`) on the CF card | Read file into a DDR stage at the top of the reserve; write a mailbox (length, hash, check word, name); park the DSPs; **soft reset** (RCR SOFTRST). The flashed image's early boot finds the mailbox, spends it (one-shot), and a stub copies stage to `0x40000400` **with caches off**, sets CACR, jumps to the entry | Avoided by design: copy runs with caches off after a reset | Hash, length vs stage size, entry's first instruction, bootstrap version; every refusal boots the flashed image and records why | One-shot mailbox (a hang costs one reset, not a loop); FUNC at power-on bypasses a saved choice; pull the card | MIT |
| [otype](https://github.com/nordseele/otype) / [OT1](https://github.com/nordseele/ot1) | Octatrack 1.40C | Engine (compiler + VM, ColdFire, not PIC) at a linked base in reserved DRAM; ~1.9 MB work block carved from sample memory | Integrator's tool copies `otype.bin` after stock init; **scripts** (`/SCRIPTS/*.OTY`) are compiled on the unit to VM tables (data, not native code) | Contract: "push and invalidate the caches after the copy" | Link records size + sha256; refuses a base outside cached DRAM or overlapping the image | (integrator's) | GPL-3.0 |
| GGSM (Discord) | DT mk1 | Code loaded "from the +Drive flash, not the firmware image" | Not public; no repo found (search 2026-10-16) | ? | ? | ? | ? |

Also relevant to the Rytm: [Iced-Cream/rytm-mods-src](https://github.com/Iced-Cream/rytm-mods-src) (MrScruff's
Rytm MKII mods, GPL-2.0) keeps a cave/detour registry with `expect` bytes and ran the DDR canary that verified the
`0x47BE0000` area on a Rytm.

## What the field does [inferred from the table]

1. **Nobody else loads native mod code from storage at run time into the running OS.** Every shipping project puts the
   code in the OS file (appended to section 3, compressed or not) and copies it out at one early boot point. The one
   storage loader, REMIX SWITCH, loads a whole OS and reboots into it rather than patching a live system. GGSM's
   +Drive loader is the only claimed counterexample and is not public.
2. **The copy point is always before the OS's .bss clear and cache enable** (elekloader's OS-entry call, Synthack's
   .bss-clear hook, sy-fm4's stub). That is why none of them flushes a cache: there is nothing stale yet.
3. **The destination is a fixed DDR window between the OS's .bss end and the boot stack** (or carved from the sample
   pool), shown free by "no instruction operand points there" plus a write watch in an emulator plus a canary on a
   unit. Not a heap allocation.
4. **Version pinning is by hash** of the stock file and the bytes at every site, and refusal is the default.
5. **Recovery relies on the untouched bootstrap** (FUNC at power-on, stock file over DIN). Run-time features add their
   own off-switch (digihealth's SETTINGS row and watchdog; REMIX SWITCH's one-shot mailbox).

## Lessons for our Rytm loader

1. **Consider a fixed, verified area instead of `mem_new`.** `0x47BE0000-0x47C00000` (128 KB) on Rytm 1.73 is
   verified untouched on a unit by elekloader's/rytm-mods' canary through playback, sampling, FX, MIDI, Overbridge,
   save/reload and a power cycle [doc]. A fixed base would let the blob be linked absolute (no relocation table)
   and frees us from heap timing. It also collides with elekloader core-ar2 if both are ever in one image: claim it in
   the registry. If we keep `mem_new`, keep the relocation step; the field gives no precedent either way.
2. **Loading after the caches are on is the risky part, and it is ours alone.** The plan's push-D/invalidate-I-and-D
   with interrupts masked is what otype's contract asks for. Datasheet point [inferred]: in copyback mode, a CACR
   invalidate-all alone discards dirty D lines; push first (`cpushl` over every set and way), then invalidate. Hook
   sites written in the image's code (already cached, maybe executed) need the same flush, and sites inside code
   another task may be running at that moment are a hazard: write hooks only where no other task can be mid-routine.
3. **Option worth keeping in reserve: the REMIX SWITCH pattern.** Stage the file in DDR, write a mailbox, soft reset,
   and copy it out in the early boot hook with caches off. It sidesteps cache coherence and live patching entirely,
   at the cost of a reboot and of needing DDR (and the mailbox) to survive the soft reset, which octabam measured on
   the OT and we have not on the Rytm. Its one-shot mailbox is a good idea for us anyway: record "load attempted"
   before calling `entry`, clear it after a clean return, and skip loading on the next boot if it is still set.
4. **Pin the blob to the exact image**, as every project pins to the stock hash (our 32-bit key does this), and
   verify the whole file before running any of it. REMIX SWITCH and elekloader refuse on any mismatch and say why in a
   status word: our `dl_state` matches that.
5. **A run-time watchdog is cheap and proven** (digihealth): checksum the loaded code periodically and disable the
   feature if it changes. Useful while the area's safety rests on emulator evidence.
6. **Do not lengthen boot.** Synthack notes the Syntakt's boot waits for its intro animation; set up from hooks, not
   inline [doc]. Our stub runs in the init task: measure its cost.
7. **Budget the image too.** If the stub or hooks ever grow, the Rytm's limits are the bootstrap's in-place depack gap
   (stock 401,261 B; elekloader's repack 533,073 B) and the container ending below `0x300000` (a second OS store sits
   at `0x300000-0x380000`) [doc, elekloader's Rytm profile].
8. **SRAM for hot code** (digihealth's FAST AUDIO: ~1000 I-cache misses a block avoided) is a later option; on the Rytm
   find which SRAM range the OS zeroes and never uses before claiming any, and copy only after the OS's SRAM init.
9. **Transfer is not a code-install path anyone uses for files**; all install whole OS files. Our checksum against a
   non-byte-exact Transfer conversion is the right guard; the Sidecar writing the file stays the preferred route.

## Sources

- elekloader: README, docs/DEVICES.md ("The Analog Rytm MKII (1.73)"), docs/CORES.md, docs/ADAPTING.md ("The rules",
  "The mods' RAM"), mods/core/core.s, mods/core/fast.s, elekloader/devices/analog_rytm_mk2.py.
- digihealth: README ("FAST AUDIO"), fastaudio.s header.
- digidrum: mod.json, Verification/SPIKE.md, tools/elekloader-mod.ld.
- Synthack-OS: docs/memory-map.md, mods/modarea/README.md.
- sy-fm4: README, docs/DESIGN.md.
- octabam: README, modules/remix-switch/README.md, docs/proposals/FIRMWARE_SWITCHER.md; Modwerk README.
- otype: firmware/README.md ("Integration contract"); OT1 README.
- Scene overview: [Dubspot, Oct 2026](https://blog.dubspot.com/custom-firmware-vibe-coding-music-hardware-2026).
