# Rytm MKII OS 1.73 under heavy projects: CPU, UI and heap budgets for our mods

Date: 2026-10-16. Status: measured in rytm-emu (emulator only, no hardware). Builds: stock 1.73, hwus1nl
(`fw/AR173_us_hwus1nl.syx`), sfx1b (rytm-mods-sfx `build/Analog-Rytm_MKII_OS1.73_sfx1b.syx`, the same bytes as
`fw/AR173_us_sfx1b.syx`). These numbers fill the placeholder sections of
`2026-10-16-drive-code-loader.md` (A.2 heap margin, A.3 CPU and time budgets). That spec is not edited here; read
these numbers in its place.

Two data sets:

1. **ISLAND 2026 02.** The user's real project, loaded the way Transfer loads it. This is the **cleaned-up** version,
   which was **fast again on the real Rytm**. The sluggish original was ISLAND plus one more full bank of 16 patterns.
   So ISLAND is the "large but OK on hardware" reference point.
2. **Synthetic heavy project.** Built by an earlier helper, with scripts in the session scratchpad `stress/`
   (`make_heavy.sh`, `build_heavy.py`). All 128 patterns are full: 12 tracks x 64 trigs, a trig condition on every
   step, and all 72 lock slots with a value on every step. This is the "beyond" case.

Plus a reconstruction of the sluggish original (ISLAND + 1 bank) and a p-lock density sweep.

All clocks are emulated instructions. rytm-emu runs **200 M instructions per emulated second**. So one audio block
(32 samples at 48 kHz, 1500 per second) is **133,333 instructions**, and one 20 Hz UI tick is 10 M instructions.
The emulator has **no cache or wait-state timing**: every instruction costs the same. Budgets are therefore given in
instructions, followed by a hardware-timing caveat (section 6).

## 1. Loading the project [measured]

- **Payload format.** The 107,191-byte payload "ISLAND 2026 02" from the `.arprj` (manifest: FileType Project,
  FirmwareVersion 1.72, ProductType 8/20) is the raw **Elektron data-protocol blob** for `/projects/N`. It is not a
  SysEx dump and not a +Drive file. It carries each sample as a pair (+Drive file hash, size) in the clear, for
  example `3680830d 000289b8` for EFM-KCs01.
- **Upload.** I sent it as Transfer does, over the emulated USB MIDI port. The messages are elektroid's
  data-protocol frames (0x57 open with size and path, 0x58 partial writes of 8 KB with `crc32(0xffffffff)`, 0x59
  close). The target was `/projects/4`. All 14 blocks were acknowledged (status 1, total 0x1a2b7), and the slot
  list showed `004 ISLAND 2026 02`.
- **Samples.** All 75 WAVs went in the way Transfer syncs them, with the file protocol: 0x11 mkdir for each folder,
  then 0x40/0x42/0x41, written to `/samples/...`, `/recorded/...` and `/projects/...` without the `.wav`. The file is
  converted to the Elektron sample format: a 64-byte header (size, rate 48000, loop start 0, loop end n-1, loop type
  0x7f at +0x14), big-endian PCM and a 16-byte zero trailer. With that header, **all 75 lookup3 hashes (seed
  0x654c654b, |1) match the manifest**, so the converted files are byte-exact. 6,241 messages were sent and every
  close was acknowledged with the right size. The two whose replies were not captured are noted in the
  reproduction section. Even so, **after LOAD PROJECT the project's sample slots stayed empty** (FUNC+SMPL: `1:---`),
  and the firmware did not open them during the load. Why is open. As a stand-in, the 43 samples in `/incoming`
  were loaded into slots 1..43 (GLOBAL > SAMPLES > SELECT ALL > LOAD TO PROJ: "43 SAMPLES LOADED"). The project was
  then saved, so the tracks' SMP values point at real samples.
- **1.72 -> 1.73.** LOAD PROJECT 004 on stock 1.73 converted the project and loaded it: kit K122 "A13", pattern D11,
  BPM 125, no fault. It plays. The mod builds cold-boot into it and load it the same way.
- **Content** (decoded from the working state after a power-off; layout as in `build_heavy.py`):

| | ISLAND | ISLAND + 1 bank | synthetic heavy |
|---|---|---|---|
| filled patterns | 76 (bank H empty; A 16, B 15, C 12, D 11, E 13, F 4, G 5) | 92 | 128 |
| trigs | 4,811 (busiest pattern 180; mean 63 per filled pattern; 768 max) | 7,003 | 98,304 |
| trigs with a condition | 879 (mostly code 22, then 34, 31, ...) | 1,175 | 98,304 |
| lock slots used | 626 (max 34 per pattern) | 810 | 9,216 (72 per pattern) |
| track lengths | 119 x 64, 5 x 32, 4 x 16 | | 64 |

