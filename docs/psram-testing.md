# Sidecar Mini with PSRAM (testing)

A Teensy 4.1 with PSRAM fitted gets a second plugin code arena in PSRAM, so more and bigger user machines load at
once. A stock Teensy 4.1 (no PSRAM) keeps working exactly as before: same 32 KB arena, same numbers, same slot states.
Written and tested on the Mac (simulated arenas) and compiled for the Teensy; **nothing of the PSRAM path has run on
PSRAM hardware yet** - this page is what testers run and report.

## Which PSRAM

- One or two **APS6404L-3SQR** (8 MB each, SOIC-8) on the bottom pads of the Teensy 4.1: 8 or 16 MB.
  ESP-PSRAM64H is the same die and works the same.
- Nothing to configure: Teensyduino's startup code reads the chips' IDs over FlexSPI2 at boot and sets
  `external_psram_size` (8 or 16; 0 without). The firmware is the same binary for both boards.

## How it works

- **Two arenas, one rule.** `rmp_arena_alloc` takes the internal arena (32 KB in DTCM, the fastest) first and the PSRAM
  arena (1 MB by default, `-DRMP_PSRAM_ARENA_BYTES=...`, a power of two 64 KB..8 MB) when the internal one has no room.
  A package bigger than the internal arena goes straight to PSRAM. NO_ROOM only when neither has room. Both are bump
  allocators reset together at a full reload (a project change, or an incremental assign that found no room), as before:
  nothing fragments. A full load places in slot order, so **U1 gets the internal arena first**: put the heaviest machine
  on the lowest slot. (`teensy/libraries/RytmPlugins/src/rmp_arenas.*`, tested by `extras/arenas_test.cpp`.)
