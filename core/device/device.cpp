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

static void select(Link& l, const Profile* h);
static void usb_take();
static constexpr int kMax = 8;
static const Profile* s_list[kMax] = {&kRytm, &kDigitakt, &kA4};
static int s_n = 3;
Link g_links[kLinks] = {{}, {}};   // (slot 0 presumed the Rytm's: s_presume below)
Link* g_cur = &g_links[0];
Link* g_base = &g_links[0];
int g_depth = 0;
uint32_t g_hold_ms = 0;
volatile int8_t g_web_link = 0;
static volatile int8_t s_card_link = -1;   // the card core's request in hand: its link (CardScope)
static struct LinkIndex {
  LinkIndex() {
    for (int k = 0; k < kLinks; ++k) g_links[k].index = (uint8_t)k;
    g_links[0].held = g_links[0].was = &kRytm, g_links[0].presumed = true;   // boot: slot 0 the Rytm's until a box says otherwise
  }
} s_link_index;

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

const Profile& rytm() { return kRytm; }
const Profile& active() { return cur().box ? *cur().box : kRytm; }
uint32_t dropped() { return cur().dropped; }
bool rytm_at(int s) { return kVoiceSlots < 2 || (s >= 0 && s < kLinks && g_links[s].held == &kRytm); }
int rytm_slot() { return kVoiceSlots < 2 ? 0 : slot_held_by(&kRytm); }
CORE_COLD int slot_held_by(const Profile* p) {
  if (!p) p = &kRytm;
  for (int k = 0; k < kLinks; ++k)
    if (g_links[k].held == p) return k;
  return -1;
}