ISLAND + 1 bank: bank H (patterns 113..128) was filled with copies of ISLAND's 16 busiest patterns (`plus1.py`), then
cold-booted.

## 2. Per-scenario results (stock 1.73, ISLAND) [measured]

Scenario (`scen.sh`, about 55 s emulated): idle stopped, then PLAY, then page keys as latency probes, then two
pattern changes while playing (BANK_B+T3, BANK_E+T6), then STOP, then two stopped pattern changes (BANK_D+T11,
BANK_A+T1), then PLAY, then **SAVE PROJECT AS 004 while playing**, then **LOAD PROJECT 004**. The probe tool
(`stress`, a scratch Rust binary on the rytm-emu library) sets breakpoints:

- the audio ISR entry `0x4011fafa` and its only RTE `0x4012165e`, giving instructions per block;
- the screen idle routine `0x400a2aaa`, the 20 Hz render tick (entry intervals, and its duration up to the return
  address);
- the lock rebuild `0x40009216` and the lock load `0x40008f98`.

It also runs the heap probe (`DTEMU_HEAP` state 0x40285830, PCs 0x40082772 and 0x400825e6), and measures a key's
latency as the time from key down to the first change of the OLED pixels.

| metric | ISLAND, stock |
|---|---|
| audio ISR per block, stopped | 64,934 instructions (48.7 % of the block) |
| audio ISR per block, playing D11 | mean 69,566 (52.2 %); busier B03 72,550 (54.4 %) |
| audio ISR, worst block in the run | 75,537 (56.7 %), on step starts |
| all executed work while playing (ISR + tasks) | 54.7 % (idle 45 %) |
| render tick 0x400a2aaa | 20.1 to 20.4 per second, gap p99 50.1 ms, max 50.1 ms, **no slip >75 ms in any scenario** |
| tick duration | mean 24 to 52 k instructions, max 240 k (1.2 ms); during a 43-sample load max 1.58 M |
| key -> screen (page keys AMP/FLTR/SRC) | 0.7 to 1.4 ms |
| pattern select (BANK+T) -> screen | 57.8 ms (one T6 change 358 ms: queued to the pattern end while playing) |
| pattern change while playing | 1 lock rebuild, 0.83 to 1.03 M instructions (4 to 5 ms) |
| SAVE PROJECT (while playing) | 129 x `0x40009216` (every stored pattern plus the working one), mean 435 k each = 56 M; about +460 ms of CPU over the playing load, about 0.5 s at >95 % busy |
| LOAD PROJECT | 129 x lock load (mean 332 k) + 258 rebuilds; about +970 ms of CPU, 1.5 s at >=95 % busy; the audio ISR halves in the first window (audio paused) |
| heap, at rest | 13,876 kB free (stock), 13,860 kB (hwus1nl / sfx1b) |
| heap low-water | LOAD PROJECT 13,614 kB (stock) / 13,600 kB (mods); the first load of the uploaded 1.72 project 12,804 kB |
| 43-sample load (browser) | low-water 13,819 kB; tick max 1.58 M (7.9 ms) |

Other scenarios from the synthetic-heavy data set (earlier helper, `stress/logs`): SysEx kit/pattern dumps in over
USB (288 KB) bottom out at 13,843 kB stock / 13,780 kB mods. Request bursts bottom out at 13,851 kB, and DIN replies
are capped at 65,536 B (not investigated). The **heap worst case is LOAD PROJECT on the heavy project: 9,685 kB
stock, 9,672 kB hwus1nl/sfx1b** (about 4.2 MB transient). Its ISR is mean 72.9 k, max 79.8 k (59.9 %; step starts
+7 k). The UI task tick is 60 Hz (message type 6 at 0x400a498e) and never slipped. Key to OLED frame was <= 10 ms.

## 3. Builds: what the mods add [measured]

| | stock | hwus1nl | sfx1b |
|---|---|---|---|
| ISR stopped / playing / max | 64,934 / 69,566 / 75,537 | **identical** | 65,002 / 69,635 / 75,647 |
| 0025 `sfx_hook` (0x402cc920 -> rejoin 0x401213aa), per block | - | - | mean **70**, p99 101, max **119** instructions (0.05 % of a block) |
| 0021 `ext_render` (0x400b21c8) | - | 0 calls | 0 calls |
| SAVE / LOAD PROJECT | 435 k / 332 k per pattern | 433 k / 327 k | 433 k / 325 k |
| heap at rest / low | 13,876 / 13,614 kB | 13,860 / 13,600 kB | 13,860 / 13,600 kB |
| latencies | as stock | +1.3 ms on pattern select (59.1 ms) | same as hwus1nl |

