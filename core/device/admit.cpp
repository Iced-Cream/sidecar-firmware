// core/device/admit.cpp - see admit.h. Per attach, never per sample: cold.
#include "device/admit.h"

#include <string.h>
#include <strings.h>

#include "platform.h"

namespace core {
namespace admit {

static Costs s_costs = {};
static unsigned s_clock = 0;   // 0: the bench's own
static unsigned s_split = 4;
// voices= per box type: the Rytm, the A4, the Digitakt (-1: its profile's count)
static const char* const kTypes[] = {"rytm", "a4", "digitakt"};
static constexpr int kNTypes = 3;
static int8_t s_over[kNTypes] = {-1, -1, -1};

void set_costs(const Costs& c) { s_costs = c; }
const Costs& costs() { return s_costs; }
void set_clock(unsigned mhz) { s_clock = mhz; }
unsigned clock() { return s_clock ? s_clock : s_costs.mhz; }
void set_split(unsigned n) { s_split = n < 1 ? 1 : n > 7 ? 7 : n; }
unsigned split() { return s_split; }
bool on() { return s_costs.mhz != 0; }
static bool s_enforce = true;
void set_enforce(bool on) { s_enforce = on; }
bool enforce() { return s_enforce; }

static unsigned scaled(unsigned long long bp) {   // the bench's bp at the clock now
  const unsigned now = clock();
  return now ? (unsigned)((bp * s_costs.mhz + now - 1) / now) : (unsigned)bp;
}

unsigned split_for(unsigned asked) {
  if (!asked) return s_split;
  const unsigned half = (asked + 1) / 2;
  return half < s_split ? half : s_split;
}

// box voices 0..n-1 at split sp: how many of them render on core c
static unsigned on_core(int c, unsigned n, unsigned sp) {
  const unsigned one = n < sp ? n : sp;
  return c == 1 ? one : n - one;
}

CORE_COLD unsigned core_bp(int c, const Load& l) {
  if (c != 1 && c != 2) return 0;
  const int base = l.held[0] ? 0 : 1;   // the base box: AMP plays on its voices' and master chains only
  unsigned v = 0, amps = 0;
  unsigned long long bp = 0;
  for (int s = 0; s < 2; ++s) {
    const unsigned n = on_core(c, l.voices[s] > 8 ? 8 : l.voices[s], split_for(l.asked[s]));
    v += n;
    if (s == base) amps = n < (unsigned)kAmpCap ? n : (unsigned)kAmpCap;
    if (l.held[s] && chain_core(s) == c) {   // its chains, on the core that runs them; the other box's without the AMP pair
      const unsigned pair = 2 * s_costs.amp_bp;
      bp += s == base ? s_costs.fx_bp : s_costs.fx_bp > pair ? s_costs.fx_bp - pair : 0;
    }
  }
  bp += (unsigned long long)v * (s_costs.voice_bp + s_costs.ins_bp) + (unsigned long long)amps * s_costs.amp_bp;
  return scaled(bp);
}

CORE_COLD int fit(int s, int want, const Load& l) {
  if (want < 0) want = 0;
  if (want > 8) want = 8;
  if (!on() || s < 0 || s > 1) return want;
  Load t = l;
  t.held[s] = true;
  t.voices[s] = 0;
  t.asked[s] = (uint8_t)want;
  const unsigned sp = split_for((unsigned)want);
  const unsigned base[3] = {0, core_bp(1, t), core_bp(2, t)};   // without its voices (its chains counted)
  for (int n = want; n > 0; --n) {
    t.voices[s] = (uint8_t)n;
    bool ok = true;
    for (int c = 1; c <= 2 && ok; ++c)
      if (on_core(c, (unsigned)n, sp) && core_bp(c, t) > kLimitBp && core_bp(c, t) > base[c]) ok = false;
    if (ok) return n;
  }
  return 0;
}

CORE_COLD int admit(int s, int want, const Load& l) {
  if (s_enforce) return fit(s, want, l);
  return want < 0 ? 0 : want > 8 ? 8 : want;
}

static int type_index(const char* name, size_t len) {
  if (len == 2 && !strncasecmp(name, "dt", 2)) return 2;
  for (int k = 0; k < kNTypes; ++k)
    if (strlen(kTypes[k]) == len && !strncasecmp(name, kTypes[k], len)) return k;
  return -1;
}

CORE_COLD bool parse_override(const char* text) {
  if (!text) return false;
  int8_t next[kNTypes];
  memcpy(next, s_over, sizeof next);
  const char* p = text;
  while (*p) {
    const char* colon = strchr(p, ':');
    if (!colon) return false;
    const int k = type_index(p, (size_t)(colon - p));
    if (k < 0) return false;
    const char* end = colon + 1;   // (no strtol on the Pi)
    long n = 0;
    while (*end >= '0' && *end <= '9' && n <= 8) n = n * 10 + (*end++ - '0');
    if (end == colon + 1 || n > 8 || (*end && *end != ',')) return false;
    next[k] = (int8_t)n;
    p = *end ? end + 1 : end;
  }
  memcpy(s_over, next, sizeof s_over);
  return true;
}

int override_for(const char* type_short) {
  if (!type_short) return -1;
  const int k = type_index(type_short, strlen(type_short));
  return k < 0 ? -1 : s_over[k];
}

void reset() {
  s_costs = {};
  s_clock = 0;
  s_split = 4;
  s_enforce = true;
  for (int8_t& o : s_over) o = -1;
}

}  // namespace admit
}  // namespace core
