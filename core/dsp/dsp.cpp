// core/dsp/dsp.cpp - see dsp.h.
#include "dsp.h"

#include <string.h>

#include <new>

#include "../route/route.h"
#include "blocks.h"
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
                              {"SLN", 0},        {"SLC", 0}};

const ParamInfo kOscP[] = {{"LEV", 100 * 64}, {"WAV", 85 * 64}, {"TUN", 64 * 64}, {"FIN", 64 * 64},
                           {"PW", 64 * 64},   {"FM", 0},       {"SUB", 0},       {"THRU", 0}};
const ParamInfo kNoiseP[] = {{"LEV", 100 * 64}, {"COL", 64 * 64}, {"THRU", 127 * 64}};
const ParamInfo kEnvP[] = {{"ATK", 0}, {"DEC", 64 * 64}, {"SUS", 0}, {"REL", 32 * 64}, {"MODE", 0}, {"VCA", 127 * 64}, {"VEL", 0}};

const ParamInfo kGrainP[] = {{"LEV", 100 * 64}, {"SMP", 0},       {"POS", 0},       {"SPRY", 0},
                             {"SIZE", 64 * 64}, {"DENS", 64 * 64}, {"PRND", 0},      {"REV", 0},
                             {"WIN", 0},        {"HOLD", 0},       {"TUN", 64 * 64}, {"FIN", 64 * 64}};

const ParamInfo kWtOscP[] = {{"LEV", 100 * 64}, {"TBL", 0},   {"POS", 0},   {"TUN", 64 * 64},
                             {"FIN", 64 * 64},  {"FM", 0},    {"SUB", 0},   {"THRU", 0}};

const ParamInfo kMidiOutP[] = {{"DST", 1 * 64}, {"CHN", 0},  {"LEN", 0},  {"TRN", 64 * 64}, {"PROG", 0}, {"PB", 0},
                               {"AT", 0},      {"MW", 0},   {"VAL1", 0}, {"SEL1", 0},      {"VAL2", 0}, {"SEL2", 0},
                               {"VAL3", 0},    {"SEL3", 0}, {"VAL4", 0}, {"SEL4", 0}};

// POLY's params: the copies' (16 b + p, named by the chain's blocks, INFO says "CHAIN"), then its own (poly.h)
struct PolyNames {
  ParamInfo p[Poly::kParamCount];
  PolyNames() {
    static const char* const own[] = {"CHD", "INV", "SPR", "VOI", "MOD1", "MOD2", "MOD3", "MOD4"};
    for (int i = 0; i < Poly::kOwn; ++i) p[i] = {"CHAIN", 0};
    for (int i = 0; i < Poly::kParamCount - Poly::kOwn; ++i) p[Poly::kOwn + i] = {own[i], (uint16_t)(i < 4 ? 0 : 8192)};
  }
};
const PolyNames kPolyNames;

const BlockInfo kLibrary[] = {
    {"DELAY", 8, kDelayP, sizeof(Delay), make<Delay>},
    {"FILTER", 4, kFilterP, sizeof(Filter), make<Filter>},
    {"SHAPER", 5, kShaperP, sizeof(Shaper), make<Shaper>},
    {"MOD", 5, kModP, sizeof(Modulator), make<Modulator>},
    {"WIDTH", 3, kWidthP, sizeof(Width), make<Width>},
    {"SAMPLE", 10, kSampleP, sizeof(Sample), make<Sample>},
    {"OSC", 8, kOscP, sizeof(Osc), make<Osc>},
    {"NOISE", 3, kNoiseP, sizeof(Noise), make<Noise>},
    {"ENV", 7, kEnvP, sizeof(Env), make<Env>},
    {"GRAIN", 12, kGrainP, sizeof(Grain), make<Grain>},
    {"WTOSC", 8, kWtOscP, sizeof(WtOsc), make<WtOsc>},
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

}  // namespace dsp
}  // namespace core
