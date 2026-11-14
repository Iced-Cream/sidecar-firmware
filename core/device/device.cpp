// core/device/device.cpp - see device.h. Per message, never per sample: cold.
#include "device/device.h"
#include "device/a4.h"
#include "device/digitakt.h"
#include "device/strip.h"

#include <string.h>

#include "fmt/fmt.h"
#include "fx/out.h"
#include "host_link.h"
#if CORE_POLY
#include "inst.h"
#endif
#include "keys.h"
#include "platform.h"

namespace core {
namespace device {

// The Rytm's table: what the rest of the engine and rytm-mods 0021 / 0026 assume. Its reports are the engine's own, so
// map / fixed / out_allow are never read for it.
static constexpr Profile kRytm = {
    kIdRytm, "Analog Rytm MKII", 0x1935, 0x0b48,
    12, 8, 16,
    {0x7D, 0x52, 0x54},
    0x04, 0x03, 0x11,
    kRemoteUi | kProject | kKits | kPatterns | kSendFx | kUserSlots | kSrcView | kFxIn | kPools,
    0, 0, {}, {}, {}, nullptr, nullptr, {},
    kBracketFull, &out::kRytmMk2, nullptr, false, false, 0, false, nullptr, "Rytm"};
static_assert(kRytm.voices == kBoxVoices, "the Rytm's voices are one box's");
static_assert(kRytm.params == kMaxParams, "the Rytm's reports carry P1..P16");

static constexpr int kMax = 8;
static const Profile* s_list[kMax] = {&kRytm, &kDigitakt, &kA4};
static int s_n = 3;
Link g_links[kLinks] = {{}, {}};
Link* g_cur = &g_links[0];
Link* g_base = &g_links[0];
int g_depth = 0;
volatile int8_t g_web_link = 0;
static volatile int8_t s_card_link = -1;   // the card core's request in hand: its link (CardScope)
static struct LinkIndex { LinkIndex() { for (int k = 0; k < kLinks; ++k) g_links[k].index = (uint8_t)k; } } s_link_index;

static inline bool on_control() { return !hooks.control_core || hooks.control_core(); }

int path_link() {
  if (on_control()) return g_cur->index;
  int c = __atomic_load_n(&s_card_link, __ATOMIC_RELAXED);
  return c >= 0 && c < kLinks ? c : g_web_link >= 0 && g_web_link < kLinks ? g_web_link : 0;
}

CardScope::CardScope(int link) : was(s_card_link), was_cur(g_cur) {
  if (link < 0 || link >= kLinks) link = 0;
  if (on_control()) ++g_depth, g_cur = &g_links[link];
  else __atomic_store_n(&s_card_link, (int8_t)link, __ATOMIC_RELAXED);
}
CardScope::~CardScope() {
  if (on_control()) g_cur = --g_depth ? was_cur : g_base;
  else __atomic_store_n(&s_card_link, was, __ATOMIC_RELAXED);
}
static volatile bool s_rytm_usb;          // core 0 writes (rytm_on_usb), the control core reads
static bool s_said;                       // a box's hello was taken this boot
// The Pi's own files, the same for every box: the network, the pairing, the screen, the black box, this choice.
static const char* const kMachine[] = {"wifi", "pair", "display", "prevboot", "blackbox", "device", "recspeed", "storbench", "wpa.conf"};

const Profile& rytm() { return kRytm; }
const Profile& active() { return cur().box ? *cur().box : kRytm; }
uint32_t dropped() { return cur().dropped; }
void rytm_on_usb(bool on) { s_rytm_usb = on; }

// g_base: slot 1 while a box has it and no Rytm is on USB, else slot 0 (device.h). The control core's; the web follows it.
static CORE_COLD void rebase() {
  Link* b = g_links[1].box && !s_rytm_usb ? &g_links[1] : &g_links[0];
  if (b == g_base) return;
  g_base = b, g_web_link = (int8_t)b->index;
  if (!g_depth) g_cur = b;
}

CORE_COLD const Profile* by_id(uint16_t id) {
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->id == id) return s_list[k];
  return nullptr;
}

// A box's stream is claimed when it enumerates, its hello or not: each slot has its own stream (pi/engine/ob_usb.h), so a
// stock A4 beside a Rytm no longer stands in the Rytm's way, and its hello always comes after enumeration now that no restart
// brings it back already chosen (audio_by_hello used to wait for that restart). Its MIDI still waits for the hello (hello_gate.h).
CORE_COLD const Profile* by_usb(uint16_t vid, uint16_t pid, bool) {
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->usb_vid == vid && s_list[k]->usb_pid == pid && s_list[k]->audio) return s_list[k];
  return nullptr;
}

