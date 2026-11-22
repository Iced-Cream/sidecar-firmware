// core/device/admit.h - CPU admission of a box's voices at attach (two-box design, decided 2026-11-21, item 8 of the
// project's design alignment; docs/device-profiles.md "Voices and CPU admission").
//
// Each box asks for its type's voices (Profile::voices: the Rytm 8, the A4 4, the Digitakt 8), or the cmdline's
// voices= for its type. When a box attaches, its voices are admitted against the boot bench's costs, scaled to the CPU
// clock setting, so that each render core stays under kLimitBp (80 % of a core) in the worst case:
//   - a voice costs the worst machine in the bench (any voice may play any machine), plus its INSERT chain at the worst
//     TYPE pair and the POST EQ, plus one AMP instance while the AMP cap (amp::kMaxRunning) allows another on that core, for
//     the base box's voices only (AMP plays on the base box's chains, its voices' and its master's: core/fx/insert.cpp
//     voice_out, master.cpp render; the other box's AMP slots play dry), so the other box's chains leave out the AMP pair
//   - each box held adds its own send FX, send TYPE inserts, bus and master chains (with its AMP pair), measured together
//     as one bench row, on the render core that runs them: slot 0's on core 2, slot 1's on core 1 (chain_core; decided
//     2026-11-22: LT1 found core 2 ~95 % with both boxes' chains on it and core 1 ~70 % idle)
//   - a box's voices are its voices 0..n-1: voices 0..s-1 on core 1, s..7 on core 2, s its split (split_for: the cmdline's
//     split=, or half the voices its box asks for if fewer: an A4's 4 as 2 + 2, so its voices and the chains share both cores)
// The first box keeps its voices (it was alone: nothing to take them from). The second gets the most that fit beside the
// first one's (never more than it asked for), is told (a popup on its screen), and /status shows the shortfall. A box whose
// partner leaves gets back what it asked for. Nothing here runs per sample; the render cores read the counts (device.h
// admitted) once per voice.
//
// No bench (bench=0, the host, the Teensy): nothing measured, so nothing is refused (every box its count; /status says so).
#pragma once
#include <stdint.h>

namespace core {
namespace admit {

constexpr unsigned kLimitBp = 8000;   // 80 % of a render core, in 1/100 % (bp)
constexpr int kAmpCap = 6;            // fx/amp.h kMaxRunning: AMP slots playing at once, engine-wide

// The boot bench's figures, each in 1/100 % of a core at `mhz` (kernel.cpp LogBench).
struct Costs {
  unsigned mhz;        // the clock the bench ran at; 0: no bench (admission off)
  unsigned voice_bp;   // one voice at the worst machine of the bench (its mean over 1 s)
  unsigned ins_bp;     // one voice's INSERT chain: the worst TYPE pair, plus the POST EQ (bench ins rows / 8)
  unsigned amp_bp;     // one AMP instance (bench amp)
  unsigned fx_bp;      // one box's chains on its render core (chain_core): the bench's box chains row (its send FX, bus and master chains at the
                       // worst set the rooms allow, as one run: master.h box_bench) + its master AMP pair. Before LT1
                       // (2026-11-22) the sum of overlapping rows; still that when the boot has no box row
};

void set_costs(const Costs& c);
const Costs& costs();
void set_clock(unsigned mhz);   // the CPU clock setting now (cpumhz=); the costs scale by costs().mhz / mhz
unsigned clock();
void set_split(unsigned n);     // split=: voices 0..n-1 of each box on render core 1 (1..7)
unsigned split();
// the split of a box that asks for `asked` voices: split(), or half of asked (rounded up) if that is fewer; 0 asked
// (a free slot): split(). The Pi's cores.cpp renders by it (core 3 puts it in each pass), core_bp counts by it
unsigned split_for(unsigned asked);
bool on() ;                     // a bench to admit against
// admit=report (console, the two-box load test): the costs, core_bp and fit stay, but no box is refused (every box its
// count) so the worst case can be measured against core_bp; admit=enforce (the default) refuses again
void set_enforce(bool on);
bool enforce();

// The render core that runs slot s's send FX, bus and master chains (the Pi's cores.cpp ChainCore): slot 1's core 1, slot 0's core 2
constexpr int chain_core(int s) { return s == 1 ? 1 : 2; }

// A load: voices[s] of slot s's box render (its 0..voices-1), held[s] its chains run (on chain_core(s)), asked[s] what it
// asked for (its split: split_for; 0: split()). The base box (AMP's): slot 0 while held, else slot 1 (device.h g_base).
struct Load {
  uint8_t voices[2];
  bool held[2];
  uint8_t asked[2];
};
// Render core 1 or 2's worst load for l, in bp at the clock now. Another core: 0.
unsigned core_bp(int core, const Load& l);
// The most voices (<= want, which is also what it asked for: its split) slot s may render beside the other slot's in l, so that every render core this box's voices land
// on stays within kLimitBp. A core already over without this box's voices (by the chains alone) takes none of them.
int fit(int s, int want, const Load& l);
// What slot s gets at attach: fit() while enforcing, else want (0..8)
int admit(int s, int want, const Load& l);

// cmdline voices=: per box type, "rytm:8,a4:2,dt:6" (dt or digitakt). Each count 0..8, at most its profile's (it lowers a
// count: device.cpp voices_asked); false: not understood (nothing changed).
bool parse_override(const char* text);
int override_for(const char* type_short);   // -1: none ("Rytm", "A4", "Digitakt": device::short_name, case ignored)
void reset();                                // the defaults (tests)

}  // namespace admit
}  // namespace core
