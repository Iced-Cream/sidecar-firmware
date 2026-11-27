// fifo_late.h with the measured FX guard (core/fx/fx_guard.cpp): LT13 P5's shape (2026-11-25, T509) on the host.
// Two boxes take turns: a base pass (the A4, slot 0) then a side pass (the Rytm, slot 1), each on cores 1 and 2. The Rytm's
// V1 A and V2 A (core 1, side passes) are slowed by 20 and 40 us, the cycle goes past a block, and the FIFOs drain to the
// floor and stay there (the control loop cannot refill them). On the unit the mute came 15.2 s late: the FIFO pinned low
// gave one dip and no hold, and the late core flipped between 1 and 2 pass by pass. Here the guard must mute V2 A within
// 20 ms of the overload, exactly one effect, and the FIFOs must climb back.
//
// The FIFO model: each pass drains 12 frames for every 250 us of the slower core's time plus core 3's own, and adds the
// block it rendered (12 frames); the level starts at the target. A muted slot costs nothing from its next block.
#include <stdio.h>
#include <string.h>

#include <cstdarg>
#include <string>

#include "check.h"
#include "fifo_late.h"
#include "fx/fx_guard.h"

static std::string g_log;
namespace core {
uint32_t cycles() { static uint32_t c; return c += 1000; }
uint32_t cycles_per_second() { return 54000000; }
uint32_t g_ms;
uint32_t now_ms() { return g_ms; }
void log(const char* fmt, ...) {
  char b[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  g_log += b;
  g_log += "\n";
}
}  // namespace core
extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap) { return vsnprintf(out, n, fmt, ap); }
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r;
}

namespace g = core::fx::guard;

static const uint32_t kBlock = 12, kTarget = 96;
static const uint32_t kTicksUs = 54;   // cycles_per_second / 1e6

struct Fx { int unit; int core; bool side; uint32_t us; };

struct Result { int mutes; int first_mute_pass; int muted_unit; uint32_t under; uint32_t level_end; uint32_t fits;
                uint32_t cyc_late, cyc_fits, pass_late, ext_under; };

// cores.cpp's gate (fifo_late.h counts, decided 2026-11-26 after VA1): a FIFO-late pass counts for its core only at or
// over admission's limit for its time a block, or after a real underrun. g_gate off: the guard as before (the shape's
// precondition). g_stall_every: every so many passes a long control pass (g_stall_us) and 22 passes later a shorter one
// (g_stall2_us), rendering nothing while both FIFOs play on: two dips (VA1: pass max 2-3 ms in normal play, 7 dips in the
// mute's second).
static bool g_gate = true;
static int g_stall_every = 0;
static uint32_t g_stall_us = 0, g_stall2_us = 0;
// cores.cpp's late cycle (fifo_late.h cycle_late, decided 2026-11-26 after AL1, the owner's pick A): with g_cycle the base
// pass holds the guard's late run and the side pass counts the cycle (its base pass plus itself past a block, gated as
// above). Off: each pass alone, as before. g_fifo: the FIFO's view (fxguard=fifo:on|off). g_spike_every: every so many
// side passes core 1's own work there grows by g_spike_us (AL1 P5: the Rytm's side passes alone went past a block).
static bool g_cycle = true, g_fifo = true;
static int g_spike_every = 0, g_burst_at = -1, g_burst_len = 0;
static uint32_t g_spike_us = 0, g_burst_us = 0;
// g_cycle_gate80: the late cycle's first gate (PR #58 1722936-bae3fb4: admission's 80 % or an underrun, as the FIFO's),
// which LC1 disproved (2026-11-27). g_burst_every: the burst again every so many passes. g_ext_under_at: the outputs
// underrun for g_ext_under_len passes from there with the core's work untouched (LC1: a load setup's reload, +234).
static bool g_cycle_gate80 = false;
// g_gate_fast: the FIFO view's gate on the fast mean (Load, ~4-8 ms) as before LC2; else the slow mean (LoadSlow, ~64 ms)
static bool g_gate_fast = false;
static int g_burst_every = 0, g_ext_under_at = -1, g_ext_under_len = 0;
static uint32_t stall(int p) {
  if (!g_stall_every || p < 2 * (int)fifo_late::State::kGrace) return 0;
  const int k = p % g_stall_every;
  return k == 0 ? g_stall_us : k == 22 ? g_stall2_us : 0;
}