- 0021 adds **no ISR work** with this project. `ext_render` runs only for tracks on an EXT or user-slot machine, and
  ISLAND has none. Its per-call cost is **not measured yet**: it needs a project with EXT tracks.
- The mods' heap cost is 13.5 kB, permanent (at rest and at low-water alike).

## 4. Does the emulator see the difference between ISLAND and its sluggish original? No [measured]

| | ISLAND (fast on hardware) | ISLAND + 1 bank (sluggish on hardware) |
|---|---|---|
| ISR stopped / playing / max | 64,934 / 69,566 / 75,537 | 64,934 / 69,566 / 75,537 |
| busy while playing | 54.7 % | 54.7 % |
| render tick | no slip | no slip |
| latencies | 0.7 to 57.8 ms | identical |
| SAVE (rebuild mean x 129) | 435,468 | 438,181 (+0.6 %) |
| LOAD (lock load mean x 129) | 332,206 | 333,065 (+0.3 %) |
| heap low-water | 13,614 kB | 13,614 kB |

**The emulator cannot tell them apart.** The hardware threshold therefore comes from something it does not model:
cache misses, DDR wait states, uncached accesses, FPGA/voice SPI traffic, or display/LED UART timing. This supports
the earlier helper's conclusion.

## 5. P-lock density curve [measured]

Variants of ISLAND's working state (`var.py`): every stored pattern gets 12 tracks x 64 trigs (no conditions,
length 64) and L lock slots with a value on every step (slot i on track i mod 12). Then a cold boot and the same
scenario. The `npat N` rows have N full patterns (72 locks) and the rest cleared.

| variant | ISR playing mean | ISR worst (step start) | rebuild mean (x129 on SAVE) | lock load mean (x129 on LOAD) | LOAD >=95 % busy |
|---|---|---|---|---|---|
| ISLAND (626 slots, max 34/pattern) | 69,566 | 75,537 | 435,468 | 332,206 | 1.5 s |
| L = 0 | 71,232 | 87,787 | 428,522 | 322,996 | 1.5 s |
| L = 18 | 71,176 | 88,269 | 460,294 | 340,861 | 1.5 s |
| L = 36 | 71,179 | 88,864 | 527,101 | 357,852 | 1.5 s |
| L = 54 | 71,163 | 89,616 | 542,879 | 369,454 | 2.0 s |
| L = 72 | 71,146 | **90,174 (67.6 %)** | 582,078 | 384,838 | 2.0 s |
| npat 16 (rest empty) | (played an empty pattern) | 89,118 | 459,269 | 330,579 | 1.5 s |
| npat 76 | 71,146 | 90,174 | 513,509 | 361,939 | 1.5 s |
| npat 92 | 71,146 | 90,174 | 549,997 | 367,539 | 1.5 s |

Where the lock work happens, and what it scales with:

- **Audio ISR:** only the **playing** pattern. The per-step lock apply into the trig array (0x8000e5b0) costs about
  +2.4 k instructions on step-start blocks from L = 0 to 72 (87.8 k -> 90.2 k). The mean does not move. Full trigs
  on 12 tracks cost much more: +12 k on step starts over ISLAND. The number of other patterns has no effect on the
  ISR (npat 76/92/128 identical).
- **SAVE PROJECT** and **LOAD PROJECT:** a scan over **all 129 pattern slots every time**, filled or empty. A large
  fixed cost (~430 k per pattern even with no locks) plus ~2.1 k per used lock slot (rebuild 428 k -> 582 k for 0 ->
  72 slots). So a save costs 55 M (ISLAND) to 75 M instructions (all patterns at 72 slots): 0.28 to 0.38 s of CPU.
  Each pattern filled with locks adds ~1.2 M instructions to a save+load pair (~6 ms). In the emulator this is
  linear, with no threshold.
- **Pattern change:** one rebuild of the new pattern (~1 M), about 5 ms.

So in the emulator, lock density is a small, linear cost: ISR step-start +3 %, save/load +35 %. There is no cliff
between 76, 92 and 128 patterns.

## 6. Hardware timing caveat: the cache model run [measured + inferred]

