// core/dsp/dsp.cpp - see dsp.h.
#include "dsp.h"

#include <string.h>

#include <new>

#include "../route/route.h"
#include "../platform.h"
#include "../samples/samples.h"
#include "blocks.h"
#include "clfo.h"
#include "gen.h"
#include "grain.h"
#include "wtosc.h"
#include "midiout.h"
#include "poly.h"
#include "sample.h"

namespace core {
namespace dsp {

namespace {
// ---- the library: one entry per primitive (blocks.h) ----
template <class T>
proc::Proc* make(void* mem, const void*) { return new (mem) T(); }

const ParamInfo kDelayP[] = {{"TIME", 11000}, {"FEEDBACK", 6000}, {"DAMP", 16383}, {"TONE", 0},
                             {"DRIVE", 0},    {"MODDEPTH", 0},   {"MODRATE", 6000}, {"SPREAD", 0}};
const ParamInfo kFilterP[] = {{"MODE", 0}, {"CUTOFF", 16383}, {"RESO", 2000}, {"DRIVE", 0}};
const ParamInfo kShaperP[] = {{"MODE", 0}, {"DRIVE", 0}, {"BITS", 16383}, {"RATE", 16383}, {"LEVEL", 8192}};
const ParamInfo kModP[] = {{"MODE", 0}, {"RATE", 9000}, {"DEPTH", 16383}, {"SHAPE", 0}, {"PHASE", 0}};
const ParamInfo kWidthP[] = {{"WIDTH", 8192}, {"BALANCE", 8192}, {"GAIN", 8192}};
const ParamInfo kSampleP[] = {{"LEV", 100 * 64}, {"SMP", 0}, {"TUN", 64 * 64}, {"STA", 0},
                              {"END", 16383},    {"LOOP", 0}, {"FIN", 64 * 64}, {"VEL", 0},
                              {"SLN", 0},        {"SLC", 0},       {"RTRG", 0},      {"ATK", 64 * 64},
                              {"DEC", 64 * 64},  {"SUS", 64 * 64}, {"REL", 64 * 64}, {"OUT", 0}};

const ParamInfo kOscP[] = {{"LEV", 100 * 64}, {"WAV", 85 * 64}, {"TUN", 64 * 64}, {"FIN", 64 * 64},
                           {"PW", 64 * 64},   {"FM", 0},       {"SUB", 0},       {"THRU", 0}};
const ParamInfo kNoiseP[] = {{"LEV", 100 * 64}, {"COL", 64 * 64}, {"THRU", 127 * 64}};
const ParamInfo kEnvP[] = {{"ATK", 0}, {"DEC", 64 * 64}, {"SUS", 0}, {"REL", 32 * 64}, {"MODE", 0}, {"VCA", 127 * 64}, {"VEL", 0}, {"SHAPE", 0}, {"OUT", 0}};
const ParamInfo kClfoP[] = {{"CYC", 0}, {"H1", 1 * 64}, {"H2", 2 * 64}, {"H3", 3 * 64}, {"PH1", 0}, {"PH2", 3390}, {"PH3", 1043}};
const ParamInfo kSvfP[] = {{"CUT", 64 * 64}, {"RES", 0}, {"KEY", 64 * 64}, {"DRV", 0}};

const ParamInfo kGrainP[] = {{"LEV", 100 * 64}, {"SMP", 0},       {"POS", 0},       {"SPRY", 0},
                             {"SIZE", 64 * 64}, {"DENS", 64 * 64}, {"PRND", 0},      {"REV", 0},
                             {"WIN", 0},        {"ATK", 0},        {"REL", 64 * 64}, {"HOLD", 0},
                             {"TUN", 64 * 64},  {"FIN", 64 * 64},  {"HOFS", 8192},   {"LOFS", 0}};

const ParamInfo kWtOscP[] = {{"LEV", 100 * 64}, {"TBL", 0},       {"POS", 0}, {"WARP", 0},  {"AMT", 0},  {"UNI", 0},
                             {"SUB", 0},        {"TUN", 64 * 64}, {"FIN", 64 * 64}, {"PM", 0}, {"THRU", 0}, {"POSM", 8192}, {"PCV", 8192}};

const ParamInfo kMidiOutP[] = {{"DST", 1 * 64}, {"CHN", 0},  {"LEN", 0},  {"TRN", 64 * 64}, {"PROG", 0}, {"PB", 0},
                               {"AT", 0},      {"MW", 0},   {"VAL1", 0}, {"SEL1", 0},      {"VAL2", 0}, {"SEL2", 0},
                               {"VAL3", 0},    {"SEL3", 0}, {"VAL4", 0}, {"SEL4", 0}};

// POLY's params: the copies' (16 b + p, named by the chain's blocks, INFO says "CHAIN"), then its own (poly.h)
struct PolyNames {
  ParamInfo p[Poly::kParamCount];
  PolyNames() {
    static const char* const own[] = {"CHD", "INV", "SPR", "VOI", "MOD1", "MOD2", "MOD3", "MOD4", "VCRV", "NLEV", "LLEV", "HKEY"};
    static const uint16_t def[] = {0, 0, 0, 0, 8192, 8192, 8192, 8192, 0, 100 * 64, 100 * 64, 0};
    for (int i = 0; i < Poly::kOwn; ++i) p[i] = {"CHAIN", 0};
    for (int i = 0; i < Poly::kParamCount - Poly::kOwn; ++i) p[Poly::kOwn + i] = {own[i], def[i]};
  }
};
const PolyNames kPolyNames;

const BlockInfo kLibrary[] = {
    {"DELAY", 8, kDelayP, sizeof(Delay), make<Delay>},
    {"FILTER", 4, kFilterP, sizeof(Filter), make<Filter>},
    {"SHAPER", 5, kShaperP, sizeof(Shaper), make<Shaper>},
    {"MOD", 5, kModP, sizeof(Modulator), make<Modulator>},
    {"WIDTH", 3, kWidthP, sizeof(Width), make<Width>},
    {"SAMPLE", 16, kSampleP, sizeof(Sample), make<Sample>},
    {"OSC", 8, kOscP, sizeof(Osc), make<Osc>},
    {"NOISE", 3, kNoiseP, sizeof(Noise), make<Noise>},
    {"ENV", 9, kEnvP, sizeof(Env), make<Env>},
    {"SVF", 4, kSvfP, sizeof(Svf), make<Svf>},
    {"CLFO", 7, kClfoP, sizeof(Clfo), make<Clfo>},
    {"GRAIN", 16, kGrainP, Grain::bytes(), make<Grain>, nullptr, -1, Grain::kShared},
    {"WTOSC", 13, kWtOscP, sizeof(WtOsc), make<WtOsc>},
    {"MIDIOUT", 16, kMidiOutP, sizeof(MidiOut), make<MidiOut>},
    {"POLY", Poly::kParamCount, kPolyNames.p, sizeof(Poly), nullptr},   // CHAIN only (a PLACE is refused)
};
constexpr int kLibSize = (int)(sizeof kLibrary / sizeof kLibrary[0]);
// the shipped blocks (core/code), in load order
const BlockInfo* s_ship[kShipped];
int s_nship;

// ---- the slots (control side) ----
struct Slot {
  State state;
  uint8_t owner;
  const BlockInfo* info;
  proc::Proc* p;
};
Slot s_slot[kSlots];
uint8_t* s_arena;
Stats s_st;

struct Answer { uint8_t n; uint8_t d[32]; };
constexpr int kAnswers = 16;
Answer s_ans[kAnswers];
int s_ans_rd, s_ans_wr;

void answer(const uint8_t* body, int n) {
  if ((s_ans_wr - s_ans_rd) >= kAnswers || n > 32 - 7) return;
  Answer& a = s_ans[s_ans_wr % kAnswers];
  static const uint8_t hdr[] = {0xF0, 0x7D, 0x52, 0x54, 0x51, 0x01};
  memcpy(a.d, hdr, 6);
  memcpy(a.d + 6, body, (size_t)n);
  a.d[6 + n] = 0xF7;
  a.n = (uint8_t)(7 + n);
  ++s_ans_wr;
}
bool refuse(int seq, int owner, int code) {
  const uint8_t b[] = {kRefused, (uint8_t)seq, (uint8_t)owner, (uint8_t)code};
  answer(b, 4);
  ++s_st.refused;
  s_st.last_refused = code;
  return false;
}
int count_used() {
  int n = 0;
  for (const Slot& s : s_slot) n += s.state != kEmpty;
  return n;
}

// PLACE / CHAIN's slot: want (0..15) if empty, 7F the first empty; -1 none free, -2 out of range
int free_slot(int want) {
  if (want == 0x7F) {
    for (int i = 0; i < kSlots; ++i) if (s_slot[i].state == kEmpty) return i;
    return -1;
  }
  if (want >= kSlots) return -2;
  return s_slot[want].state == kEmpty ? want : -1;
}

// FREE: the slot's points fade out first (route close_slot), so its block goes without a click
void start_free(int i) {
  Slot& s = s_slot[i];
  if (s.state != kPlacedState) return;
  route::close_slot(i);
  s.state = kFading;
  ++s_st.freed;
}

// TRIG's / PARAM's <first> <n> {<hi> <lo>} x n for slot s: checked whole first (false: nothing set), then queued
bool params_ok(const Slot& s, const uint8_t* p, int pn) {
  if (pn < 2 || pn != 2 + 2 * p[1] || p[1] < 1) return false;
  int top = s.info ? s.info->nparams : 0;
  return p[0] + p[1] <= (top < proc::kParams ? top : proc::kParams);
}
void params_set(int slot, const uint8_t* p) {
  for (int i = 0; i < p[1]; ++i) proc::set_param(slot, p[0] + i, p[2 + 2 * i] << 7 | p[3 + 2 * i]);
}

int text(uint8_t* b, const char* name) {
  int n = 0;
  while (name[n] && n < 16) { b[1 + n] = (uint8_t)(name[n] & 0x7F); ++n; }
  b[0] = (uint8_t)n;
  return 1 + n;
}
// WTOSC's built-in tables (upstream's core/wavetable/tables): 6 x 64 frames, generated once (11 MB, static)
float s_wt[wt::kBuiltins][wt::kBuiltinFrames][wt::kFrameFloats];
bool s_wt_built;
}  // namespace

int library_size() { return kLibSize + s_nship; }
const BlockInfo* library(int i) {
  if (i >= 0 && i < kLibSize) return &kLibrary[i];
  if (i >= kLibSize && i < kLibSize + s_nship) return s_ship[i - kLibSize];
  return nullptr;
}
bool builtin(const char* name) {
  for (const BlockInfo& b : kLibrary) if (!strcmp(b.name, name)) return true;
  return false;
}
const BlockInfo* find(const char* name) {
  for (const BlockInfo& b : kLibrary) if (!strcmp(b.name, name)) return &b;
  for (int i = 0; i < s_nship; ++i) if (!strcmp(s_ship[i]->name, name)) return s_ship[i];
  return nullptr;
}
bool add_block(const BlockInfo* b) {
  if (!b || !b->name || find(b->name) || s_nship >= kShipped || b->bytes > kSlotBytes) return false;
  s_ship[s_nship++] = b;
  return true;
}
void remove_block(const BlockInfo* b) {
  for (int i = 0; i < s_nship; ++i)
    if (s_ship[i] == b) {
      for (int k = i; k + 1 < s_nship; ++k) s_ship[k] = s_ship[k + 1];
      s_ship[--s_nship] = nullptr;
      return;
    }
}
int users(const BlockInfo* b) {
  int n = 0;
  for (const Slot& s : s_slot) n += s.state != kEmpty && s.info == b;
  return n;
}

bool begin(void* arena, size_t bytes) {
  for (const BlockInfo& b : kLibrary) if (b.bytes > kSlotBytes) return false;
  if (!arena || bytes < (size_t)kSlots * kSlotBytes || ((uintptr_t)arena & 63)) return false;
  s_arena = (uint8_t*)arena;
  wt_build();   // WTOSC's tables at boot (tens of ms), not at a PLACE mid-session
  grain_build();
  return true;
}

bool is_dsp_op(uint8_t op) { return (op >= kPlace && op <= kInfo) || op == kTrig || op == kParam || op == kChainOp; }

bool sysex(const uint8_t* m, int n) {
  if (n < 8 || m[0] != 0xF0 || m[4] != 0x51 || m[n - 1] != 0xF7 || !is_dsp_op(m[6])) { ++s_st.ops_bad; return false; }
  const uint8_t* b = m + 7;
  int bn = n - 8;
  for (int i = 0; i < bn; ++i) if (b[i] & 0x80) { ++s_st.ops_bad; return false; }
  if (m[5] != 1) {
    ++s_st.ops_bad;
    return (m[6] == kPlace || m[6] == kChainOp) && bn >= 2 ? refuse(b[0], b[1], kBad) : false;
  }
  switch (m[6]) {
    case kPlace: {   // <seq> <owner> <slot | 7F> <name 1..8>
      if (bn < 4 || bn > 3 + kNameMax) { ++s_st.ops_bad; return bn >= 2 ? refuse(b[0], b[1], kBad) : false; }
      int seq = b[0], owner = b[1], want = b[2];
      char name[kNameMax + 1];
      int k = 0;
      for (int i = 3; i < bn; ++i) name[k++] = (char)b[i];
      name[k] = 0;
      const BlockInfo* info = find(name);
      if (!info || (info->owner != -1 && info->owner != owner)) return refuse(seq, owner, kUnknown);
      if (!info->make) return refuse(seq, owner, kBad);   // POLY: CHAIN only
      if (!s_arena) return refuse(seq, owner, kFull);
      int slot = -1;
      if (want == 0x7F) {
        for (int i = 0; i < kSlots && slot < 0; ++i) if (s_slot[i].state == kEmpty) slot = i;
        if (slot < 0) return refuse(seq, owner, kFull);
      } else {
        if (want >= kSlots) return refuse(seq, owner, kBad);
        if (s_slot[want].state != kEmpty) return refuse(seq, owner, kFull);
        slot = want;
      }
      Slot& s = s_slot[slot];
      memset(s_arena + (size_t)slot * kSlotBytes, 0, info->bytes);   // a block's memory starts zeroed
      proc::Proc* p = info->make(s_arena + (size_t)slot * kSlotBytes, info->ctx);
      if (!p) return refuse(seq, owner, kBad);
      if (!proc::register_slot(slot, p)) { p->~Proc(); return refuse(seq, owner, kFull); }
      s = Slot{kPlacedState, (uint8_t)owner, info, p};
      // the params' bases for mod points (core/proc): the block's defaults, until a set
      for (int k = 0; k < info->nparams && k < proc::kParams; ++k) proc::set_default(slot, k, info->params ? info->params[k].def : 0);
      ++s_st.placed;
      s_st.used = count_used();
      const uint8_t a[] = {kPlaced, (uint8_t)seq, (uint8_t)owner, (uint8_t)slot};
      answer(a, 4);
      return true;
    }
    case kChainOp: {   // <seq> <owner> <slot | 7F> <notes> <weight> <nb> {<len> <name>} x nb <nm> {<src> <dst>} x nm
      if (bn < 6) { ++s_st.ops_bad; return bn >= 2 ? refuse(b[0], b[1], kBad) : false; }
      int seq = b[0], owner = b[1], want = b[2];
      Poly::Spec sp{};
      sp.notes = b[3], sp.weight = b[4], sp.nb = b[5];
      if (sp.notes < 1 || sp.notes > Poly::kNotes || sp.nb < 1 || sp.nb > Poly::kChain) return refuse(seq, owner, kBad);
      int i = 6;
      for (int k = 0; k < sp.nb; ++k) {
        if (i >= bn || b[i] < 1 || b[i] > kNameMax || i + 1 + b[i] > bn) return refuse(seq, owner, kBad);
        char name[kNameMax + 1];
        memcpy(name, b + i + 1, b[i]);
        name[b[i]] = 0;
        i += 1 + b[i];
        const BlockInfo* info = find(name);
        if (!info || (info->owner != -1 && info->owner != owner)) return refuse(seq, owner, kUnknown);
        if (!info->make) return refuse(seq, owner, kBad);   // POLY in POLY
        sp.blk[k] = info;
      }
      if (i >= bn) return refuse(seq, owner, kBad);
      sp.nm = b[i++];
      if (sp.nm > Poly::kMods || i + 2 * sp.nm != bn) return refuse(seq, owner, kBad);
      for (int k = 0; k < sp.nm; ++k, i += 2) {
        sp.mods[k][0] = b[i], sp.mods[k][1] = b[i + 1];
        if ((b[i] >> 4) >= sp.nb || (b[i] & 15) >= proc::kCtls || (b[i + 1] >> 4) >= sp.nb ||
            (b[i + 1] & 15) >= sp.blk[b[i + 1] >> 4]->nparams) return refuse(seq, owner, kBad);
      }
      if (sp.weight != Poly::kNoWeight && ((sp.weight >> 4) >= sp.nb || (sp.weight & 15) >= proc::kCtls)) return refuse(seq, owner, kBad);
      if (Poly::bytes(sp) > kSlotBytes) return refuse(seq, owner, kFull);
      if (!s_arena) return refuse(seq, owner, kFull);
      int slot = free_slot(want);
      if (slot < 0) return refuse(seq, owner, slot == -2 ? kBad : kFull);
      uint8_t* mem = s_arena + (size_t)slot * kSlotBytes;
      memset(mem, 0, Poly::bytes(sp));
      Poly* p = new (mem) Poly(sp, mem);
      if (!p->ok()) { p->~Poly(); return refuse(seq, owner, kBad); }
      if (!proc::register_slot(slot, p)) { p->~Poly(); return refuse(seq, owner, kFull); }
      const BlockInfo* pinfo = find("POLY");
      s_slot[slot] = Slot{kPlacedState, (uint8_t)owner, pinfo, p};
      for (int k = 0; k < sp.nb; ++k)   // the chain's blocks' defaults (each copy has them already), POLY's own
        for (int q = 0; q < sp.blk[k]->nparams && q < 16; ++q)
          proc::set_default(slot, 16 * k + q, sp.blk[k]->params ? sp.blk[k]->params[q].def : 0);
      for (int q = Poly::kOwn; q < Poly::kParamCount; ++q) proc::set_default(slot, q, pinfo->params[q].def);
      ++s_st.placed;
      s_st.used = count_used();
      const uint8_t a[] = {kPlaced, (uint8_t)seq, (uint8_t)owner, (uint8_t)slot};
      answer(a, 4);
      return true;
    }
    case kFree: {    // <owner> <slot | 7F>
      if (bn != 2) { ++s_st.ops_bad; return false; }
      bool any = false;
      for (int i = 0; i < kSlots; ++i) {
        const Slot& s = s_slot[i];
        if (s.state != kPlacedState || s.owner != b[0] || (b[1] != 0x7F && b[1] != i)) continue;
        start_free(i);
        any = true;
      }
      if (!any) ++s_st.ops_bad;
      poll();
      return any;
    }
    case kTrig: {    // <owner> <slot> <kind> <note> <vel> <blk4> [<first> <n> {<hi> <lo>} x n]
      if (bn < 9 || b[1] >= kSlots || b[2] > 1) { ++s_st.ops_bad; return false; }
      const Slot& s = s_slot[b[1]];
      if (s.state != kPlacedState || s.owner != b[0] || (bn > 9 && !params_ok(s, b + 9, bn - 9))) { ++s_st.ops_bad; return false; }
      if (bn > 9) params_set(b[1], b + 9);
      uint8_t vel = b[4] ? b[4] : 1;
      bool ok = b[2] == 0 ? proc::note(b[1], 0x90, b[3], vel) : proc::note(b[1], 0x80, b[3], 0);
      if (!ok) { ++s_st.ops_bad; return false; }
      ++s_st.trigs;
      s_st.last_blk = (uint32_t)b[5] << 21 | (uint32_t)b[6] << 14 | (uint32_t)b[7] << 7 | b[8];
      return true;
    }
    case kParam: {   // <owner> <slot> <first> <n> {<hi> <lo>} x n
      if (bn < 4 || b[1] >= kSlots) { ++s_st.ops_bad; return false; }
      const Slot& s = s_slot[b[1]];
      if (s.state != kPlacedState || s.owner != b[0] || !params_ok(s, b + 2, bn - 2)) { ++s_st.ops_bad; return false; }
      params_set(b[1], b + 2);
      ++s_st.params;
      return true;
    }
    case kInfo: {    // <seq> <what> <a> <b>
      if (bn != 4) { ++s_st.ops_bad; return false; }
      uint8_t a[24] = {kText, b[0], b[1], b[2], b[3]};
      int len = 5;
      const BlockInfo* info = library(b[2]);
      if (b[1] == 0) {
        len += text(a + len, info ? info->name : "");
        a[len++] = (uint8_t)(info ? info->nparams : 0);
      } else if (b[1] == 1) {
        const ParamInfo* p = info && b[3] < info->nparams ? &info->params[b[3]] : nullptr;
        len += text(a + len, p ? p->name : "");
        a[len++] = (uint8_t)(p ? p->def >> 7 : 0);
        a[len++] = (uint8_t)(p ? p->def & 0x7F : 0);
      } else {
        ++s_st.ops_bad;
        return false;
      }
      ++s_st.infos;
      answer(a, len);
      return true;
    }
  }
  return false;
}

int take_answer(uint8_t* out, int cap) {
  if (s_ans_rd == s_ans_wr) return 0;
  const Answer& a = s_ans[s_ans_rd % kAnswers];
  if (a.n > cap) return 0;
  memcpy(out, a.d, a.n);
  ++s_ans_rd;
  return a.n;
}

void poll() {
  wt_poll();
  for (int i = 0; i < kSlots; ++i) {
    Slot& s = s_slot[i];
    if (s.state == kFading && route::slot_quiet(i)) {
      proc::clear_slot(i);
      s.state = kReleasing;
    }
    if (s.state == kReleasing && proc::released(i)) {
      s.p->~Proc();
      s = Slot{};
      s_st.used = count_used();
    }
  }
}

void reset() {
  for (int i = 0; i < kSlots; ++i) start_free(i);
  poll();
}

SlotView slot(int i) {
  if (i < 0 || i >= kSlots) return SlotView{kEmpty, 0, ""};
  const Slot& s = s_slot[i];
  return SlotView{s.state, s.owner, s.info ? s.info->name : ""};
}
Stats stats() { return s_st; }


// GRAIN's built-in sources (upstream granular.cpp gen_builtin, as is) and windows
int16_t s_gr_src[2][kGrainBuiltinFrames];
float s_gr_win[kGrainWins][kGrainWin + 1];
bool s_gr_built;
const int16_t* grain_builtin(int b) { return s_gr_built && b >= 0 && b < 2 ? s_gr_src[b] : nullptr; }
const float* grain_window(int w) { return s_gr_win[w < 0 ? 0 : w >= kGrainWins ? kGrainWins - 1 : w]; }
void grain_build() {
  if (s_gr_built) return;
  static float d[kGrainBuiltinFrames];
  const float f0 = 261.6256f, sr = 48000.0f, tw = 6.2831853f;
  for (int b = 0; b < 2; ++b) {
    for (uint32_t i = 0; i < kGrainBuiltinFrames; ++i) d[i] = 0;
    if (b == 0) {   // PAD: two detuned band-limited saws, their brightness swelling and falling over the 4 s
      const int kH = 24;
      float xr[2][kH], xi[2][kH], cr[2][kH], ci[2][kH];
      for (int c = 0; c < 2; ++c)
        for (int h = 0; h < kH; ++h) {
          float w = tw * f0 * (c ? 1.004f : 0.996f) * (h + 1) / sr;
          xr[c][h] = 1, xi[c][h] = 0, cr[c][h] = cosf(w), ci[c][h] = sinf(w);
        }
      for (uint32_t i0 = 0; i0 < kGrainBuiltinFrames; i0 += 64) {
        float bright = 0.5f - 0.5f * cosf(tw * (float)i0 / sr / 4.0f);
        float a[kH];
        for (int h = 0; h < kH; ++h) a[h] = expf(-(float)h / (2.0f + 10.0f * bright)) / (h + 1);
        for (int c = 0; c < 2; ++c)
          for (int h = 0; h < kH; ++h) {
            float r = xr[c][h], q = xi[c][h];
            for (uint32_t i = i0; i < i0 + 64 && i < kGrainBuiltinFrames; ++i) {
              d[i] += a[h] * q;
              float nr = r * cr[c][h] - q * ci[c][h];
              q = r * ci[c][h] + q * cr[c][h], r = nr;
            }
            float m = 1.0f / sqrtf(r * r + q * q);
            xr[c][h] = r * m, xi[c][h] = q * m;
          }
      }
    } else {   // BELLS: eight strikes of a bell's inharmonic partials, each ringing out
      const float kRatio[5] = {1.0f, 2.756f, 5.404f, 8.933f, 13.34f}, kAmp[5] = {1.0f, 0.6f, 0.4f, 0.25f, 0.15f};
      for (uint32_t i = 0; i < kGrainBuiltinFrames; ++i) {
        float t = (float)i / sr, ts = fmodf(t, 0.5f);
        for (int k = 0; k < 5; ++k) d[i] += kAmp[k] * expf(-ts * (1.5f + 2.0f * k)) * sinf(tw * f0 * kRatio[k] * t);
      }
    }
    float pk = 0;
    for (uint32_t i = 0; i < kGrainBuiltinFrames; ++i) pk = fabsf(d[i]) > pk ? fabsf(d[i]) : pk;
    float g = pk > 0 ? 0.9f * 32767.0f / pk : 0.0f;
    for (uint32_t i = 0; i < kGrainBuiltinFrames; ++i) s_gr_src[b][i] = (int16_t)(d[i] * g);
  }
  for (int w = 0; w < kGrainWins; ++w)
    for (int i = 0; i <= kGrainWin; ++i) {
      float x = (float)i / kGrainWin, y;
      if (w == 0) y = 0.5f - 0.5f * cosf(6.2831853f * x);
      else if (w == 1) y = 1.0f - fabsf(2 * x - 1);
      else if (w == 2) y = x < 0.25f ? 0.5f - 0.5f * cosf(6.2831853f * 2 * x)
                         : x > 0.75f ? 0.5f - 0.5f * cosf(6.2831853f * 2 * (1 - x)) : 1.0f;
      else y = x < 0.05f ? 0.5f - 0.5f * cosf(3.14159265f * x / 0.05f) : expf(-5.0f * (x - 0.05f) / 0.95f);
      s_gr_win[w][i] = i == kGrainWin ? 0 : y;
    }
  s_gr_built = true;
}

const float* wt_table(int b) { return s_wt_built && b >= 0 && b < wt::kBuiltins ? &s_wt[b][0][0] : nullptr; }
void wt_build() {
  if (s_wt_built) return;
  static float c[wt::kSpec], scratch[4 * wt::kFrame];
  for (int b = 0; b < wt::kBuiltins; ++b) {
    float peak = 0.0f;
    for (int f = 0; f < wt::kBuiltinFrames; ++f) {
      wt::builtin_frame(b, f, c, scratch);
      wt::synth_frame(c, s_wt[b][f], scratch);
      float pk = wt::frame_peak(s_wt[b][f]);
      peak = pk > peak ? pk : peak;
    }
    float g = peak > 1e-6f ? 0.95f / peak : 1.0f;   // (as upstream: the table's loudest frame at 0.95)
    for (int f = 0; f < wt::kBuiltinFrames; ++f)
      for (int i = 0; i < wt::kFrameFloats; ++i) s_wt[b][f][i] *= g;
  }
  s_wt_built = true;
}

// ---- WTOSC's tables from the sample pool (upstream WAVETABLE's card slots: kWtSlots resident, least recently used
// replaced; a pool entry's frames as upstream's load: detect_frame_size with the 'clm ' hint, frame_count / source_frame,
// the first two channels halved, every frame's levels, the table's loudest frame at 0.95). The control core builds one
// frame a wt_poll (dsp::poll); the render side finds a table by one word (entry << 16 | frames, 0 none) and marks the
// entry wanted. A slot's memory is rewritten only kRetireMs after it was unpublished and only when its entry was not
// wanted for kKeepMs (a WTOSC asks again every block).
namespace {
constexpr int kWtSlots = 4;
constexpr uint32_t kWtKeepMs = 250, kWtRetireMs = 50, kWtWantMs = 1000;
constexpr int kWtMaxFs = 16384;   // a 'clm ' frame at most (detect_frame_size)
struct WtSlot {
  uint32_t pub;             // entry << 16 | frames; 0: none (render reads, acquire)
  int entry;                // control side: what it holds or is building (-1 none)
  const int16_t* pcm;       // the pool entry it was built from (a dropped / reloaded entry: rebuilt)
  float* data;              // wt::kMaxFrames x kFrameFloats, on first use
  uint32_t used_ms, retired_ms;
};
WtSlot s_wts[kWtSlots] = {{0, -1, nullptr, nullptr, 0, 0}, {0, -1, nullptr, nullptr, 0, 0},
                          {0, -1, nullptr, nullptr, 0, 0}, {0, -1, nullptr, nullptr, 0, 0}};
uint32_t s_wt_want[samples::kEntries];          // render: now_ms() | 1 when a WTOSC asked for the entry
const int16_t* s_wt_bad[samples::kEntries];     // an entry's pcm that gave no table (not tried again)
struct WtJob {
  int slot = -1, entry, fs, frames, frame;
  bool norm;
  float peak;
  samples::View v;
  float *x, *c, *scratch;
} s_wtj;
WtStats s_wtst;

bool wt_source(int e, samples::View* v) { return samples::get(e, v) && v->frames > 0 && v->ch >= 1; }
void wt_unpublish(WtSlot& s) {
  if (__atomic_load_n(&s.pub, __ATOMIC_RELAXED)) s.retired_ms = now_ms();
  __atomic_store_n(&s.pub, 0u, __ATOMIC_RELEASE);
}
}  // namespace

const float* wt_pool(int e, uint32_t* frames) {
  if (e < 0 || e >= samples::kEntries) return nullptr;
  __atomic_store_n(&s_wt_want[e], now_ms() | 1u, __ATOMIC_RELAXED);
  for (WtSlot& s : s_wts) {
    uint32_t p = __atomic_load_n(&s.pub, __ATOMIC_ACQUIRE);
    if (p && (int)(p >> 16) == e) {
      *frames = p & 0xFFFF;
      return s.data;
    }
  }
  return nullptr;
}

bool wt_poll() {
  const uint32_t now = now_ms();
  // the resident tables still the pool's: a dropped or reloaded entry's table goes
  for (int k = 0; k < kWtSlots; ++k) {
    WtSlot& s = s_wts[k];
    samples::View v;
    if (s.entry < 0 || k == s_wtj.slot) continue;
    if (!wt_source(s.entry, &v) || v.pcm != s.pcm) wt_unpublish(s), s.entry = -1;
    else if (now - (__atomic_load_n(&s_wt_want[s.entry], __ATOMIC_RELAXED) & ~1u) < kWtKeepMs) s.used_ms = now;
  }
  WtJob& j = s_wtj;
  if (j.slot >= 0) {
    WtSlot& s = s_wts[j.slot];
    samples::View v;
    if (!wt_source(j.entry, &v) || v.pcm != j.v.pcm) {   // its entry went while it was built
      s.entry = -1, j.slot = -1;
      return true;
    }
    if (now - s.retired_ms < kWtRetireMs) return true;   // a render call may still read the old table
    float* fr = s.data + (size_t)j.frame * wt::kFrameFloats;
    if (!j.norm) {
      uint32_t sf = wt::source_frame(v.frames, j.fs, j.frame);
      const int16_t* in = v.pcm + (size_t)sf * j.fs * v.ch;
      for (int i = 0; i < j.fs; ++i)
        j.x[i] = v.ch > 1 ? 0.5f * ((float)in[i * v.ch] * (1.0f / 32768.0f) + (float)in[i * v.ch + 1] * (1.0f / 32768.0f))
                          : (float)in[i] * (1.0f / 32768.0f);
      wt::analyze(j.x, j.fs, j.c, j.scratch);
      wt::synth_frame(j.c, fr, j.scratch);
      float pk = wt::frame_peak(fr);
      if (pk > j.peak) j.peak = pk;
      if (++j.frame >= j.frames) j.frame = 0, j.norm = true;
      return true;
    }
    float g = j.peak > 1e-6f ? 0.95f / j.peak : 1.0f;
    for (int i = 0; i < wt::kFrameFloats; ++i) fr[i] *= g;
    if (++j.frame < j.frames) return true;
    s.used_ms = now;
    __atomic_store_n(&s.pub, (uint32_t)j.entry << 16 | (uint32_t)j.frames, __ATOMIC_RELEASE);
    ++s_wtst.loads;
    j.slot = -1;
    return true;
  }
  // a wanted entry not resident: into a free slot, else the least recently used one not wanted now
  for (int e = 0; e < samples::kEntries; ++e) {
    uint32_t w = __atomic_load_n(&s_wt_want[e], __ATOMIC_RELAXED);
    if (!w || now - (w & ~1u) > kWtWantMs) continue;
    bool have = false;
    for (const WtSlot& s : s_wts) have |= s.entry == e;
    samples::View v;
    if (have || !wt_source(e, &v) || s_wt_bad[e] == v.pcm) continue;
    int fs = wt::detect_frame_size(v.frames, v.clm), nf = fs <= kWtMaxFs ? wt::frame_count(v.frames, fs) : 0;
    if (nf < 1) {
      s_wt_bad[e] = v.pcm, ++s_wtst.fails;
      continue;
    }
    int b = -1;
    for (int k = 0; k < kWtSlots; ++k) {
      const WtSlot& s = s_wts[k];
      if (s.entry >= 0 && now - (__atomic_load_n(&s_wt_want[s.entry], __ATOMIC_RELAXED) & ~1u) < kWtKeepMs) continue;
      if (b < 0 || (s.entry < 0 && s_wts[b].entry >= 0) ||
          ((s.entry >= 0) == (s_wts[b].entry >= 0) && (int32_t)(s.used_ms - s_wts[b].used_ms) < 0))
        b = k;
    }
    if (b < 0) return false;   // every slot in use: it waits
    WtSlot& s = s_wts[b];
    // plain new as the rest of core (the Pi's kernel has no std::nothrow: core/store/store.cpp); on the Pi an
    // allocation past the heap's reserve halts before new returns 0, so the check below is the host's
    if (!s.data) s.data = new float[(size_t)wt::kMaxFrames * wt::kFrameFloats];
    if (!j.x) {
      j.x = new float[kWtMaxFs];
      j.c = new float[wt::kSpec];
      j.scratch = new float[4 * kWtMaxFs];
    }
    if (!s.data || !j.x || !j.c || !j.scratch) return false;
    wt_unpublish(s);
    s.entry = e, s.pcm = v.pcm;
    j.slot = b, j.entry = e, j.fs = fs, j.frames = nf, j.frame = 0, j.norm = false, j.peak = 0.0f, j.v = v;
    return true;
  }
  return false;
}

WtStats wt_stats() {
  WtStats r = s_wtst;
  r.resident = 0;
  for (const WtSlot& s : s_wts) r.resident += __atomic_load_n(&s.pub, __ATOMIC_RELAXED) != 0;
  r.building = s_wtj.slot >= 0;
  return r;
}

}  // namespace dsp
}  // namespace core