// passes: base and side alternate (pass 0 base). slow_at: the pass the slow commands land. Per pass kind, each core's
// own work outside the listed effects (voices, other chains), us.
static Result run(int passes, int slow_at, const uint32_t own[2][3], const Fx* fx, int nfx, uint32_t c3_us,
                  uint32_t slow_v1, uint32_t slow_v2, int v1, int v2) {
  g::reset();
  fifo_late::Pair fl;
  uint64_t last[2][3] = {};
  double level[2] = {kTarget, kTarget};
  Result r = {0, -1, -1, 0, 0, 0, 0, 0, 0, 0};
  uint32_t late_seen = 0;
  fifo_late::Load load[3];
  fifo_late::LoadSlow loads[3];
  fifo_late::Under under;
  // the grace and a quiet window first
  for (int p = 0; p < passes; ++p) {
    const bool side = p & 1;
    const uint32_t lvl = (uint32_t)level[side];
    const uint32_t nl = g::stats().late;
    if (nl != late_seen) late_seen = nl, fl.rearm();
    const bool late = fl.step(side, lvl, 1, kBlock, g_fifo, true);
    for (int c = 1; c <= 2; ++c) load[c].step(last[0][c] + last[1][c]), loads[c].step(last[0][c] + last[1][c]);
    if (g_ext_under_at >= 0 && p >= g_ext_under_at && p < g_ext_under_at + g_ext_under_len) ++r.ext_under;
    under.step(r.under + r.ext_under);
    unsigned lc = late ? fifo_late::blame(last[0][1], last[1][1], last[0][2], last[1][2]) : 0;
    if (lc && g_gate && !fifo_late::counts(g_gate_fast ? load[lc].bp(250) : loads[lc].bp(250), 8000, under.recent()))
      lc = 0, ++r.fits;
    uint32_t t[3] = {0, own[side][1], own[side][2]};
    if (side && g_spike_every && (p / 2) % g_spike_every == 0) t[1] += g_spike_us;
    if (g_burst_at >= 0 && p >= g_burst_at &&
        (g_burst_every ? (p - g_burst_at) % g_burst_every : p - g_burst_at) < g_burst_len) t[1] += g_burst_us;
    for (int c = 1; c <= 2; ++c) {
      g::set_core(c);
      for (int i = 0; i < nfx; ++i) {
        const Fx& f = fx[i];
        if (f.core != c || f.side != side || g::muted(f.unit)) continue;
        uint32_t us = f.us;
        if (p >= slow_at && f.unit == v1) us += slow_v1;
        if (p >= slow_at && f.unit == v2) us += slow_v2;
        g::note(f.unit, us * kTicksUs, 12);
        t[c] += us;
      }
      last[side][c] = t[c];
      bool lt = lc == (unsigned)c || t[c] >= 250, hold = false;   // (cores.cpp: the pass alone past a block, or the FIFO's)
      if (t[c] >= 250) ++r.pass_late;
      if (g_cycle) {
        if (!side) hold = true;
        else if (g_cycle_gate80 ? last[0][c] + t[c] >= 250 && fifo_late::counts(load[c].bp(250), 8000, under.recent())
                                : fifo_late::cycle_late(last[0][c], t[c], 250, load[c].bp(250)))
          lt = true, ++r.cyc_late;
        else if (last[0][c] + t[c] >= 250) ++r.cyc_fits;
      }
      g::core_done(c, t[c] >= 200, lt, hold);
    }
    // the FIFO: drained by this pass's time (the slower core, then core 3's own), refilled by the block
    const uint32_t pass_us = (t[1] > t[2] ? t[1] : t[2]) + c3_us + stall(p);
    for (int k = 0; k < 2; ++k) {
      level[k] -= 12.0 * pass_us / 250.0;   // (each FIFO plays 12 frames every 250 us)
      if (k == (int)side) level[k] += 12;   // (the block this pass rendered)
      if (level[k] < 0) level[k] = 0, ++r.under;
      if (level[k] > kTarget) level[k] = kTarget;
    }
    if (r.first_mute_pass < 0 && g::stats().late) r.first_mute_pass = p;
  }
  for (int u = 0; u < g::kBuiltin; ++u)
    if (g::muted(u)) r.muted_unit = u, ++r.mutes;
  r.level_end = (uint32_t)level[1];
  g::poll();
  return r;
}