// g_base: slot 0 while it is held, else the first slot that is (device.h). The control core's; the web follows it.
static CORE_COLD void rebase() {
  Link* b = &g_links[0];
  for (Link& l : g_links)
    if (l.held) { b = &l; break; }
  if (b == g_base) return;
  g_base = b, g_web_link = (int8_t)b->index;
  if (!g_depth) g_cur = b;
  out::g_base_slot = (int8_t)slot_of_voice(voice_base(*b));   // the base box's settings: its stage, the console's trim, AMP
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

// hooks.to_rytm from the links: the Rytm's own sender while she is the one slot held and nothing else is linked (the Rytm's
// path as it always was: no test, no filter), else filtered_out. Transport 0's sender is the platform's to_rytm, taken the
// first time it is not already ours, and given to the slot transport 0 goes to.
static int8_t s_route[kLinks] = {0, -1};
static void (*s_tx[kLinks])(const uint8_t*, int);   // each transport's sender: 0 the platform's to_rytm, 1 set_out(1)
static CORE_COLD void rewire() {
  if (!s_tx[0] && hooks.to_rytm && hooks.to_rytm != filtered_out) s_tx[0] = hooks.to_rytm;
  if (s_route[0] >= 0 && !g_links[s_route[0]].out) g_links[s_route[0]].out = s_tx[0];
  const Link* only = nullptr;
  int n = 0;
  for (const Link& l : g_links)
    if (l.held || l.out) ++n, only = &l;
  hooks.to_rytm = n == 0 ? s_tx[0] : n == 1 && !only->box && only->out ? only->out : filtered_out;
}


CORE_COLD const char* state_path_box(const char* state, const char* path, char* buf, int cap) {
  // only /.rytm's paths are a box's: the Pi's own files are in /.sidecar (core/device/sidecar_dir.h), the libraries shared
  if (strncmp(path, "/.rytm", 6) || (path[6] && path[6] != '/')) return path;
  sniprintf(buf, cap, "%s%s", state, path + 6);
  return buf;
}

// Boot: nothing to choose. Slot 0 is presumed the Rytm's, in /.rytm, from the first file on; boxes take slots by attach order
// (docs/device-profiles.md "Several devices"). An older card's /.rytm/device (the last box, read here before two slots) is
// left as it is and never read.
CORE_COLD void boot() {}

CORE_COLD bool leaving() {   // a box left and its edits are not all saved yet
  for (const Link& l : g_links)
    if (l.parting) return true;
  return false;
}
CORE_COLD bool step(bool settled) {   // (each idle pass: once every edit is saved, a slot whose box left is free for any box)
  usb_take();   // the USB devices that came or went (core 0's)
  if (settled)
    for (Link& l : g_links)
      if (l.parting) {
        l.parting = false, l.holding = g_hold_ms > 0;
        if (l.holding) log(CORE_STR("device: slot %d saved, kept %u ms for its box"), l.index, (unsigned)g_hold_ms);
        else log(CORE_STR("device: slot %d saved, free"), l.index);
      }
  for (Link& l : g_links)
    if (l.holding && !l.parting && now_ms() - l.left_ms >= g_hold_ms) l.holding = false, log(CORE_STR("device: slot %d free"), l.index);
  rebase();
  return false;
}

static uint32_t s_held[kBoxVoices][4];   // box_note's
static struct { uint8_t n, vb, notes[kChordNotes]; uint32_t at; } s_chord[kBoxVoices];   // chord_note's: the notes so far, the machine, the newest note's time
// Slot l's holder now (h: a profile, rytm() the Rytm; null: free). Its box is h unless h is the Rytm. A holder takes its folder
// (its type's; the Rytm's /.rytm). A slot freed keeps its folder until its next holder: a write queued for the box that left (a
// card job carries its slot, not the folder) still lands in that box's own folder.
static CORE_COLD void select(Link& l, const Profile* h) {
  const Profile* p = h == &kRytm ? nullptr : h;
  if (h == l.held && p == l.box) return;
  memset(s_held, 0, sizeof s_held);   // the box before it: what it held is not the next one's
  memset(s_chord, 0, sizeof s_chord);
  if (!l.box) rewire();   // (link 0's sender taken while it is still the platform's)
  __atomic_store_n(&l.held, h, __ATOMIC_RELAXED);
  __atomic_store_n(&l.box, p, __ATOMIC_RELAXED);
  l.presumed = false;
  const char* was = l.state;
  if (h) l.state = p ? p->state_dir : nullptr, l.was = h, l.parting = l.holding = false;
  if (l.state && l.state != was) Fs().mkdir(l.state);
  rewire();
  rebase();
  // the slot's TRK trim, its box's (the Rytm's: 0). On 8 voices every link plays slot 0's settings: the holder's there
  if (h || kVoiceSlots > 1) out::control_of(slot_of_voice(voice_base(l))).trk10 = p ? p->trk10 : 0;
  log(CORE_STR("device: slot %d: %s, state in %s"), l.index, h ? h->name : "free", l.state ? l.state : "/.rytm");
  link_reset("device", l.index);   // the box before it: its notes and transport unknown (its slot's voices)
}

// Which slot each transport's messages are (in), -1 none: transport 0 is the platform's host link (the USB-C box's MIDI, or the
// emulator's TCP link), transport 1 the other box's (pi/engine/link1.h, the Teensy relay). Transport 0 goes to the presumed
// Rytm's slot 0 from boot; a hello on either claims its box's slot (attach order), a Rytm's first message the Rytm's.
static CORE_COLD void route(int t, int slot) {
  for (int k = 0; k < kLinks; ++k)
    if (k != t && s_route[k] == slot) s_route[k] = -1;   // (a box speaks on one transport: the other one's old route goes)
  s_route[t] = (int8_t)slot;
  for (Link& l : g_links) l.out = nullptr;
  for (int k = 0; k < kLinks; ++k)
    if (s_route[k] >= 0) g_links[s_route[k]].out = s_tx[k];
  rewire();
}

// usb_attach's reservations (core 0 writes them, the control core takes them in step / claim) and usb_detach's
static const Profile* volatile s_usb_want[kLinks];
static volatile bool s_usb_gone[kLinks];
static inline bool kept(const Link& l) { return l.parting || l.holding; }   // (its last holder's still: saving, or held for it)
static CORE_COLD bool open_for(const Link& l, const Profile* p) {   // a slot p may take now (no other's reservation)
  const Profile* w = __atomic_load_n(&s_usb_want[l.index], __ATOMIC_ACQUIRE);
  const Profile* h = __atomic_load_n(&l.held, __ATOMIC_RELAXED);
  return (!w || w == p) && (!h || (l.presumed && p != &kRytm)) && (!kept(l) || l.was == p);
}

CORE_COLD int usb_attach(const Profile* p) {
  if (!p) p = &kRytm;
  for (const Link& l : g_links) {
    if (__atomic_load_n(&s_usb_want[l.index], __ATOMIC_ACQUIRE) == p) return l.index;
    if (__atomic_load_n(&l.held, __ATOMIC_RELAXED) != p) continue;
    // its slot already (the Rytm's presumed at boot): reserved for it all the same, so usb_take confirms it (a stock Rytm
    // says no 35: her USB is her hello) and routes her MIDI
    const Profile* none = nullptr;
    __atomic_compare_exchange_n(&s_usb_want[l.index], &none, p, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return l.index;
  }
  // its own slot first while that is still saving (a box that dropped off the bus a moment and came back: it stays where it
  // was, the RA1 re-enumeration), then the lowest free one
  for (int own = 1; own >= 0; --own)
    for (const Link& l : g_links) {
      if (own && !(kept(l) && l.was == p)) continue;
      if (!open_for(l, p)) continue;
      const Profile* none = nullptr;
      if (__atomic_compare_exchange_n(&s_usb_want[l.index], &none, p, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return l.index;
    }
  return -1;
}
void usb_detach(int s) {
  if (s >= 0 && s < kLinks) __atomic_store_n(&s_usb_gone[s], true, __ATOMIC_RELEASE);
}
static CORE_COLD void usb_take() {   // (the control core: step, claim)
  for (Link& l : g_links) {
    if (__atomic_exchange_n(&s_usb_gone[l.index], false, __ATOMIC_ACQ_REL)) {
      __atomic_store_n(&s_usb_want[l.index], (const Profile*)nullptr, __ATOMIC_RELEASE);
      release(l.index);
    }
    const Profile* w = __atomic_load_n(&s_usb_want[l.index], __ATOMIC_ACQUIRE);
    if (!w) continue;
    if (l.held != w) {
      if (l.held && l.presumed) release(l.index);   // (the presumed Rytm never spoke: the box's USB came first)
      if (!l.held) select(l, w), log(CORE_STR("device: %s attached (USB): slot %d"), w->name, l.index);
    }
    if (w == &kRytm && l.held == w && s_route[0] < 0) route(0, l.index);   // (her MIDI on the host link: a stock Rytm says no 35)
    l.presumed = false;
    __atomic_store_n(&s_usb_want[l.index], (const Profile*)nullptr, __ATOMIC_RELEASE);
  }
}

CORE_COLD int claim(const Profile* p) {
  if (!p) p = &kRytm;
  usb_take();
  const int had = slot_held_by(p);
  if (had >= 0) return g_links[had].presumed = false, had;
  // the presumed Rytm never spoke: the first box to say hello is the first attached, slot 0 is its
  for (Link& l : g_links)
    if (l.presumed && p != &kRytm) release(l.index);
  // the lowest free slot; one whose box left and is still saving only for that same box (its own folder); none another
  // box's USB has just reserved
  for (int own = 1; own >= 0; --own)   // (its own slot first while that is still saving: usb_attach's)
  for (Link& l : g_links)
    if (!l.held && (!own || (kept(l) && l.was == p)) && open_for(l, p)) {
      select(l, p);
      log(CORE_STR("device: %s attached: slot %d"), p->name, l.index);
      return l.index;
    }
  log(CORE_STR("device: %s: no slot free"), p->name);
  return -1;
}

CORE_COLD void release(int s) {
  if (s < 0 || s >= kLinks || !g_links[s].held) return;
  Link& l = g_links[s];
  const Profile* h = l.held;
  const bool quiet = l.presumed;   // (a Rytm presumed at boot never spoke: nothing of hers to save beyond what idle_save does anyway)
  slot_left(s);   // its transport, sequencer, arpeggiator and MIDI stop, in its Scope (host_link.h)
  select(l, nullptr);   // its voices silenced (link_reset); its folder kept for its saves
  l.parting = !quiet, l.holding = false, l.left_ms = now_ms();
  for (int t = 0; t < kLinks; ++t)
    if (s_route[t] == s) s_route[t] = -1;
  route(0, s_route[0]);   // (the senders again)
  log(CORE_STR("device: %s left slot %d%s"), h->name, s, quiet ? "" : ": its edits saved into its folder");
}

void rytm_on_usb(bool on) {
  if (on) {
    const int s = claim(&kRytm);
    if (s >= 0 && s_route[0] < 0) route(0, s);   // her MIDI on the host link (a stock Rytm says no 35), unless a box has it
  }
  else release(slot_held_by(&kRytm));   // (her link: rytm_slot() is 0 on one slot)
}

CORE_COLD void set_out(int link, void (*out)(const uint8_t*, int)) {
  if (link <= 0 || link >= kLinks) return;   // (transport 0's is the platform's to_rytm)
  rewire();   // transport 0's sender taken first, while hooks.to_rytm is still the platform's
  s_tx[link] = out;
  if (s_route[link] >= 0) g_links[s_route[link]].out = out;
  if (!out && s_route[link] >= 0) release(s_route[link]);   // its box went: its slot free again (the other box stays)
  rewire();
}

// The slot the bytes of transport t are for, -1 none.
int route_of(int t) { return t >= 0 && t < kLinks ? s_route[t] : -1; }

// A hello's profile when the box may have a slot (known, this protocol); null: its answer is kUnknown / kBadProto
static CORE_COLD const Profile* hello_profile(const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1 || d[8] != kProto) return nullptr;
  return by_id((uint16_t)(d[6] << 7 | d[7]));
}

CORE_COLD int slot_of(int t, const uint8_t* d, int len) {
  if (t < 0 || t >= kLinks) t = 0;
  const int r = s_route[t];
  if (!d || len < 6) return r;
  if (r >= 0 && d[4] != kCmdHello && d[4] != 0x35) {   // (every other message of a slot's box: two compares, and the presumed test)
    if (g_links[r].presumed && is_sidecar(d, len)) g_links[r].presumed = false;   // the presumed Rytm spoke: slot 0 is hers
    return r;
  }
  if (!s_tx[0] && hooks.to_rytm && hooks.to_rytm != filtered_out) s_tx[0] = hooks.to_rytm;
  if (!is_sidecar(d, len)) return r;
  if (d[4] == kCmdHello) {
    const Profile* p = hello_profile(d, len);
    if (!p) return -1;   // (engine_sysex_link answers kUnknown / kBadProto)
    if (r >= 0 && g_links[r].held == p) return r;   // (its hello again)
    if (r >= 0 && g_links[r].held && !g_links[r].presumed) release(r);   // another box on this transport now: the one before left
    const int s = claim(p);
    if (s >= 0) route(t, s);
    return s;
  }
  if (d[4] != 0x35) return r;   // (a transport that goes nowhere: only a hello or a Rytm's 35 gives it a slot)
  // a Rytm's 35, her hello (a remote-UI box's slot keeps its own: translate drops a 35 from it)
  if (r >= 0 && (g_links[r].held == &kRytm || (g_links[r].box && (g_links[r].box->features & kRemoteUi)))) {
    if (g_links[r].presumed) g_links[r].presumed = false;
    return r;
  }
  if (r >= 0) release(r);   // a Rytm on this transport now: the box before it left
  const int s = claim(&kRytm);
  if (s >= 0) route(t, s);
  return s;
}

CORE_COLD void reset() {
  rewire();   // (the platform's sender taken, if it is not yet)
  void (*plat)(const uint8_t*, int) = s_tx[0];
  g_cur = g_base = &g_links[0], g_depth = 0, s_card_link = -1, g_web_link = 0, out::g_base_slot = 0;
  s_route[0] = 0, s_route[1] = -1;
  for (Link& l : g_links) select(l, nullptr), l.state = nullptr, l.was = nullptr, l.parting = l.holding = false, l.out = nullptr;
  select(g_links[0], &kRytm), g_links[0].presumed = true;   // (as at boot)
  for (Link& l : g_links) l.out = nullptr;
  s_tx[0] = s_tx[1] = nullptr;
  for (int k = 0; k < kLinks; ++k) s_usb_want[k] = nullptr, s_usb_gone[k] = false;
  hooks.to_rytm = plat;          // back to the platform's sender (taken again: a test may set another)
  strip::reset();
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

// A hello's answer: kOk when slot_of claimed this slot for it (else engine_sysex_link answered it, with hello_status)
uint8_t hello_status(const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1) return kUnknown;
  const Profile* p = by_id((uint16_t)(d[6] << 7 | d[7]));
  return !p ? kUnknown : d[8] != kProto ? kBadProto : kWait;
}

CORE_COLD void hello(Link& l, const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1) return;
  int id = d[6] << 7 | d[7];
  const Profile* p = by_id((uint16_t)id);
  uint8_t st = p && d[8] == kProto && l.held == p ? kOk : hello_status(d, len);
  if (st != kOk) log(CORE_STR("device: hello from id %d proto %d on slot %d: %d"), id, d[8], l.index, st);
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
  if (slot < 0) {   // a hello refused (an unknown box or protocol, or no slot free: it waits); anything else goes nowhere
    if (len >= 10 && d[4] == device::kCmdHello) {
      const uint8_t a[10] = {0xF0, 0x7D, 0x52, 0x54, device::kCmdHello, 1, d[6], d[7], device::hello_status(d, len), 0xF7};
      if (auto* out = device::tx(t)) out(a, (int)sizeof a);
    }
    return;
  }
  device::Scope s(device::g_links[slot]);
  host_sysex_slot(d, len);
}

CORE_COLD void engine_sysex_self(const uint8_t* d, int len) {
  device::Scope s(device::g_links[0]);
  host_sysex_slot(d, len);
}

}  // namespace core