void strip_tick(uint32_t us) {   // (one box with controls at a time: the first slot's that has them)
  for (const Link& l : g_links)
    if (l.box && l.box->controls) return strip::tick(*l.box->controls, us);
}

static inline bool is_sidecar(const uint8_t* d, int len) { return len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54; }

// The engine's output while a box is in use or a second link is up: to the link in hand (Scope), the box whose message this
// answers, else link 0. A box gets only what its out_allow lists; a Rytm everything.
static CORE_COLD void filtered_out(const uint8_t* m, int n) {
  int c = n > 4 ? m[4] & 0x7f : 0;
  Link& l = cur();
  if (!l.box) {
    if (l.out) l.out(m, n);
    else ++l.dropped;
  } else if (l.box->out_allow[c >> 5] >> (c & 31) & 1) {
    if (l.out) l.out(m, n);
  } else {
    ++l.dropped;
  }
}

// hooks.to_rytm from the links: link 0's own sender while it is a Rytm alone (the Rytm's path as it always was: no test, no
// filter), else filtered_out. Link 0's sender is the platform's to_rytm, taken the first time it is not already ours.
static CORE_COLD void rewire() {
  if (!g_links[0].out && hooks.to_rytm && hooks.to_rytm != filtered_out) g_links[0].out = hooks.to_rytm;
  bool alone = !g_links[0].box;
  for (int k = 1; k < kLinks; ++k) alone = alone && !g_links[k].box && !g_links[k].out;
  hooks.to_rytm = alone ? g_links[0].out : filtered_out;
}


CORE_COLD const char* state_path_box(const char* state, const char* path, char* buf, int cap) {
  if (strncmp(path, "/.rytm", 6) || (path[6] && path[6] != '/')) return path;
  if (path[6])
    for (const char* k : kMachine)
      if (!strncmp(path + 7, k, strlen(k))) return path;
  sniprintf(buf, cap, "%s%s", state, path + 6);
  return buf;
}

// Boot: nothing to choose. Slot 0 is the Rytm's, in /.rytm, from the first file on; a box takes slot 1 at its hello, in its own
// folder (docs/device-profiles.md "Several devices"). An older card's /.rytm/device (the last box, read here before two
// slots) is left as it is and never read.
CORE_COLD void boot() {}

CORE_COLD bool leaving() { return false; }   // (no change of folder waits any more: no restart)
CORE_COLD bool step(bool) {   // (each idle pass: a Rytm's USB id seen or gone moves the base)
  rebase();
  return false;
}

static uint32_t s_held[kBoxVoices][4];   // box_note's
static struct { uint8_t n, vb, notes[kChordNotes]; uint32_t at; } s_chord[kBoxVoices];   // chord_note's: the notes so far, the machine, the newest note's time
static CORE_COLD void select(Link& l, const Profile* p) {   // p null: the Rytm (slot 0), or no box (slot 1)
  if (p == l.box) return;
  memset(s_held, 0, sizeof s_held);   // the box before it: what it held is not the next one's
  memset(s_chord, 0, sizeof s_chord);
  if (!l.box) rewire();   // (link 0's sender taken while it is still the platform's)
  l.box = p;
  const char* was = l.state;
  l.state = p ? p->state_dir : nullptr;   // its own folder from its hello on: nothing restarts
  if (l.state && l.state != was) Fs().mkdir(l.state);
  rewire();
  rebase();
  out::set_trim(2, p ? p->trk10 : g_links[1].box ? g_links[1].box->trk10 : 0);   // a live stereo=trim:trk lasts until the box changes
  log(CORE_STR("device: slot %d: %s, state in %s"), l.index, p ? p->name : l.index ? "none" : kRytm.name, l.state ? l.state : "/.rytm");
  link_reset("device", l.index);   // the box before it: its notes and transport unknown (its slot's voices)
}