int main() {
  // the boxes: box 0 the A4 (base), box 1 the Rytm (side)
  const int v1a = g::ins_unit(1, 0, 0), v2a = g::ins_unit(1, 1, 0);
  const Fx fx[] = {
      {g::ins_unit(0, 0, 0), 1, false, 3},   // A4 V1 A, core 1
      {g::ins_unit(0, 2, 0), 2, false, 4},   // A4 V3 A, core 2
      {v1a, 1, true, 6},                     // Rytm V1 A SAT, core 1
      {v2a, 1, true, 5},                     // Rytm V2 A SAT, core 1
      {g::ins_unit(1, 4, 0), 2, true, 3},    // Rytm V5 A, core 2
  };
  // own work a pass: base (A4) heavier on core 2, side (Rytm) heavier on core 1 (the flip LT13 showed)
  // (quiet: 221 us a cycle; slowed: 281 us, an overload the FIFO cannot ride out; V2 A muted: 236 us, it climbs back)
  const uint32_t own[2][3] = {{0, 70, 86}, {0, 80, 50}};
  const uint32_t c3 = 20;   // core 3's own after the render wait
  const int kSlowAt = 2 * (int)fifo_late::State::kGrace + 400, kPasses = kSlowAt + 8000 * 2;   // ~2 s after the slow
  {  // without the slow: never a mute, never an underrun
    Result r = run(kPasses, 1 << 30, own, fx, 5, c3, 0, 0, v1a, v2a);
    CHECK(r.mutes == 0 && r.under == 0);
    printf("test_fifo_late_guard: no slow: %d mutes, %u underruns\n", r.mutes, r.under);
  }
  {  // LT13 P5: V1 A +20 us, V2 A +40 us
    Result r = run(kPasses, kSlowAt, own, fx, 5, c3, 20, 40, v1a, v2a);
    const double ms = r.first_mute_pass < 0 ? -1 : (r.first_mute_pass - kSlowAt) * 0.125;   // (two passes a 250 us cycle)
    printf("test_fifo_late_guard: LT13 P5 shape: %d mute(s), first %.1f ms after the slow, unit %d (V2 A %d), "
           "%u underrun passes, side FIFO %u at the end\n", r.mutes, ms, r.muted_unit, v2a, r.under, r.level_end);
    CHECK(r.mutes == 1);
    CHECK(r.muted_unit == v2a);
    CHECK(ms >= 0 && ms < 20);
    CHECK(r.level_end >= 2 * kBlock);
  }
  {  // LT13 P5 with the gate: core 1 at ~90 % a block once slowed, over the limit: the same one mute, within 20 ms
    Result r = run(kPasses, kSlowAt, own, fx, 5, c3, 20, 40, v1a, v2a);
    CHECK(r.mutes == 1 && r.muted_unit == v2a && r.first_mute_pass >= kSlowAt && (r.first_mute_pass - kSlowAt) * 0.125 < 20);
    g_gate = false;
    Result o = run(kPasses, kSlowAt, own, fx, 5, c3, 20, 40, v1a, v2a);
    g_gate = true;
    // (the slow mean, after LC2: the gate opens a little later than ungated, and a few underrun passes may come first,
    // which open it at once; the fast mean opened it at the ungated pass)
    CHECK(o.mutes == 1 && o.first_mute_pass <= r.first_mute_pass && r.under <= o.under + 16);
    printf("test_fifo_late_guard: LT13 P5 gated: first mute %.1f ms after the slow (ungated %.1f ms), %u underrun passes "
           "(ungated %u)\n", (r.first_mute_pass - kSlowAt) * 0.125, (o.first_mute_pass - kSlowAt) * 0.125, r.under, o.under);
  }
  {  // VA1 (2026-11-26, T515) at 1200 MHz, admitted 79.77 %: core 1 measures ~73 % a block (the Rytm's FX1 CLOUDSEED
     // 18 us in its passes, ~9 us a pass as the guard counts it); two long control passes dip both FIFOs under a block,
     // no underrun. Ungated the guard muted FX1 (VA1's three mutes); gated nothing goes, nothing underruns.
    const int fx1 = g::send_unit(1, 0);
    const Fx va1[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},   // A4 V1 A, core 1
        {g::ins_unit(0, 2, 0), 2, false, 4},   // A4 V3 A, core 2
        {v1a, 1, true, 6},                     // Rytm V1 A SAT, core 1
        {v2a, 1, true, 5},                     // Rytm V2 A SAT, core 1
        {fx1, 1, true, 18},                    // Rytm FX1 CLOUDSEED, core 1 (slot 1's send FX)
        {g::ins_unit(1, 4, 0), 2, true, 3},    // Rytm V5 A, core 2
    };
    const uint32_t own1[2][3] = {{0, 70, 60}, {0, 80, 50}};   // core 1: 73 + 109 = 182 us a block (73 %)
    const uint32_t c3v = 5;                                   // (core 3's own: the cycle 192 us, the FIFOs refill)
    g_stall_every = 400, g_stall_us = 1900, g_stall2_us = 600;   // 91 frames of the 96 gone, then 29: under a block, not empty
    g_gate = false;
    Result o = run(kPasses, 1 << 30, own1, va1, 6, c3v, 0, 0, v1a, v2a);
    g_gate = true;
    printf("test_fifo_late_guard: VA1 shape ungated: %d mute(s) (unit %d, FX1 %d), %u underrun passes\n", o.mutes, o.muted_unit, fx1, o.under);
    CHECK(o.under == 0);
    CHECK(o.mutes >= 1);   // (the precondition: the old rule mutes in this shape)
    Result r = run(kPasses, 1 << 30, own1, va1, 6, c3v, 0, 0, v1a, v2a);
    CHECK(r.under == 0 && r.mutes == 0 && r.fits > 0);
    printf("test_fifo_late_guard: VA1 shape: ungated %d mute(s), gated %d (%u passes fit), 0 underruns\n", o.mutes, r.mutes, r.fits);
    g_stall_us = 2300;   // 110 frames: the FIFO runs empty (VA1 O1 P1's +4): a real underrun still mutes
    Result u = run(kPasses, 1 << 30, own1, va1, 6, c3v, 0, 0, v1a, v2a);
    CHECK(u.under > 0 && u.mutes >= 1);
    printf("test_fifo_late_guard: VA1 shape with underruns: %d mute(s), %u underrun passes\n", u.mutes, u.under);
    g_stall_every = 0, g_stall_us = 0, g_stall2_us = 0;
  }
  {  // LC2 SOAK (2026-11-27, T535) at 1200 MHz: core 1 at ~73 % (VA1's shape), the FIFO dipping under a block at long
     // control passes, no underrun, and a few long passes (a burst of notes) in each dip's hold. On the fast mean the
     // burst lifted core 1 over 80 % in the hold and the 8 us CLOUDSEED went; on the slow mean (~64 ms) nothing goes
    const int fx1 = g::send_unit(1, 0);
    const Fx lc2[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},
        {g::ins_unit(0, 2, 0), 2, false, 4},
        {v1a, 1, true, 6},
        {v2a, 1, true, 5},
        {fx1, 1, true, 18},                    // Rytm FX1 CLOUDSEED (~9 us a pass as the guard counts it)
        {g::ins_unit(1, 4, 0), 2, true, 3},
    };
    const uint32_t own1[2][3] = {{0, 70, 60}, {0, 80, 50}};   // core 1: 182 us a block (73 %)
    g_stall_every = 400, g_stall_us = 1700, g_stall2_us = 600;
    g_burst_at = 400 * 2 + 24, g_burst_len = 12, g_burst_every = 400, g_burst_us = 50;   // 12 passes +50 us in each dip's hold
    g_gate_fast = true;
    Result o = run(kPasses, 1 << 30, own1, lc2, 6, 5, 0, 0, v1a, v2a);
    g_gate_fast = false;
    Result r = run(kPasses, 1 << 30, own1, lc2, 6, 5, 0, 0, v1a, v2a);
    g_stall_every = 0, g_stall_us = g_stall2_us = 0, g_burst_at = -1, g_burst_len = 0, g_burst_every = 0, g_burst_us = 0;
    printf("test_fifo_late_guard: LC2 SOAK shape (73 %%, a burst in each dip): fast mean %d mute(s) (unit %d, FX1 %d), "
           "slow mean %d mute(s) (%u passes fit), %u underrun passes\n", o.mutes, o.muted_unit, fx1, r.mutes, r.fits, r.under);
    CHECK(o.under == 0 && o.mutes >= 1);   // (the precondition: LC2's false mute)
    CHECK(r.under == 0 && r.mutes == 0 && r.fits > 0);
  }
  {  // AL1 P5 (2026-11-26, T526): V1 A +20, V2 A +40 put core 1 at 84 % with its chains, the Rytm's side passes heavy and the
     // A4's base passes light; ~6 % of side passes went past a block alone, each followed by an on-time base pass, and the
     // FIFO stayed up (35-58, underruns +0). The late run never passed 2. Picked (A): this clean overload still mutes nothing.
    const Fx p5[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},   // A4 V1 A, core 1
        {v1a, 1, true, 6},                     // Rytm V1 A, core 1
        {v2a, 1, true, 5},                     // Rytm V2 A, core 1
        {g::ins_unit(1, 4, 0), 2, true, 3},    // Rytm V5 A, core 2
    };
    const uint32_t ownp5[2][3] = {{0, 20, 30}, {0, 115, 20}};   // core 1: 23 + 126 = 149 us (60 %); slowed 209 (84 %)
    g_spike_every = 16, g_spike_us = 80;                         // every 16th side pass 255-289 us alone: past a block
    g_cycle = false;
    Result o = run(kPasses, kSlowAt, ownp5, p5, 4, 5, 20, 40, v1a, v2a);
    g_cycle = true;
    Result r = run(kPasses, kSlowAt, ownp5, p5, 4, 5, 20, 40, v1a, v2a);
    printf("test_fifo_late_guard: AL1 P5 shape: passes alone late %u; per pass %d mute(s), per cycle %d mute(s) (cycles late %u, "
           "gated off %u), %u underrun passes\n", r.pass_late, o.mutes, r.mutes, r.cyc_late, r.cyc_fits, r.under);
    CHECK(o.pass_late > 0 && o.mutes == 0 && o.under == 0);   // (the precondition: late passes, never 4 in a row)
    CHECK(r.cyc_fits > 0 && r.cyc_late == 0);                 // (cycles past a block, gated off: core 1 under 95 %)
    CHECK(r.mutes == 0 && r.under == 0);
    g_spike_every = 0, g_spike_us = 0;
  }
  {  // the backstop with two boxes: core 1's cycle past a block at every cycle (V2 A slowed to 60 us), each pass alone
     // under a block, the FIFO's view off (fxguard=fifo:off). Per pass nothing ever mutes and the FIFO underruns on; per
     // cycle V2 A goes after 4 late cycles (~1 ms) and the FIFO climbs back
    const Fx bs[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},   // A4 V1 A, core 1
        {v1a, 1, true, 6},                     // Rytm V1 A, core 1
        {v2a, 1, true, 5},                     // Rytm V2 A, core 1
        {g::ins_unit(1, 4, 0), 2, true, 3},    // Rytm V5 A, core 2
    };
    const uint32_t ownbs[2][3] = {{0, 100, 30}, {0, 120, 20}};   // core 1: 103 + 131 = 234 us; slowed +55: 289 (116 %)
    g_fifo = false, g_cycle = false;
    Result o = run(kPasses, kSlowAt, ownbs, bs, 4, 5, 0, 55, v1a, v2a);
    g_cycle = true;
    Result r = run(kPasses, kSlowAt, ownbs, bs, 4, 5, 0, 55, v1a, v2a);
    g_fifo = true;
    const double ms = r.first_mute_pass < 0 ? -1 : (r.first_mute_pass - kSlowAt) * 0.125;
    printf("test_fifo_late_guard: two-box backstop (FIFO view off): per pass %d mute(s), %u underrun passes; per cycle %d "
           "mute(s) (unit %d, V2 A %d) %.2f ms after the slow, %u underrun passes, side FIFO %u at the end\n",
           o.mutes, o.under, r.mutes, r.muted_unit, v2a, ms, r.under, r.level_end);
    CHECK(o.pass_late == 0 && o.mutes == 0 && o.under > 1000);   // (the precondition: the old backstop is dead here)
    CHECK(r.mutes == 1 && r.muted_unit == v2a && ms >= 0 && ms < 2);
    CHECK(r.level_end >= 2 * kBlock && r.under < o.under / 10);
    // just over a block (V2 A +21 us: core 1 at 255 us, 102 %; LC1 P7's ~108 % is an estimate): the 95 % gate still opens
    g_fifo = false;
    Result j = run(kPasses, kSlowAt, ownbs, bs, 4, 5, 0, 21, v1a, v2a);
    g_fifo = true;
    const double jms = j.first_mute_pass < 0 ? -1 : (j.first_mute_pass - kSlowAt) * 0.125;
    printf("test_fifo_late_guard: two-box backstop at 102 %%: %d mute(s) (unit %d) %.2f ms after the slow\n", j.mutes,
           j.muted_unit, jms);
    CHECK(j.mutes == 1 && j.muted_unit == v2a && jms >= 0 && jms < 20);
  }
  {  // harmless lateness: a burst of 5 cycles past a block on a core at ~70 % (a burst of notes), the FIFO riding it out:
     // the gate holds (core 1 under 80 %, no underrun), nothing mutes
    const Fx hb[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},
        {v1a, 1, true, 6},
        {v2a, 1, true, 12},
        {g::ins_unit(1, 4, 0), 2, true, 3},
    };
    const uint32_t ownhb[2][3] = {{0, 60, 30}, {0, 95, 20}};   // core 1: 63 + 113 = 176 us (70 %)
    g_burst_at = kSlowAt, g_burst_len = 10, g_burst_us = 40;    // 5 cycles at 256 us, each pass alone under a block
    Result r = run(kPasses, 1 << 30, ownhb, hb, 4, 5, 0, 0, v1a, v2a);
    g_burst_at = -1, g_burst_len = 0, g_burst_us = 0;
    printf("test_fifo_late_guard: harmless burst at 70 %%: %d mute(s), cycles late %u, gated off %u, %u underrun passes\n",
           r.mutes, r.cyc_late, r.cyc_fits, r.under);
    CHECK(r.cyc_fits >= 4 && r.cyc_late == 0 && r.mutes == 0 && r.under == 0);
  }
  {  // LC1 SOAK (2026-11-27): core 1 at ~78 % with its chains (admitted, never overloaded), now and then a short run of
     // cycles past a block (a burst of notes), underruns +0. The first gate (80 % or an underrun) let the run's own cycles
     // lift the core's mean over 80 % and took a 10 us CLOUD; at 95 % nothing mutes
    const Fx sk[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},
        {v1a, 1, true, 20},                    // Rytm V1 A: a CLOUD (the guard's window reads it ~10 us, as LC1's)
        {v2a, 1, true, 6},
        {g::ins_unit(1, 4, 0), 2, true, 3},
    };
    const uint32_t ownsk[2][3] = {{0, 80, 30}, {0, 86, 20}};   // core 1: 83 + 112 = 195 us (78 %)
    g_burst_at = kSlowAt, g_burst_len = 12, g_burst_every = 400, g_burst_us = 70;   // 6 cycles at 265 us every 50 ms
    g_cycle_gate80 = true;
    Result o = run(kPasses, 1 << 30, ownsk, sk, 4, 5, 0, 0, v1a, v2a);
    g_cycle_gate80 = false;
    Result r = run(kPasses, 1 << 30, ownsk, sk, 4, 5, 0, 0, v1a, v2a);
    g_burst_at = -1, g_burst_len = 0, g_burst_every = 0, g_burst_us = 0;
    printf("test_fifo_late_guard: LC1 SOAK shape (78 %%, short bursts): gate 80 %% %d mute(s) (cycles late %u); gate 95 %% "
           "%d mute(s) (cycles late %u, gated off %u), %u underrun passes\n", o.mutes, o.cyc_late, r.mutes, r.cyc_late,
           r.cyc_fits, r.under);
    CHECK(o.mutes >= 1);                                      // (the precondition: LC1's false mute)
    CHECK(r.mutes == 0 && r.cyc_late == 0 && r.cyc_fits > 0 && r.under == 0);
  }
  {  // LC1 load setup (2026-11-27): the reload's underruns (+234, the outputs, not core 1) with core 1 at ~70 % and a burst
     // of cycles past a block: the first gate's underrun leg took CLOUD and both CLOUDSEEDs; at 95 % nothing mutes
    const Fx ls[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},
        {v1a, 1, true, 20},
        {v2a, 1, true, 8},
        {g::ins_unit(1, 4, 0), 2, true, 3},
    };
    const uint32_t ownls[2][3] = {{0, 60, 30}, {0, 85, 20}};   // core 1: 63 + 113 = 176 us (70 %)
    g_ext_under_at = kSlowAt, g_ext_under_len = 40;
    g_burst_at = kSlowAt + 20, g_burst_len = 12, g_burst_us = 80;   // 6 cycles at 256 us inside the underruns' window
    g_cycle_gate80 = true;
    Result o = run(kPasses, 1 << 30, ownls, ls, 4, 5, 0, 0, v1a, v2a);
    g_cycle_gate80 = false;
    Result r = run(kPasses, 1 << 30, ownls, ls, 4, 5, 0, 0, v1a, v2a);
    g_ext_under_at = -1, g_ext_under_len = 0, g_burst_at = -1, g_burst_len = 0, g_burst_us = 0;
    printf("test_fifo_late_guard: LC1 reload shape (70 %%, outputs underran): gate 80 %% %d mute(s); gate 95 %% %d mute(s) "
           "(gated off %u)\n", o.mutes, r.mutes, r.cyc_fits);
    CHECK(o.mutes >= 1);
    CHECK(r.mutes == 0 && r.cyc_late == 0 && r.cyc_fits >= 4);
  }
  {  // cycle_late: past a block and at or over kCycleLimitBp (95 %); no underrun leg (LC1)
    CHECK(fifo_late::kCycleLimitBp == 9500);
    CHECK(fifo_late::cycle_late(120, 140, 250, 9500));
    CHECK(!fifo_late::cycle_late(120, 140, 250, 9499));
    CHECK(!fifo_late::cycle_late(120, 140, 250, 8000));
    CHECK(!fifo_late::cycle_late(120, 120, 250, 9900));
    CHECK(!fifo_late::cycle_late(120, 140, 0, 9900));
  }
  {  // /status's fxMuted with many mutes (LC1 gate, 2026-11-27: ~49 mutes ran past kernel.cpp's 2 KB buffer and the reply
     // was cut mid-entry): every built-in unit muted, the JSON stays valid at 2 KB (what does not fit counted) and at 8 KB
    g::reset();
    static const char* at[] = {"V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "B1", "B2", "B3", "M1", "M2"};
    char c[64], rep[256];
    for (int b = 0; b < 2; ++b) {
      for (const char* a : at)
        for (const char* sl : {"A", "B"}) snprintf(c, sizeof c, "fxguard=mute:%d:%s:%s", b, a, sl), g::command(c, rep, sizeof rep);
      for (const char* f : {"FX1", "FX2"}) snprintf(c, sizeof c, "fxguard=mute:%d:%s", b, f), g::command(c, rep, sizeof rep);
    }
    int muted = 0;
    for (int u = 0; u < g::kBuiltin; ++u) muted += g::muted(u);
    auto valid = [](const char* j) {   // brackets balanced outside strings, strings closed
      int d = 0;
      bool str = false;
      for (const char* p = j; *p; ++p) {
        if (str) { if (*p == '\\') ++p; else if (*p == '"') str = false; continue; }
        if (*p == '"') str = true;
        else if (*p == '{' || *p == '[') ++d;
        else if (*p == '}' || *p == ']') --d;
        if (d < 0) return false;
      }
      return !str && d == 0;
    };
    static char small[2048], big[8192];
    const int ns = g::json(small, sizeof small), nb = g::json(big, sizeof big);
    std::string w = std::string("{") + small + "}", W = std::string("{") + big + "}";
    printf("test_fifo_late_guard: fxMuted with %d mutes: 2 KB %d B (%s), 8 KB %d B (%s)\n", muted, ns,
           strstr(small, "fxMutedMore") ? "some counted" : "all listed", nb, strstr(big, "fxMutedMore") ? "some counted" : "all listed");
    CHECK(muted == g::kBuiltin);
    CHECK(valid(w.c_str()) && strstr(small, "fxMutedMore") && strstr(small, "\"fxGuard\":{") && ns < (int)sizeof small);
    CHECK(valid(W.c_str()) && !strstr(big, "fxMutedMore") && strstr(big, "\"fxGuard\":{"));
    char tiny[128];
    g::json(tiny, sizeof tiny);
    CHECK(valid((std::string("{") + tiny + "}").c_str()) && strstr(tiny, "fxMutedMore"));
    g::reset();
  }
  {  // blame: the cycle, not the last pass (LT13: the last pass's longer core flipped between 1 and 2)
    CHECK(fifo_late::blame(90, 160, 120, 70) == 1);   // core 1 longer over the cycle, core 2 in the base pass
    CHECK(fifo_late::blame(120, 0, 90, 0) == 1);      // one box: its pass alone
    CHECK(fifo_late::blame(90, 0, 120, 0) == 2);
  }
  printf("test_fifo_late_guard: PASS\n");
  return 0;
}