- **Memory protection and caches.** The core marks all of FlexSPI2 (0x7000_0000, region 9) no-execute. The PSRAM arena
  gets MPU regions 11 and 12 (two windows of the arena's size, 8 subregions each, only the ones over the arena enabled):
  normal memory, write-back write-allocate cacheable (as region 9), read/write, **executable**. The block comes from
  `extmem_malloc` (size + 1/8 for the subregion alignment). After a package is copied and relocated, the loader cleans
  the D-cache over the image (the code reaches PSRAM) and invalidates the I-cache (no line of an older image survives) -
  the same `rmp_arena_sync` as for the internal arena.
- **The guard.** Faults are caught by context (the call's exception level), not by address, so a fault in PSRAM code or
  data (MemManage, BusFault, an imprecise BusFault from FlexSPI2) mutes the slot exactly as in the internal arena. Code
  at an address outside both arenas still faults (XN) and is caught.
- **Limits.** PSRAM code is slower when the caches miss, so its slots get longer limits (`rmp_arenas.h`):
  - over-budget count (3 renders in a row over it mute the slot): 3 x the internal limit (`RMP_PSRAM_LIMIT_X`);
  - guard timeout: that plus 300000 cycles (`RMP_PSRAM_COLD_CYCLES`, 0.5 ms: a call on cold caches);
  - the per-call limits (voice_init, params, note_on/off, init) the same way. Internal slots: unchanged.
- **Where it shows.** `:psram`; the boot log; the info JSON (`arena.psram_size`, `arena.psram_used`, each loaded slot's
  `place`: `internal` or `psram`); the web console's memory bars ("Code arena (PSRAM)") and the user-machine list
  ("READY 1.0.0 · arena" / "· PSRAM"); the "ready" log line ends "code in PSRAM".

## The PSRAM split (shared with the recorder)

PSRAM is shared out once at boot, in this order (`extmem_malloc`, the core's PSRAM heap):

| who | size | where set |
|---|---|---|
| sample pool (DSP SAMPLE) | all but 4 MB (with an SD card) | `core/pool/sample_ram_pool.h` `kReserve` |
| plugin state (> 16 KB a voice) | 2 MB | `rmp_state.cpp` `RMP_STATE_PSRAM_BYTES` |
| plugin code arena | 1 MB + 128 KB alignment | `rmp_arena.h` `RMP_PSRAM_ARENA_BYTES` |
| left (recorder ring, looper) | ~0.9 MB of the 4 MB reserve | take it with `extmem_malloc` after `plug_setup()` |

A bigger recorder ring needs a bigger `kReserve` (or a smaller code arena); change the numbers in these three places
together.

## Expected performance (estimate, not measured)

From the core's FlexSPI2 setup (Teensyduino 1.62 startup.c: 105.6 MHz, QPI SDR, 6 dummy clocks): one 32-byte cache
line from PSRAM ~78 bus clocks ~0.74 us ~ **450-480 CPU cycles** (88 MHz: ~560, 132 MHz: ~380) [inferred].

- **Warm caches** (the machine's hot code in the 32 KB I-cache, its tables in the 32 KB D-cache): as fast as the internal
  arena (a cache hit has no wait state; DTCM code was measured as fast as ITCM, docs/plugins.md 9.1). Expect 1.0-1.1x.
- **Cold**: every line touched pays ~480 cycles. sine_test's hot loop (~1 KB, 32 lines): ~15000 cycles on a cold call; a
  Noise Plethora package (4-8 KB of hot code, 125-250 lines): 60000-120000 cycles - up to 0.8 of a 12-sample block
  (150000 cycles) - on the first call after a load or after the caches were flushed.
- **Steady state** is between: 8 voices of one machine share its lines (the first voice of a block pays, the other 7
  hit); what evicts them between blocks is the loop's flash code (USB, SD, LittleFS) and the sample pool's PSRAM reads.
  If a quarter of an NP package's lines are lost per block: ~30000 cycles a block, ~20 % of the Teensy. The NP packages
  already take 37-80 % of the Teensy with 8 voices from the internal arena (docs/plugins.md 9.5), so **keep the heaviest
  machine internal** (U1) and use PSRAM for the lighter or occasional ones. The bench below measures what is real.

## Checking detection

1. Boot with the micro-USB serial open (or the web console's log). With PSRAM:
   `plugins: PSRAM code arena 1024 KB at 0x70xxxxxx (MPU regions 11, 12)`. Without: no such line (stock behaviour).
2. Console `:psram`:
   ```
   psram: 16 MB fitted; code arena internal 9032 of 32768 B, PSRAM 0 of 1048576 B
   psram: arena at 0x70...; MPU region 11 RBAR 7.....1b RASR 0.....27, region 12 ... (XN bit 28 must be 0)
   psram: limits for PSRAM code: budget x3, timeout x3 + 300000 cycles
   psram: U1 befaco.np.a READY, in internal
   ```
   RASR bit 28 (XN) must be 0 and bit 0 (enable) 1 on at least one of the two regions.
3. Web console, Memory: a "Code arena (PSRAM)" bar.

## The self-test (`:psram bench`)

Stop the transport first: the bench blocks the Teensy's loop for about a second (no audio meanwhile).

- `:psram bench` - sine_test (`rytm.sine_test` must be installed); `:psram bench befaco.np.a` - any installed package.
- It places the package twice, in the free tail of each arena (the project's slots keep theirs; the space is given back
  afterwards), and prints:
  - the PSRAM copy read back past the D-cache: `same` (the PSRAM holds what was written);
  - for each copy: init cycles, render first / average / peak, then 200 renders with the plugin evicted from both caches
    first (cold), against the budget;
  - `PSRAM / internal: warm avg xA.BB, cold avg xC.DD; output identical` (both copies render bit for bit the same);
  - the stress check: 20 PSRAM reloads (each read back), 8 voices x 500 blocks from the PSRAM copy with a 64 KB PSRAM sweep
    between rounds (the D-cache turned over), every call against the PSRAM budget and timeout; the 8-voice block peak;
  - `PASS` or `FAIL`.
- With the internal arena full the internal copy is skipped (`no room in this arena`): load an empty project, or one with
  small machines, for the comparison.

## What to report

1. The board: PSRAM chip(s), 8 or 16 MB; Teensyduino version if you built it yourself.
2. The boot lines starting `plugins:` and `PSRAM:`.
3. The whole output of `:psram`, `:psram bench` and `:psram bench befaco.np.a` (or another NP package).
4. A real project: three or more NP packages assigned so one lands in PSRAM (`:psram` shows `in psram`), 8 voices playing
   for a minute: any `over budget` / `timed out` / `CRASHED` mutes, the host's underrun counters, and the device-info
   report's `avg_permille` / `peak_permille` for each slot (with its `place`).
5. Anything odd: a slot BAD on PSRAM that is READY internally, a different output, a reset (safe mode on the next boot).