`DTEMU_CACHE=1` (cache.rs: 16 KB I and D, 4 ways, 16-byte lines, copyback for 0x40000000-0x47FFFFFF, the
0x48000000+ alias uncached; no timing). ISLAND, stock, 3 s emulated (1 s stopped, 2 s playing), 323 M instructions
executed: I-cache 482 M fetches, **3.64 M misses (1.1 per 100 instructions)**; D-cache 103.6 M accesses, **0.36 M
misses** (0.11 per 100), 0.15 M write-backs.

[inferred] At 250 MHz with a ~30-cycle line fill, the I-misses alone add ~0.34 cycles per instruction. That is
before the uncached accesses, which the model counts nowhere: the voice code reads samples through the uncached
0x4b... alias, and there are SRAM/IO and SPI accesses besides. With an effective CPI of c, the ISR's share is
`ISR_instr * c / 166,667 cycles`.

| c | ISLAND worst block (75.5 k) | synthetic worst (90.2 k) |
|---|---|---|
| 1.0 | 45 % | 54 % |
| 1.4 | 63 % | 76 % |
| 1.8 | 82 % | 97 % |
| 2.2 | 100 % (budget gone) | >100 % |

A plausible hardware CPI of 1.4 to 1.8 puts heavy projects close to the edge, where a few more trigs per step or a
longer save/load scan starves the UI. This matches the shipped compressor estimator's history
(rytm-mods `re/subsystems/compressor-hardware.md`): on the device, about **10 %** of a block on a busy pattern made
the UI queue (0x41943f54) grow into the thousands and pattern changes wait seconds, while **4 %** shipped fine.

## 7. Budgets for our mods

**Audio ISR (per 32-sample block; a block is 133,333 instructions in the emulator, ~166,667 cycles on hardware):**

- Total added by all mods together: **<= 2,700 instructions per block (2 %)** on a step-start block. That is half
  the 4 % that was proven safe on hardware, kept as margin for heavy projects. The firm ceiling is 5,300 (4 %).
  Never exceed it, and stand aside (as the estimator does) when the UI queue at 0x41943f54+4 or the BgWorker deque
  backs up.
- Current use: 0025 sfx_hook 70 mean / 119 max. 0021 adds nothing without EXT tracks.
- **Open:** `ext_render` with every track on an EXT machine (12 calls per block on step starts). Measure it before
  it ships. Target <= 150 instructions per call, so that 12 tracks plus 0025 stay under 2,700.
- Nothing proportional to the pattern count or the lock count in the ISR.

**UI task (per 20 Hz render tick, or per UI message):**

- Hooks: **<= 10,000 instructions per tick on average** (0.1 % of CPU) and **<= 100,000 in any single call**
  (0.5 ms, under the stock tick's own 240 k max). Keep O(1) work per message, as the loader spec already says.
- Never add a per-pattern scan to SAVE/LOAD: those already walk all 129 patterns. At ~430 k each, every extra 1 k
  per pattern adds 129 k per save.
- The tick never slipped in any scenario, and page-key latency is ~1 ms. The budget protects hardware headroom,
  not emulator numbers.

**Heap (16 MB buddy arena at 0x41ac1340):**

- At rest: 13.86 MB free on the mod builds. The worst low-water measured is **9.67 MB** (heavy-project LOAD PROJECT,
  mod builds). ISLAND's worst is 12.80 MB (first load of the 1.72 upload) and 13.60 MB on later loads.
- A **permanent block of up to 32 KB is safe**: 0.3 % of the worst-case margin. It is the same order as the mods'
  existing 13.5 kB. The firm cap is 64 KB. Allocate once at boot and never during LOAD/SAVE.

## 8. Reproduction (session scratchpad, git-ignored data; nothing derived from firmware is committed)

- `ek.py`: data-protocol frames. `smp.py`: Transfer-style sample sync (ids = the next inode, starting at 47 on
  rytm.img). `conv.py`: WAV -> Elektron sample, with the hash check. `fullprep2.sh`: cold boot, sync, upload,
  LOAD 004.
- `stress/src/main.rs` (cargo crate on the rytm-emu library): `stress FW SNAP CARD --until +N --win N --press K@+T[:H]
  --probe K@+T --span NAME:START:END|ret --heap STATE:PCS`.
- `scen.sh`: the scenario. `ana.py`: pattern statistics. `plus1.py`: ISLAND + 1 bank. `var.py` / `runvar.sh`: the
  lock sweep. `summ.py`: the table rows. Logs are in `islres/`.
- Caveats: the project's own sample slots did not resolve (stand-in samples were used). In the sync, replies
  for the first two files were not captured, but their sizes match the project. Pattern T6's 358 ms is the queued
  change, not a stall.
