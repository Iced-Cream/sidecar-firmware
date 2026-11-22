// core/device/admit.h - CPU admission of a box's voices at attach (two-box design, decided 2026-11-21, item 8 of the
// project's design alignment; docs/device-profiles.md "Voices and CPU admission").
//
// Each box asks for its type's voices (Profile::voices: the Rytm 8, the A4 4, the Digitakt 8), or the cmdline's
// voices= for its type. When a box attaches, its voices are admitted against the boot bench's costs, scaled to the CPU
// clock setting, so that each render core stays under kLimitBp (80 % of a core) in the worst case:
//   - a voice costs the worst machine in the bench (any voice may play any machine), plus its INSERT chain at the worst
//     TYPE pair and the POST EQ, plus one AMP instance while the AMP cap (amp::kMaxRunning) allows another on that core
//   - each box held adds its own send FX, send TYPE inserts, bus and master chains (with its AMP pair) on render core 2,
//     measured together as one bench row (LT1 2026-11-22: core 2 ran ~95 % with both boxes' chains, the limit that binds)
//   - a box's voices are its voices 0..n-1: voices 0..split-1 of each box on core 1, split..7 on core 2 (cmdline split=)
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
  unsigned fx_bp;      // one box's chains on core 2: the bench's box chains row (its send FX, bus and master chains at the
                       // worst set the rooms allow, as one run: master.h box_bench) + its master AMP pair. Before LT1
                       // (2026-11-22) the sum of overlapping rows; still that when the boot has no box row
};

void set_costs(const Costs& c);
const Costs& costs();
void set_clock(unsigned mhz);   // the CPU clock setting now (cpumhz=); the costs scale by costs().mhz / mhz
unsigned clock();
void set_split(unsigned n);     // split=: voices 0..n-1 of each box on render core 1 (1..7)
unsigned split();
bool on() ;                     // a bench to admit against
// admit=report (console, the two-box load test): the costs, core_bp and fit stay, but no box is refused (every box its
// count) so the worst case can be measured against core_bp; admit=enforce (the default) refuses again
void set_enforce(bool on);
bool enforce();

// A load: voices[s] of slot s's box render (its 0..voices-1), held[s] its chains run.
struct Load {
  uint8_t voices[2];
  bool held[2];
};
// Render core 1 or 2's worst load for l, in bp at the clock now. Another core: 0.
unsigned core_bp(int core, const Load& l);
// The most voices (<= want) slot s may render beside the other slot's in l, so that every render core this box's voices land
// on stays within kLimitBp. A core already over without this box's voices (core 2 by the chains alone) takes none of them.
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