// Which slot each transport's messages are (in): transport 0 is the platform's host link (the Rytm's USB MIDI, or the
// emulator's TCP link), transport 1 the second Elektron box's USB MIDI (pi/engine/link1.h). A box's hello on either takes
// slot 1; a Rytm's 35 there gives its transport back to slot 0.
static int8_t s_route[kLinks] = {0, 1};
static void (*s_tx[kLinks])(const uint8_t*, int);   // each transport's sender: 0 the platform's to_rytm, 1 set_out(1)

static CORE_COLD void route(int t, int slot) {
  for (int k = 0; k < kLinks; ++k)
    if (k != t && s_route[k] == slot) s_route[k] = (int8_t)s_route[t];   // (the other transport: the slot left over)
  s_route[t] = (int8_t)slot;
  for (int k = 0; k < kLinks; ++k) g_links[s_route[k]].out = s_tx[k];
  rewire();
}

CORE_COLD void set_out(int link, void (*out)(const uint8_t*, int)) {
  if (link <= 0 || link >= kLinks) return;   // (transport 0's is the platform's to_rytm)
  rewire();   // transport 0's sender taken first, while hooks.to_rytm is still the platform's
  if (!s_tx[0]) s_tx[0] = g_links[0].out;
  s_tx[link] = out;
  Link& l = g_links[s_route[link]];
  l.out = out;
  if (!out && l.index != 0) select(l, nullptr);   // its box went: slot 1 free again
  rewire();
}

// The slot the bytes of transport t are for; a box's hello moves t to slot 1 (an answer kWait while another transport's box has it).
CORE_COLD int slot_of(int t, const uint8_t* d, int len) {
  if (t < 0 || t >= kLinks) t = 0;
  if (len < 6 || (d[4] != kCmdHello && d[4] != 0x35)) return s_route[t];   // (a Rytm's every other message: two compares)
  if (!s_tx[0] && hooks.to_rytm && hooks.to_rytm != filtered_out) s_tx[0] = hooks.to_rytm;
  if (!is_sidecar(d, len)) return s_route[t];
  if (d[4] == kCmdHello && len >= 10) {
    const Profile* p = by_id((uint16_t)(d[6] << 7 | d[7]));
    if (p && p != &kRytm && s_route[t] == 0) {
      int u = 1 - t;   // the other transport
      if (s_route[u] == 1 && g_links[1].box && g_links[1].box != p) return -1;   // slot 1 is another box's: kWait
      route(t, 1);
    }
  } else if (d[4] == 0x35 && s_route[t] == 1 && !(g_links[1].box && (g_links[1].box->features & kRemoteUi))) {
    select(g_links[1], nullptr);   // a Rytm on this transport now: slot 1 has no box
    route(t, 0);
  }
  return s_route[t];
}

CORE_COLD void reset() {
  g_cur = g_base = &g_links[0], g_depth = 0, s_card_link = -1, g_web_link = 0;
  for (Link& l : g_links) select(l, nullptr), l.state = nullptr;
  for (int k = 1; k < kLinks; ++k) g_links[k].out = nullptr;
  s_route[0] = 0, s_route[1] = 1, s_tx[0] = s_tx[1] = nullptr;
  rewire();                      // hooks.to_rytm back to link 0's sender, the platform's
  g_links[0].out = nullptr;      // (taken again from hooks.to_rytm: a test may set another)
  strip::reset();
  s_rytm_usb = s_said = false;
}

CORE_COLD bool add(const Profile* p) {
  if (!p || p->voices < 1 || p->voices > kBoxVoices || p->params > kMaxParams || s_n >= kMax) return false;
  for (int k = 0; k < kMaxParams; ++k)
    if (p->map[k] >= p->params) return false;
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->id == p->id) return false;
  s_list[s_n++] = p;
  return true;
}

void (*tx(int t))(const uint8_t*, int) { return t >= 0 && t < kLinks ? (t == 0 && !s_tx[0] ? hooks.to_rytm : s_tx[t]) : nullptr; }

