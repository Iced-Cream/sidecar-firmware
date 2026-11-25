# The measured-time guard of every effect

Decided 2026-11-23: Plugin API 3's effect budget (docs/plugin-api-3-plan.md "The effect's budget", core/user/um.cpp
`fx_run`) applies to every effect, built-in or not. Code: core/fx/fx_guard.h, .cpp; tests: core/tests/test_fx_guard.cpp.

CPU admission (core/device/admit.h) stays the first layer: it predicts each slot's cost from the boot bench and refuses
or sheds a slot that would not fit (NO CPU). This guard is the second: it measures what really runs and catches what the
model did not foresee (a Pi without a fan throttled to 600 MHz, the L2 shared under load).

## What is timed

- Every INSERT slot of every chain, each box: the voices' (V1..V8), the buses' (B1..B3) and the master's (M1, M2), slots A
  and B. AMP and the master's send TYPEs run inside an INSERT slot, so they are covered. A slot that is OFF, refused or
  shed does not run and is not timed.
- Each send FX slot's built-in TYPE (FX1, FX2), STUTTER's insert as its slot's.
- User effects (UFX1..UFX4) keep um.cpp's own guard; their time also counts on their core (`note_user`).
- The POST EQ is not timed (decided 2026-11-23: cheap, not an effect slot).

Cost: two counter reads (CNTPCT) per running slot per 16-frame chunk on the core that runs it.

## When an effect is muted

A core is hot when its block took 80 % of a block or more (render cores 1 and 2: voices and chains, as the voice guard's
`kHotPct`), and core 3 when slot 0's master chain took what core 3's limit leaves after its own work
(admit.h `kC3LimitBp - kC3BaseBp` at the clock now; about 57 % of a block at 1500 MHz). Decided 2026-11-23: a built-in
effect is muted only when its core is hot, since its cost is known from the bench.

1. One call of 2 ms or more (the FIFO): muted at once, hot or not.
2. A slot over its budget (120 us a 12-frame block, scaled by the frames: API 3's) for 16 blocks in a row while its core
   is hot.
3. A core hot 16 blocks in a row: the dearest effect on it over those blocks (built-in or user) is muted, and the count
   starts again; a core still hot mutes the next one 16 blocks later. After any mute on a core (any rule), its dearest
   rule waits 32 blocks (8 ms: the muted slot's 5 ms fade still counts its time), so one slot over its budget never
   takes a second effect with it (found in the LT12 prep, 2026-11-24).
4. A late block (the core took more than a whole block, so the output FIFO drains): the block's effect most over its
   budget is muted at once; if none is over its budget, the block's dearest after 4 late blocks in a row (1 ms), so one
   block made late by something else never takes an effect. A late mute fades out over 1 ms instead of the TYPE's 5 ms
   (send FX: 1 ms instead of 10 ms), because the slot still costs its time while it fades on a core already late.
   Decided 2026-11-24 (Connor) after LT12b: a slot 180 us over a core at ~61 % made every block late for the 16-block
   wait and the 5 ms fade, ~18 ms of underruns heard as a loud hit. Core 3: late when slot 0's master chain took what a
   block leaves after core 3's own control work.
   A render core's block is also late when its base pass began with the output FIFO under a block (12 frames) and it
   took longer than the other render core in the last block: the busiest core is late for the whole block path, its
   own time plus core 3's work around it. Each dip holds for 32 base passes (8 ms), since the control loop refills the
   FIFO over a pass and only its first block begins low (LT12h P5: unheld, late and on-time blocks alternated and the
   4-in-a-row never came). Not for 64 passes after a stream (re)start, whose FIFO begins empty (pi/engine/fifo_late.h).
   Decided 2026-11-25 (Connor) after LT12g P5: two slots 20 + 40 us slower put core 1 at 83 % with its chains, no core
   past a whole block and never 16 hot blocks in a row, yet the FIFO drained for 14 s (underruns heard as clusters of
   metallic sounds) and no rule fired. fxguard=fifo:off turns this part off (QEMU, whose FIFO is short under any load).

The NaN / runaway check stays as before (the slot cleared, it fades in again).

## Muted

The slot fades out over the TYPE fade (5 ms) and plays dry; a send FX slot's return goes silent. An INSERT slot shows
NO CPU on the box (`ins::refused`); the send FX slots have no NO CPU state on the box (the Rytm draws its own send FX
page), so theirs is on the web page only.

It holds (decided 2026-11-23) until the slot is set again (its TYPE changes, `set_type` with the same TYPE, a kit that
sets it) or its box attaches again; then it fades in. No retry when the Pi cools, and the admission's `regrant` never
brings it back. A muted slot is still counted by the admission (an upper bound).

## Reporting

- /status: `"fxMuted":[{"box":0,"at":"V1 A","type":"DRIVE","user":false,"why":"muted: ran 299 us, over its budget (120
  us a block) on hot core 1"}]` with the user effects that are muted beside them (`"user":true`, `at` UFX1..UFX4, `type`
  the plugin id, `why` um.cpp's), and `"fxGuard":{"mode","budgetUs","mutes"}`.
- The web page: "Effects muted for CPU" (the FX tab), shown while anything is muted.
- The log: `fxguard: box 0 V1 A DRIVE muted: ...`, `fxguard: box 0 V1 A released (set again)`.

## Console and cmdline

- `fxguard`: the state. `fxguard=on|off|report` (report: logs what would be muted, mutes nothing). `fxguard=budget:<us>`.
- Tests and gates: `fxguard=slow:<box>:<V1..V8|B1..B3|M1|M2>:<A|B>:<us>` (or `:FX1:<us>`) adds busy time inside the
  slot's call; `fxguard=mute:<box>:<at>` mutes it; `fxguard=clear` releases every mute and slow.
- cmdline.txt `fxguard=off|report|on` (on by default).