CORE_COLD void hello(Link& l, const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1) return;
  int id = d[6] << 7 | d[7];
  const Profile* p = by_id((uint16_t)id);
  uint8_t st = !p ? kUnknown : d[8] != kProto ? kBadProto : kOk;
  const Profile* to = p == &kRytm ? nullptr : p;
  if (st == kOk && (to != nullptr) != (l.index != 0)) st = kUnknown;   // the Rytm's slot is 0, a box's 1 (slot_of routes it)
  if (st == kOk) select(l, to), s_said = to != nullptr;
  if (st != kOk) log(CORE_STR("device: hello from id %d proto %d: %s"), id, d[8], p ? "another protocol or slot" : "no profile");
  const uint8_t a[10] = {0xF0, 0x7D, 0x52, 0x54, kCmdHello, 1, d[6], d[7], st, 0xF7};
  void (*out)(const uint8_t*, int) = l.out ? l.out : l.index == 0 ? hooks.to_rytm : nullptr;   // the answer is never filtered
  if (out) out(a, (int)sizeof a);
}

// The table's translation: the box's message into the engine's 0x04 / 0x03 (P1..P16, the sound tag) or 0x11.
static CORE_COLD int table(const Profile& p, const uint8_t* d, int len, uint8_t* m) {
  if (len < 7 || d[1] != p.head[0] || d[2] != p.head[1] || d[3] != p.head[2]) return 0;
  int cmd = d[4], track = d[5];
  if (track >= p.voices) return 0;
  if (cmd == p.cmd_note_off) {
    if (len != 7) return 0;
    const uint8_t off[7] = {0xF0, 0x7D, 0x52, 0x54, 0x11, (uint8_t)track, 0xF7};
    memcpy(m, off, 7);
    return 7;
  }
  bool trig = cmd == p.cmd_trig;
  if (!trig && cmd != p.cmd_params) return 0;
  int from = trig ? 8 : 6;
  if (len != from + 2 * p.params + 1) return 0;
  int n = 0;
  m[n++] = 0xF0, m[n++] = 0x7D, m[n++] = 0x52, m[n++] = 0x54;
  m[n++] = trig ? 0x04 : 0x03;
  m[n++] = (uint8_t)(p.machine | track);
  if (trig) m[n++] = d[6], m[n++] = d[7];
  for (int k = 0; k < kMaxParams; ++k) {
    int s = p.map[k];
    m[n++] = s >= 0 ? d[from + 2 * s] : p.fixed[k];
    m[n++] = s >= 0 ? d[from + 2 * s + 1] : 0;
  }
  m[n++] = (p.sound_tag >> 7) & 0x7f;
  m[n++] = p.sound_tag & 0x7f;
  m[n++] = 0xF7;
  return n;
}

// What the sequencer sent the box: its echoes, the mod's report of each note it plays, a chord's as well; and what is held there, so that an all-off is an off for each
static struct { uint8_t note; bool on; uint32_t at; } s_sent[kBoxVoices][kChordNotes];
static uint8_t s_sent_w[kBoxVoices];

static CORE_COLD bool is_echo(int vi, uint8_t note) {
  uint32_t ms = now_ms();
  for (auto& s : s_sent[vi])
    if (s.on && s.note == note && ms - s.at < kEchoMs) return s.on = false, true;
  return false;
}

CORE_COLD void box_note(int ev, int note, int vel) {
  // ev: the engine voice; the box that owns it (its slot's, on 16 voices; the link in hand on 8) and its own voice vi
  Link& l = kVoiceSlots > 1 ? g_links[slot_of_voice(voice_wrap(ev))] : cur();
  const int vi = box_voice(ev);
  const Profile* p = l.box;
  if (!p || !p->note_out || ev < 0 || vi >= p->voices || note > 127) return;
  Scope sc(l);   // keys::forward_note sends to the link in hand
  const uint8_t off = (uint8_t)(0x80 | vi);
  if (note < 0) {   // inst::seq_off's -1: every note of the voice
    for (int n = 0; n < 128; ++n)
      if (s_held[vi][n >> 5] >> (n & 31) & 1) keys::forward_note(off, (uint8_t)n, 0);
    memset(s_held[vi], 0, sizeof s_held[vi]);
    return;
  }
  uint32_t bit = 1u << (note & 31);
  if (vel) {
    auto& s = s_sent[vi][s_sent_w[vi]++ % kChordNotes];
    s.note = (uint8_t)note, s.on = true, s.at = now_ms();
    s_held[vi][note >> 5] |= bit;
    keys::forward_note((uint8_t)(0x90 | vi), (uint8_t)note, (uint8_t)vel);
  } else {
    s_held[vi][note >> 5] &= ~bit;
    keys::forward_note(off, (uint8_t)note, 0);
  }
}

// A box's trig on a voice, with Profile::chords: true when it joined the voice's chord, so the message goes no further. The first
// note is the 0x04 it always was, on the voice's own machine: nothing waits, and a lone trig never comes here for more.
// vi: the engine voice (inst's); the chord state by the box's own voice k (box_voice)
static CORE_COLD bool chord_note(int vi, uint8_t vb, uint8_t note, uint8_t vel) {
#if CORE_POLY
  auto& c = s_chord[box_voice(vi)];
  uint32_t ms = now_ms();
  bool near = c.n && c.vb == vb && ms - c.at < kChordMs;
  if (near)
    for (int k = 0; k < c.n; ++k)
      if (c.notes[k] == note) return c.n > 1;   // a note of the chord twice is one; alone, a retrig as ever
  if (near && c.n < kChordNotes && inst::chord_on(vi, note, vel)) {
    c.notes[c.n++] = note, c.at = ms;
    return true;
  }
  if (near && inst::chord_single(vi)) return c.at = ms, true;   // one hit, as the box's own step: 04s this close are a flam
  if (c.n > 1) inst::chord_off(vi);   // a new note, no off before it: the amp's tail, as a mono retrig
  c.n = 1, c.vb = vb, c.notes[0] = note, c.at = ms;
#else
  (void)vi, (void)vb, (void)note, (void)vel;
#endif
  return false;
}

static CORE_COLD void chord_end(int vi) {
#if CORE_POLY
  if (s_chord[box_voice(vi)].n > 1) inst::chord_off(vi);
#endif
  s_chord[box_voice(vi)].n = 0;
}

CORE_COLD bool translate(Link& l, const uint8_t*& d, int& len, uint8_t* m) {
  if (len < 5 || d[len - 1] != 0xF7) return ++l.dropped, false;
  if (is_sidecar(d, len) && d[4] == kCmdHello) return hello(l, d, len), false;
  if (is_sidecar(d, len) && d[4] == 0x36 && (l.box->features & kRemoteUi) && len <= kMaxMessage) {   // a box's screen says hello: 35 is the Rytm's, and asks for /.rytm
    memcpy(m, d, (size_t)len);
    m[4] = 0x35;
    d = m;
    return true;
  }
  if (is_sidecar(d, len) && d[4] == 0x35) return ++l.dropped, false;   // (a Rytm's: slot_of gave its transport slot 0 first)
  if (is_sidecar(d, len) && d[4] == kCmdControls && l.box->controls) {
    const Controls& c = *l.box->controls;
    if (len != 7 + 2 * c.n || d[5] >= l.box->voices) return ++l.dropped, false;
    strip::report(c, d[5], d + 6);
    return false;
  }
  if (is_sidecar(d, len) && (l.box->in_allow[(d[4] & 0x7f) >> 5] >> (d[4] & 31) & 1)) {
    bool voiced = d[4] == 0x03 || d[4] == 0x04 || d[4] == 0x11;
    if (voiced && (d[5] & 7) >= l.box->voices) return ++l.dropped, false;
    if (d[4] == 0x04 && len > 7 && is_echo(d[5] & 7, d[6])) return false;
    if (l.box->chords && d[4] == 0x04 && len > 7 && chord_note(voice_in(d[5], l), d[5] & 0x78, d[6], d[7])) return false;
    if (l.box->chords && d[4] == 0x11) chord_end(voice_in(d[5], l));
    if (l.box->controls && d[4] == 0x04) strip::trig(d[5] & 7);
    if (l.box->controls && d[4] == 0x11) strip::off(d[5] & 7);
    return true;
  }
  int n = l.box->translate ? l.box->translate(*l.box, d, len, m) : table(*l.box, d, len, m);
  if (n <= 0) return ++l.dropped, false;
  d = m, len = n;
  return true;
}

}  // namespace device

CORE_COLD void engine_sysex_link(int t, const uint8_t* d, int len) {
  int slot = device::slot_of(t, d, len);
  if (slot < 0) {   // a box's hello while slot 1 is another box's: it waits
    const uint8_t a[10] = {0xF0, 0x7D, 0x52, 0x54, device::kCmdHello, 1, d[6], d[7], device::kWait, 0xF7};
    if (auto* out = device::tx(t)) out(a, (int)sizeof a);
    return;
  }
  device::Scope s(device::g_links[slot]);
  host_sysex_slot(d, len);
}

}  // namespace core
