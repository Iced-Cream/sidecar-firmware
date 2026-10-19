// core/ui/remote.cpp - see remote.h. Cold (program flash on the Teensy): a tick draws only while the page is open.
#include "ui/remote.h"

#include <string.h>

#include "ui/extra.h"

#include "fx/send_fx.h"
#include "hooks.h"
#include "lfo.h"
#include "machines.h"
#include "pool/pool.h"
#include "version.h"
#include "voices.h"

namespace core {
namespace ui {

namespace {

enum : uint8_t {
  kMode = 0x30, kKey = 0x31, kEnc = 0x32, kRect = 0x38, kCommit = 0x39, kOpen = 0x3A, kAlive = 0x3C,
  kText = 0x3D, kTextClear = 0x3E, kEcho = 0x3F, kMask = 0x37, kHello = 0x35, kSplash = 0x36, kMachines = 0x33,
  kKit = 0x3B,
  // 1027 SRC view (rytm-mods 0026 rui1027): Rytm -> host 29 <track> <on>, the stock SRC page of track t shown (also
  // on a machine change there and after each hello) / left; 2A <track> <first> <n> {<int> <frac>} x n that track's
  // P<first+1>.. (the active kit's words: P1..P8 the SRC knobs A..H, P9..P16 the SMPL page's; all 16 after each 29 t 1,
  // then the changed span each tick). No host -> Rytm 29 (0022 passes only 30..3F): the Rytm shows our frame from the
  // first 39 after its 29 t 1 while 3C / frames keep coming; 3A x 0 with the page closed gives stock back.
  kSrcView = 0x29, kSrcVals = 0x2A,
  // 1027d diagnostic: 2B <state> <views> <vt0..vt4> <age>: state bits 0 view on, 1 covered, 2 committed, 3 host idle
  // >= 1 s, 4 TRK + YES open, 5 splash, 6 our frame shown; the view list's length; the top view's vtable (vt0 << 28 |
  // .. | vt4); ticks since it changed. On a change, and once a second on the SRC page: logged when state / top change.
  kSrcDiag = 0x2B,
  kLeds = 0x34,  // 1026 (rytm-mods 0026 rui1026): host -> Rytm 0 (stock) | 1 <c1..c16> (palette 0..111) the trig keys' LEDs   // 1026 (provisional, rytm-mods 0026): Rytm -> host <kit 0..127>, the active kit (after each hello, on a change)
};
enum : int { kKeyLeft = 0x40, kKeyRight = 0x41, kKeyUp = 0x42, kKeyDown = 0x43, kKeyNo = 0x45, kKeyPage = 0x84 };
// 1022: the catch mask (37): what a page takes; everything else works on the Rytm as on its stock screen (pads, the
// trig and bank keys, MUTE / CHRO / SCNE / PERF, FIX, RTRG, FILL, FUNC, TEMPO, and on a voice page the LEVEL knob
// and its push and an encoder the page does not use (the status pages catch every knob); RECORD / PLAY / STOP are the Rytm's whatever the mask says). Every page takes the
// navigation keys and the keys that open or change a stock screen (rytm-mods 0026 design.md section 10: the screen
// would change under the page), plus the encoders it uses and their pushes. TRK too: a second TRK within ~1 s opens a
// stock screen, which leaving by TRK + YES soon after a TRK on the page would do.
constexpr uint8_t kCatchKeys[] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x84,   // LEFT RIGHT UP DOWN YES NO PAGE
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35,         // TRIG SRC SMPL FLTR AMP LFO (the stock pages)
    0x10, 0xB0, 0xB4, 0xB5, 0xB1, 0x81, 0x82,   // FX GLOBAL SAMPLER PLAYMODE QPER SONG CHAIN
    0x86,                                       // TRK
};
constexpr uint8_t kKeyPushA = 0x70;   // PUSH_A .. PUSH_H 0x70..0x77
constexpr int kMaskBytes = 3 + 37;
// A frame at most every kTickMs (status pages, the Rytm's own value changes); after a key or a turn at once (the next
// remote_tick at least kInputMs after the last frame). The Rytm renders at 20 Hz: a frame waits for its next render
// either way, so this is the host's part of the round trip, not more frames on the screen. 1020: 50 ms, no input path.
constexpr uint32_t kTickMs = 25, kInputMs = 8, kAliveMs = 250;
constexpr uint8_t kEchoNone = 0x7F;
// 1023: the connect splash (rytm-mods 0026 1023): the Rytm's hello (35) carries its uptime in 50 ms ticks; when it has
// been up kSplashMinUp or more (the link came up after the Rytm's boot, not with it: a cable plugged in, the Sidecar
// started later, a host restart) and the page is closed, the splash shows for kSplashMs: the icon slides in over
// kSlideMs, a line under it runs down to the end, then 36 0. The Rytm holds a splash kSplashTicks at most by itself.
constexpr uint32_t kSplashMs = 3000, kSlideMs = 150, kSlideStepMs = 50, kSplashStepMs = 100;
constexpr int kSplashMinUp = 60;          // 3 s of the Rytm's ticks
constexpr uint8_t kSplashTicks = 64;      // 3.2 s: if our 36 0 is lost
constexpr RFont kRBold = {7, 10, 0, 7};   // the Rytm's bold font: 10 rows, 6 px glyphs + 1 [measured, rytm-emu 1.73]
// The USB trident, 42 x 19, drawn for this project: a round plug, the shaft and its arrow, a branch up to a round end and
// one down to a square end. Row bitmaps, bit x = column x.
constexpr int kIconW = 42, kIconH = 19, kIconX = 6, kIconY = 21;
UI_ROM const uint64_t kUsbIcon[kIconH] = {
    0x0000f000000, 0x0001f800000, 0x0001fff0000, 0x0001fff8000, 0x0001f80c000, 0x00c0f006000, 0x03c00003000,
    0x0fc0000181e, 0x1fc00000c3f, 0x3ffffffffff, 0x3ffffffffff, 0x1fc0006003f, 0x0fc000c001e, 0x03cfc180000,
    0x00cfc300000, 0x000ffe00000, 0x000ffc00000, 0x000fc000000, 0x000fc000000,
};

// the voice byte's machine (voice_report's flags) from a voice's state
CORE_COLD int machine_of_state(int vi) {
  const V& x = v[vi];
  return x.user >= 0 ? 0x50 + 8 * x.user : x.fm ? kFm : x.juno ? kJuno : x.grain ? kGrainMachine : x.dsps ? kDspSample : x.daisy ? kDaisy
                                                                                 : x.dd ? kDigidrum : kPlaits;
}
// The machine the Rytm's track plays, as its reports name it (the voice byte, the same flags the engine plays the voice
// by), or kNoMachine before the first report: the voice's state says PLAITS then, which an analog track, or one just
// set to JUNO / 6-OP FM on the SRC page, is not. 0021 reports a voice only while it sounds (a trig, or a knob while
// it plays; [measured, rytm-emu rui1023: the SRC page's machine list sends nothing]), so a track's page appears at its
// first trig on an EXT machine and follows every later report live (page_ids runs every frame).
constexpr int kNoMachine = -1;
bool reported[kVoices];
// 1025 (rytm-mods 0026 "1025", 33): the Rytm tells each track's machine itself, at once (the SRC list's cursor, YES, a
// kit or project load) and as a snapshot after its hello and the page's open: per track (0..11) the voice byte its
// 03 / 04 would carry, or 0x7F (not a Sidecar machine). Once one came, it decides alone; before (an older Rytm build),
// the voices' reports do.
constexpr uint8_t kNotOurs = 0x7F;
uint8_t track_vb[12];
// The voice each track plays on, as 0021's 03 / 04 carry it (the firmware's voice -> track owner table at 0x40290dd8,
// 00 04 01 05 08 06 0a 02 on rui1026's base [measured, rytm-emu dtpeek], and the Rytm's live 04s on the Pi [measured
// 2026-10-19: SD on JUNO trigs voice 2, CH on 6-OP FM voice 4, MT / HT voice 5]); a shared voice's second track (CP OH
// HT CB) takes it over at its trig. 0026's 33 puts its own table's voice in vb (BD 0, SD 1, RS/CP 2 ...), right only
// for BD and MT / HT, so its voice bits are replaced by this one's and only its machine flag is kept.
UI_ROM const uint8_t kTrackVoice[12] = {0, 2, 7, 7, 1, 3, 5, 5, 4, 4, 6, 6};
CORE_COLD uint8_t track_vb_of(int t, uint8_t vb) {
  vb &= 0x7F;
  return vb == kNotOurs ? kNotOurs : (uint8_t)((vb & 0x78) | kTrackVoice[t]);
}
bool have_tracks = false;
// A voice page's name for the user: the track that plays the voice (BD .. CB; of a shared voice the first track on a
// Sidecar machine, else its first), never the Sidecar's voice number (voice 1 is BT's, voice 2 SD's).
UI_ROM const char* const kTrackName[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
CORE_COLD const char* voice_label(int vi) {
  vi &= 7;
  int first = -1;
  for (int t = 0; t < 12; ++t) {
    if (kTrackVoice[t] != vi) continue;
    if (first < 0) first = t;
    if (have_tracks && track_vb[t] != kNotOurs) return kTrackName[t];
  }
  return kTrackName[first < 0 ? 0 : first];
}
CORE_COLD int machine_of_voice(int vi) {
  vi &= 7;
  if (have_tracks) {
    for (int t = 0; t < 12; ++t)   // the first track on this voice with a Sidecar machine (CP after RS, OH after CH...)
      if (track_vb[t] != kNotOurs && (track_vb[t] & 7) == vi) return track_vb[t] & 0x78;
    return kNoMachine;
  }
  return reported[vi] ? machine_of_state(vi) : kNoMachine;
}
CORE_COLD const char* short_name(int m) {
  switch (m) {
    case kPlaits: return "PLAITS";
    case kJuno: return "JUNO";
    case kDaisy: return "DAISY";
    case kDspSample: return "DSPS";
    case kDigidrum: return "DD";
    case kFm: return "FM";
    case kGrainMachine: return "GRAIN";
    case kNoMachine: return "-";
    default: {
      static const char* const u[5] = {"U1", "U2", "U3", "U4", "U5"};
      int s = user_slot_of(m);
      return s >= 0 && s < 5 ? u[s] : "?";
    }
  }
}

CORE_COLD int v_param(int vi, int k) {
  int x = (int)v[vi & 7].p[k & 15];
  return x < 0 ? 0 : x > 127 ? 127 : x;
}

// The SRC view's source: a track's P1..P16 as the Rytm's stock page shows them (2A), the voice's own until they come;
// a sample's data from the voice the track plays on. Only reads: the engine has the sound from 0021's reports.
class TrackSource : public Source {
 public:
  int t = 0;
  CORE_COLD int param(int k) const override;
  CORE_COLD int voice() const override { return kTrackVoice[t]; }
};

class VoiceSource : public Source {
 public:
  int vi = 0;
  CORE_COLD int param(int k) const override {
    int x = (int)v[vi].p[k & 15];
    return x < 0 ? 0 : x > 127 ? 127 : x;
  }
  CORE_COLD int voice() const override { return vi; }
};

// The status pages (the first two): SIDECAR, every track's machine and the send FX (TYPE, PRE / POST); SYSTEM, U1..U5,
// the recorder, Overbridge and underruns, each core's load as a bar, the network (up / down and the address only).
// Stock geometry: the 9-row title bar (the page dots at its right), the 9-row bottom bar, rows 10..53 between.
struct StatusView {
  RemoteStatus st = {-1, 0, {-1, -1, -1}, 0, 0, 0, 0, {0, 0, 0, 0}, {{0}, {0}, {0}, {0}, {0}}};
  CORE_COLD void draw(Canvas& c, int which, int page, int npages) {
    c.clear();
    // SYSTEM carries the Sidecar's version in its title ("SYSTEM 0.1.0": 12 characters at 5 px = 60 px, clear of the page
    // dots from x = 78; docs/versioning.md); the commit is on the web pages and the console
    header(c, which ? "SYSTEM " SIDECAR_VERSION : kStatusTitle, page, npages, nullptr);
    if (which) system(c);
    else sidecar(c);
  }
  // Every item fits by the widest advance (5 px, kRSmall) or is centred (put_text bounds a left-aligned item by that
  // advance, which cut "PLAITS" to "PLAIT" in the last column).
  CORE_COLD void sidecar(Canvas& c) {
    char b[kTextLen + 1];
    // the tracks: two rows of four 32 px columns, the number in a 6 x 7 badge (pixels: the Rytm inverts only a glyph's
    // own cell, which drew a bare inverted digit; its digits are 3 or 4 px wide), the machine centred in the 24 px
    // after it (the Rytm draws "PLAITS" 24 px wide [measured, rytm-emu rui1021])
    for (int vi = 0; vi < kVoices; ++vi) {
      int x = 32 * (vi & 3), y = 13 + 11 * (vi >> 2);
      c.fill(Rect{(int16_t)(x + 1), (int16_t)(y - 1), 6, 7}, kOn);
      sniprintf(b, sizeof b, "%d", vi + 1);
      c.put_text(kSlotFree + 2 * vi, kRSmall, x + 2, y, b, kTextInvert);
      c.put_text(kSlotFree + 2 * vi + 1, kRSmall, x + 20, y, short_name(machine_of_voice(vi)), kAlignCentre);
    }
    // FX1, FX2 a row each: "FX1" in a badge, the TYPE's name (12 characters: 60 px), PRE / POST at the right
    for (int sl = 0; sl < 2; ++sl) {
      int y = 38 + 9 * sl;
      const char* t = sl < fx::kSlots ? fx::type_name(fx::control.knob[sl][0], sl) : nullptr;
      bool off = !t || !*t || !strcmp(t, "OFF");
      bool post = fx::control.route == fx::kRouteMain ||
                  (fx::control.route == fx::kRouteSlot && fx::control.knob[sl][1] >= 64);
      c.fill(Rect{1, (int16_t)(y - 1), 17, 7}, kOn);
      sniprintf(b, sizeof b, CORE_STR("FX%d"), sl + 1);
      c.put_text(kSlotFree + 16 + 3 * sl, kRSmall, 2, y, b, kTextInvert);
      sniprintf(b, sizeof b, CORE_STR("%.12s"), off ? "OFF" : t);
      c.put_text(kSlotFree + 17 + 3 * sl, kRSmall, 24, y, b);
      c.put_text(kSlotFree + 18 + 3 * sl, kRSmall, 126, y, off ? nullptr : post ? "POST" : "PRE", kAlignRight);
    }
    footer(c, nullptr);
  }
  CORE_COLD void system(Canvas& c) {
    char b[kTextLen + 1];
    // U1..U5 in two 64 px columns: "U1 " and the whole name (8 characters: 55 px at most)
    for (int u = 0; u < 5; ++u) {
      sniprintf(b, sizeof b, CORE_STR("U%d %.8s"), u + 1, st.user[u][0] ? st.user[u] : "-");
      c.put_text(kSlotFree + u, kRSmall, 2 + 64 * (u & 1), 12 + 8 * (u >> 1), b);
    }
    // the recorder beside U5: a dot and the time while recording (hours of minutes fit), else its state
    static const char* const kRec[] = {"REC -", "REC IDLE", "REC ARM", "REC", "REC OFF"};
    if (st.rec == 3) {
      c.fill(Rect{66, 29, 3, 3}, kOn);
      sniprintf(b, sizeof b, CORE_STR("REC %lu:%02lu"), (unsigned long)(st.rec_secs / 60), (unsigned long)(st.rec_secs % 60));
      c.put_text(kSlotFree + 5, kRSmall, 71, 28, b);
    } else {
      sniprintf(b, sizeof b, "%s", kRec[st.rec < 5 ? st.rec : 0]);
      c.put_text(kSlotFree + 5, kRSmall, 66, 28, b);
    }
    sniprintf(b, sizeof b, CORE_STR("OB %s"), st.ob < 0 ? "-" : st.ob ? "OK" : "STOP");
    c.put_text(kSlotFree + 6, kRSmall, 2, 37, b);
    sniprintf(b, sizeof b, CORE_STR("UND %lu"), (unsigned long)st.underruns);
    c.put_text(kSlotFree + 7, kRSmall, 66, 37, b);
    for (int k = 0; k < 3; ++k) {   // each core: a bar (rows 46..50), its percent beside it
      Rect o = {(int16_t)(2 + 42 * k), 46, 18, 5};
      c.rect(o, kOn);
      int pct = st.cpu[k];
      if (pct >= 0) {
        int w = (pct > 100 ? 100 : pct) * (o.w - 2) / 100;
        if (w) c.fill(Rect{(int16_t)(o.x + 1), (int16_t)(o.y + 1), (int16_t)w, 3}, kOn);
        sniprintf(b, sizeof b, CORE_STR("%d%%"), pct);
      } else {
        c.dither(Rect{(int16_t)(o.x + 1), (int16_t)(o.y + 1), (int16_t)(o.w - 2), 3});
        strcpy(b, "-");
      }
      c.put_text(kSlotFree + 8 + k, kRSmall, o.x + o.w + 2, 46, b);
    }
    // the network in the bottom bar: up or down and the address, never the Wi-Fi's name or key
    const uint8_t* ip = st.ip;
    bool addr = ip[0] | ip[1] | ip[2] | ip[3];
    if (st.eth == 2 && addr) sniprintf(b, sizeof b, CORE_STR("ETH UP %u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
    else if (st.wifi == 2 && addr) sniprintf(b, sizeof b, CORE_STR("WIFI UP %u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
    else if (st.eth == 2 || st.wifi == 2) sniprintf(b, sizeof b, CORE_STR("%s UP, NO ADDRESS"), st.eth == 2 ? "ETH" : "WIFI");
    else if (st.eth || st.wifi) strcpy(b, "NET DOWN");
    else strcpy(b, "NET -");
    footer(c, b);
  }
};

constexpr uint8_t kPageLfo = 1;   // 30's page byte (rui1026): 0 the default (SIDECAR), 1 the LFO page
constexpr int kExtra0 = -10;   // s.voice of extra page k (extra.h: SCOPE, REC): kExtra0 - k
constexpr int kMaxIds = 2 + kVoices + 8;

struct State {
  bool open = false;
  int voice = -1;            // the page: -1 SIDECAR, -2 SYSTEM, kExtra0 - k extra page k, else the voice whose machine page shows
  bool sent_valid = false;   // `sent` is what the Rytm's shadow holds
  bool page_changed = true;
  uint8_t seq = 0;
  uint32_t last_draw = 0, last_tx = 0;
  bool clock_set = false;
  // held edits: per voice a mask of P1..P16, their values, the Rytm's last value of each, the machine they are for
  uint16_t held[kVoices] = {};
  uint8_t held_v[kVoices][16] = {};
  uint8_t rytm_v[kVoices][16] = {};
  uint8_t held_m[kVoices] = {};
  bool rytm_seen[kVoices] = {};
  bool self = false;         // remote's own 0x03 in voice_report: no hold logic
  bool input = false;        // a key or a turn since the last frame: draw at once
  int touched = -1;          // the encoder turned last (0..7): its cell's tiles go first, the bottom bar names it
  // the local echo (3F) as the Rytm has it: per encoder the slot (kEchoNone: none), the value, flags and prefix
  uint8_t echo_slot[8], echo_val[8], echo_flags[8];
  char echo_pre[8][12];
  bool echo_valid = false;
  bool legacy = false;       // the 1020 behaviour (tests: before / after): 50 ms frames, no echo, one commit
  uint8_t mask[kMaskBytes] = {};   // 1022: the catch mask the Rytm has (37), valid after the first since opening
  bool mask_valid = false;
  bool splash = false;       // 1023: the connect splash runs (the page closed)
  bool splash_clock = false;
  uint32_t splash_t0 = 0;
  uint8_t leds[16] = {};     // 1026: the trig LEDs the Rytm has from us (34 mode 1), valid while leds_on
  bool leds_on = false;
  // 1027: the SRC view: the track whose stock SRC page the Rytm shows (-1 none), whether we draw it (1: frames sent
  // since its 29 t 1, 0 none, -1 not decided yet), the Rytm's own values of each track's P1..P16 (2A; `src_known` which came), the one
  // changed last (2A n 1: the bottom bar names it)
  int8_t src_track = -1, src_told = -1, src_last = -1;
  uint8_t srcv[12][16] = {};
  uint16_t src_known[12] = {};
  RemoteStats stats = {};
};

CORE_RAM2 State s;               // RAM2 on the Teensy, as the canvases (constructed at boot)
CORE_RAM2 Canvas work, sent;   // 2 KB
PageView view;
VoiceSource vsrc;
CORE_RAM2 StatusView ssrc;
TrackSource tsrc;
bool src_log = false;   // the console's "rui=srclog=1": each 29 / 2A and SRC view commit logged
// The Rytm shows the machine pages on its own SRC page (rui1027+: it sent a 29, 2A or 2B): the TRK + YES list then has
// no voice pages (SIDECAR, SYSTEM and the live pages); an older build keeps them. Until remote_reset (a boot).
bool src_capable = false;

CORE_COLD int TrackSource::param(int k) const {
  k &= 15;
  return s.src_known[t] >> k & 1 ? s.srcv[t][k] : v_param(kTrackVoice[t], k);
}

CORE_COLD void send(const uint8_t* m, int n) {
  to_rytm(m, n);
  s.stats.bytes += (uint32_t)n;
}

CORE_COLD int head(uint8_t* m, uint8_t cmd) {
  m[0] = 0xF0, m[1] = 0x7D, m[2] = 0x52, m[3] = 0x54, m[4] = cmd, m[5] = 1;
  return 6;
}

CORE_COLD void send_short(uint8_t cmd, const uint8_t* b, int nb) {   // nb <= kMaskBytes
  uint8_t m[8 + kMaskBytes];
  int n = head(m, cmd);
  for (int i = 0; i < nb; ++i) m[n++] = b[i] & 0x7F;
  m[n++] = 0xF7;
  send(m, n);
}

// 38: tile columns c0..c0+nc-1 of framebuffer page p, column-major (byte = page + 8 x column), 7 -> 8 packed
CORE_COLD void send_rect(int c0, int p, int nc) {
  uint8_t raw[8 * kTileCols], m[16 + 8 * kTileCols + kTileCols * 8 / 7 + 2];
  int nr = 0;
  for (int x = c0 * 8; x < (c0 + nc) * 8; ++x) raw[nr++] = work.fb[p + 8 * x];
  int n = head(m, kRect);
  m[n++] = s.seq;
  m[n++] = (uint8_t)c0;
  m[n++] = (uint8_t)p;
  m[n++] = (uint8_t)nc;
  m[n++] = 1;
  for (int i = 0; i < nr; i += 7) {
    int g = nr - i < 7 ? nr - i : 7;
    uint8_t hi = 0;
    for (int j = 0; j < g; ++j) hi |= (uint8_t)((raw[i + j] >> 7) << j);
    m[n++] = hi;
    for (int j = 0; j < g; ++j) m[n++] = raw[i + j] & 0x7F;
  }
  m[n++] = 0xF7;
  send(m, n);
  s.stats.rects++;
}

// 3D: slot i of `work` (empty: the slot cleared)
CORE_COLD void send_text(int i) {
  const TextItem& t = work.txt[i];
  uint8_t m[16 + kTextLen];
  int n = head(m, kText);
  m[n++] = s.seq, m[n++] = (uint8_t)i, m[n++] = t.x & 0x7F, m[n++] = t.y & 0x3F, m[n++] = t.font & 0x7F;
  m[n++] = t.flags & 0x7F;
  for (const char* c = t.s; *c; ++c) m[n++] = (uint8_t)*c & 0x7F;
  m[n++] = 0xF7;
  send(m, n);
  s.stats.texts++;
}

CORE_COLD const ExtraPage* extra_of(int id) { return id <= kExtra0 ? &extra_page(kExtra0 - id) : nullptr; }

// The pages in order (the title bar's dots): SIDECAR, SYSTEM, the voices with a page, the extra pages shown.
CORE_COLD int page_ids(int* ids) {
  int n = 0;
  ids[n++] = -1, ids[n++] = -2;
  for (int vi = 0; vi < kVoices && !src_capable; ++vi)
    if (page_for_voice(machine_of_voice(vi), vi)) ids[n++] = vi;
  for (int k = 0; k < extra_pages() && n < kMaxIds; ++k)
    if (!extra_page(k).shown || extra_page(k).shown()) ids[n++] = kExtra0 - k;
  return n;
}

CORE_COLD int page_at(const int* ids, int n) {   // the current page's index (0 when it is not in the list)
  for (int i = 0; i < n; ++i)
    if (ids[i] == s.voice) return i;
  return 0;
}

CORE_COLD void go(int dir) {   // dir: -1 previous, +1 next, 0 SIDECAR
  int ids[kMaxIds], n = page_ids(ids);
  int at = page_at(ids, n);
  at = dir == 0 ? 0 : (at + dir + n) % n;
  int nv = ids[at];
  if (nv != s.voice) s.voice = nv, s.page_changed = true, s.touched = -1;
}

CORE_COLD void set_param(int vi, int k, int nv) {
  V& x = v[vi];
  int m = machine_of_voice(vi);
  if (s.held_m[vi] != m) s.held[vi] = 0, s.held_m[vi] = (uint8_t)m;
  if (!(s.held[vi] & (1u << k))) s.rytm_v[vi][k] = (uint8_t)(int)lfo::base_of(vi, k, x.p[k]);   // the Rytm's value until now
  s.held[vi] |= (uint16_t)(1u << k);
  s.held_v[vi][k] = (uint8_t)nv;
  // a 0x03 PARAMS for the voice, its machine and every P (the edited one whole, the others as they are)
  uint8_t r[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(m | vi)};
  int n = 6;
  for (int i = 0; i < 16; ++i) {
    float p = i == k ? (float)nv : lfo::base_of(vi, i, x.p[i]);   // an LFO's target: its base, not the modulated value
    int iv = (int)p;
    iv = iv < 0 ? 0 : iv > 127 ? 127 : iv;
    int fr = (int)((p - iv) * 128.0f);
    r[n++] = (uint8_t)iv;
    r[n++] = (uint8_t)(fr < 0 ? 0 : fr > 127 ? 127 : fr);
  }
  r[n++] = 0xF7;
  s.self = true;
  voice_report(0x03, r, n);
  s.self = false;
  s.stats.edits++;
}

CORE_COLD void on_enc(int e, int delta, bool func) {
  s.input = true;
  if (const ExtraPage* x = extra_of(s.voice)) {
    if (e >= 0 && e < 8 && (extra_encs(*x) >> e & 1)) x->enc(e, delta, func);
    return;
  }
  if (s.voice < 0 || src_capable || e < 0 || e > 7) return;
  s.touched = e;
  const Page* p = page_for_voice(machine_of_voice(s.voice), s.voice);
  if (!p || p->enc[e] == kNone) return;
  int k = p->enc[e] & 15, cur = (int)lfo::base_of(s.voice, k, v[s.voice].p[k]);
  int nv = step_value(*p, k, cur < 0 ? 0 : cur > 127 ? 127 : cur, delta, func);
  if (nv != cur) set_param(s.voice, k, nv);
}

CORE_COLD void on_key(int code, bool down) {
  if (!down) return;
  s.input = true;
  if (const ExtraPage* x = extra_of(s.voice))
    if (x->key && x->key(code)) return;
  if (code == kKeyLeft || code == kKeyUp) go(-1);
  else if (code == kKeyRight || code == kKeyDown || code == kKeyPage) go(+1);
  else if (code == kKeyNo && s.voice == -1) remote_request(false);   // NO on SIDECAR leaves, as TRK + YES (3A 0 0)
  else if (code == kKeyNo) go(0);
}

// The bottom bar of a voice page: the touched encoder's parameter (or encoder A's) as stock's "BD LEV:", with a level
// box; the encoders' local echo bindings follow it.
CORE_COLD const Cell* cell_of(const Page& p, int param, int* idx) {
  for (int i = 0; i < p.ncells; ++i)
    for (int k = 0; k < 4; ++k)
      if (p.cells[i].in[k] == param) { *idx = i; return &p.cells[i]; }
  return nullptr;
}

CORE_COLD void voice_footer(const Page& p, const char* name, const Source& src, int e) {
  int k = p.enc[e], ci = -1;
  char b[kTextLen + 1];
  if (k == kNone) { footer(work, nullptr); return; }
  const Cell* c = cell_of(p, k, &ci);
  int v = src.param(k);
  if (c && c->kind == kList) {
    sniprintf(b, sizeof b, CORE_STR("%s %s:%.8s"), name, p.label[e], list_name(c->names, eng_zone((float)v, list_count(c->names))));
    footer(work, b);
  } else if (c && (c->kind == kKnobBi || c->kind == kPan)) {
    sniprintf(b, sizeof b, CORE_STR("%s %s:%+d"), name, p.label[e], v - 64);
    footer(work, b, v, true);
  } else {
    sniprintf(b, sizeof b, CORE_STR("%s %s:%d"), name, p.label[e], v);
    footer(work, b, v);
  }
}

// 3F for every encoder whose binding the Rytm does not have yet: numeric parameters echo into the bottom bar's slot
// ("V1 HRM:" + the value), lists and the SIDECAR page none.
CORE_COLD void send_echoes(const Page* p, int vi) {
  for (int e = 0; e < 8; ++e) {
    uint8_t slot = kEchoNone, val = 0, fl = 0;
    char pre[12] = "";
    int ci = -1;
    const Cell* c = p && !s.legacy && p->enc[e] != kNone ? cell_of(*p, p->enc[e], &ci) : nullptr;
    if (c && c->kind != kList) {
      slot = kSlotFoot;
      val = (uint8_t)v_param(vi, p->enc[e]);
      fl = 1 | (c->kind == kKnobBi || c->kind == kPan ? 2 : 0);   // FUNC x 8 as step_value; bipolar shown signed
      sniprintf(pre, sizeof pre, CORE_STR("%s %s:"), voice_label(vi), p->label[e]);
    }
    if (s.echo_valid && s.echo_slot[e] == slot && (slot == kEchoNone || (s.echo_val[e] == val && s.echo_flags[e] == fl &&
                                                                           !strcmp(s.echo_pre[e], pre))))
      continue;
    uint8_t m[24];
    int n = head(m, kEcho);
    m[n++] = (uint8_t)e, m[n++] = slot, m[n++] = val & 0x7F, m[n++] = 0, m[n++] = 127, m[n++] = fl;
    for (const char* q = pre; *q; ++q) m[n++] = (uint8_t)*q & 0x7F;
    m[n++] = 0xF7;
    send(m, n);
    s.echo_slot[e] = slot, s.echo_val[e] = val, s.echo_flags[e] = fl;
    strcpy(s.echo_pre[e], pre);
    s.stats.echoes++;
  }
  s.echo_valid = true;
}

// 37 when the page's catch mask is not the one the Rytm has: the navigation and stock-screen keys, the encoders the
// page uses and their pushes (the status pages: all nine, LEVEL too, so a turn there edits nothing underneath; the live
// pages without a Page, SCOPE / REC, pass their own encoders in `encs`).
CORE_COLD void send_mask(const Page* p, uint8_t encs = 0, uint16_t trigs = 0) {
  uint8_t b[kMaskBytes] = {};
  for (uint8_t k : kCatchKeys) b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
  for (int k = 0; k < 16; ++k)   // T1..T16 (key codes 0..15): a page that takes them now (the LFO design view)
    if (trigs >> k & 1) b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
  if (!p && !encs)
    for (int e = 0; e < 9; ++e) {   // A..H and LEVEL (index 8, push 0x78)
      int k = kKeyPushA + e;
      b[e / 7] |= (uint8_t)(1u << (e % 7));
      b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
    }
  for (int e = 0; e < 8; ++e)
    if ((p && p->enc[e] != kNone) || (encs >> e & 1)) {
      int k = kKeyPushA + e;
      b[e / 7] |= (uint8_t)(1u << (e % 7));
      b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
    }
  if (s.mask_valid && !memcmp(b, s.mask, sizeof b)) return;
  int n = sizeof b;
  while (n > 3 && !b[n - 1]) --n;   // the key bytes up to the last caught code
  send_short(kMask, b, n);
  memcpy(s.mask, b, sizeof b);
  s.mask_valid = true;
  s.stats.masks++;
}

// 34 (rui1026): the trig keys' LEDs, mode 1 with c[0..15] (palette colours, kLed*), or mode 0 alone (stock again)
CORE_COLD void send_leds(const uint8_t* c) {
  uint8_t b[17] = {};
  b[0] = c ? 1 : 0;
  if (c) memcpy(b + 1, c, 16);
  send_short(kLeds, b, c ? 17 : 1);
  s.leds_on = c != nullptr;
  if (c) memcpy(s.leds, c, 16);
  s.stats.leds++;
}
// A page that lights the trig keys (ExtraPage::leds_now): sent when it shows and on every change, mode 0 when it is left
CORE_COLD void update_leds(const ExtraPage* x) {
  uint8_t c[16];
  bool want = !s.legacy && x && x->leds_now && x->leds_now(c);
  if (want && (!s.leds_on || memcmp(c, s.leds, 16))) send_leds(c);
  else if (!want && s.leds_on) send_leds(nullptr);
}

// One commit: the tiles in `tiles` and the text slots in `texts` (from `work`), then 39. Returns the tiles sent.
CORE_COLD int send_part(const uint32_t tiles[4], uint64_t texts, bool clear_first) {
  int n = 0;
  s.seq = (uint8_t)((s.seq + 1) & 0x7F);
  if (clear_first) {
    uint8_t b = s.seq;
    send_short(kTextClear, &b, 1);
  }
  for (int pg = 0; pg < kTilePages; ++pg)
    for (int c = 0; c < kTileCols;) {
      int t = pg * kTileCols + c;
      if (!(tiles[t >> 5] >> (t & 31) & 1)) { ++c; continue; }
      int c0 = c;
      while (c < kTileCols && (tiles[(pg * kTileCols + c) >> 5] >> ((pg * kTileCols + c) & 31) & 1)) ++c;
      send_rect(c0, pg, c - c0);
      n += c - c0;
    }
  for (int i = 0; i < kTextSlots; ++i)
    if (texts >> i & 1) send_text(i);
  uint8_t b = s.seq;
  send_short(kCommit, &b, 1);
  s.stats.frames++;
  return n;
}

// The tiles a rectangle touches (glass rows y..y+h-1: page 7 - row / 8).
CORE_COLD void rect_tiles(const Rect& r, uint32_t m[4]) {
  for (int x = r.x / 8; x <= (r.x + r.w - 1) / 8 && x < kTileCols; ++x)
    for (int y = r.y / 8; y <= (r.y + r.h - 1) / 8 && y < 8; ++y) {
      int t = (7 - y) * kTileCols + x;
      m[t >> 5] |= 1u << (t & 31);
    }
}

CORE_COLD void frame(uint32_t ms) {
  uint32_t t0 = now_us();
  const Page* p = nullptr;
  if (s.voice >= 0 && src_capable) s.voice = -1, s.page_changed = true;   // a voice page shown when the Rytm said 29 / 2A / 2B
  if (s.voice >= 0) {
    p = page_for_voice(machine_of_voice(s.voice), s.voice);
    if (!p) s.voice = -1, s.page_changed = true;
  }
  if (s.page_changed) view.invalidate(), s.page_changed = false;
  int ids[kMaxIds], n = page_ids(ids), at = page_at(ids, n);
  if (s.voice <= kExtra0 && ids[at] != s.voice) s.voice = -1, at = 0;   // an extra page no longer shown
  const ExtraPage* x = extra_of(s.voice);
  tap_enable(x && x->tap);
  int drawn = 1;
  if (p) {
    char right[4];
    vsrc.vi = s.voice;
    sniprintf(right, sizeof right, "%s", voice_label(s.voice));
    drawn = view.draw(work, *p, vsrc, right, at, n);
    voice_footer(*p, voice_label(s.voice), vsrc, s.touched >= 0 ? s.touched : 0);
  } else if (x) {
    view.invalidate();
    x->draw(work, at, n, ms);
  } else {
    view.invalidate();   // the status pages are drawn whole; a voice page after them starts clean
    ssrc.draw(work, s.voice == -2 ? 1 : 0, at, n);
  }
  s.stats.widgets += (uint32_t)drawn;
  if (!s.legacy) send_mask(p, x ? extra_encs(*x) : 0, x && x->trigs_now ? x->trigs_now() : 0);   // before the frame: the keys and knobs the page shows are its own from now on
  update_leds(x);
  if (!s.sent_valid) s.echo_valid = false;
  send_echoes(p, s.voice);
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint64_t texts = 0;
  int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
  int ntext = s.sent_valid ? work.diff_text(sent, &texts) : 0;
  if (!s.sent_valid)
    for (int i = 0; i < kTextSlots; ++i)
      if (work.txt[i].s[0]) texts |= 1ull << i, ++ntext;
  if (tiles || ntext) {
    // the touched knob's cell and the bottom bar first, in a commit of their own, when there is more
    uint32_t first[4] = {}, rest[4];
    uint64_t tfirst = 0;
    int ci = -1;
    if (!s.legacy && s.sent_valid && p && s.touched >= 0 && p->enc[s.touched] != kNone &&
        cell_of(*p, p->enc[s.touched], &ci)) {
      const Cell& c = p->cells[ci];
      rect_tiles(cell(c.col, c.row, c.cols, c.rows), first);
      rect_tiles(Rect{0, kFooterY, kW, kFooterH}, first);
      tfirst = (1ull << (kSlotCells + ci)) | (1ull << kSlotFoot);
    }
    int nf = 0, nr = 0;
    for (int i = 0; i < 4; ++i) rest[i] = mask[i] & ~first[i], first[i] &= mask[i], nf += __builtin_popcount(first[i]),
                                nr += __builtin_popcount(rest[i]);
    uint64_t trest = texts & ~tfirst;
    tfirst &= texts;
    if ((nf || tfirst) && (nr || trest)) {
      send_part(first, tfirst, false);
      send_part(rest, trest, false);
      s.stats.split++;
    } else {
      send_part(mask, texts, !s.sent_valid);
    }
    sent = work;
    s.sent_valid = true;
    s.last_tx = ms;
    s.stats.tiles += (uint32_t)tiles;
    uint32_t dt = now_us() - t0;
    s.stats.draw_us_sum += dt;
    if (dt > s.stats.draw_us_max) s.stats.draw_us_max = dt;
  }
  s.stats.page = (uint8_t)(x ? 3 : s.voice == -2 ? 2 : s.voice < 0 ? 0 : 1);
  s.stats.voice = (uint8_t)(x ? kExtra0 - s.voice : s.voice < 0 ? 0 : s.voice);
}

// ---- 1027: the SRC view. The machine page of the track whose stock SRC page shows, drawn from the Rytm's own values
// (2A): every key and knob stays the Rytm's (no 37, no 3F, no 34), the host only draws. A track whose machine has no
// page gets none (nothing sent, the Rytm shows stock; 3A 0 0 if we drew it before). The TRK + YES page comes first:
// while it is open the view waits; after it a whole frame again.
CORE_COLD int track_machine(int t) {
  if (have_tracks) return track_vb[t] == kNotOurs ? kNoMachine : track_vb[t] & 0x78;
  return machine_of_voice(kTrackVoice[t]);
}
CORE_COLD const Page* track_page(int t) {   // DSP SAMPLE's GRAIN view by the Rytm's MOD (P12) once it came
  return src_view_page(page_for_voice(track_machine(t), kTrackVoice[t], s.src_known[t] >> 11 & 1 ? s.srcv[t][11] : -1));
}

CORE_COLD void src_draws(bool page) {
  if (!page && s.src_told == 1 && !s.open) remote_request(false);   // 3A 0 0: stock again until our next commit
  s.src_told = (int8_t)(page ? 1 : 0);
}

CORE_COLD void src_tick(uint32_t ms) {
  int t = s.src_track;
  const Page* p = track_page(t);
  if ((p ? 1 : 0) != s.src_told) {
    src_draws(p != nullptr);
    if (p) s.sent_valid = false, s.page_changed = true, s.clock_set = false;   // the Rytm's shadow: a whole frame
  }
  if (!p) return;
  wave_idle(ms);   // the sample overview of the Rytm's SMP being read (one column at most)
  if (!s.clock_set) s.clock_set = true, s.last_draw = ms - kTickMs, s.last_tx = ms;
  // a 2A at once (the Rytm sends one a 20 Hz tick at most; the 2As a USB read brought are taken before this tick, so a
  // burst is drawn once, with its latest values)
  if (s.input || ms - s.last_draw >= kTickMs) {
    s.last_draw = ms;
    s.input = false;
    if (s.page_changed) view.invalidate(), s.page_changed = false;
    tsrc.t = t;
    int drawn = view.draw(work, *p, tsrc, kTrackName[t], 0, 0);
    int e = 0;
    for (int i = 0; i < 8; ++i)
      if (s.src_last >= 0 && p->enc[i] == s.src_last) e = i;
    voice_footer(*p, kTrackName[t], tsrc, e);
    s.stats.widgets += (uint32_t)drawn;
    uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint64_t texts = 0;
    int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
    if (s.sent_valid) work.diff_text(sent, &texts);
    else
      for (int i = 0; i < kTextSlots; ++i)
        if (work.txt[i].s[0]) texts |= 1ull << i;
    if (tiles || texts) {
      bool whole = !s.sent_valid;
      send_part(mask, texts, whole);
      sent = work;
      s.sent_valid = true;
      s.last_tx = ms;
      s.stats.tiles += (uint32_t)tiles;
      s.stats.src_frames++;
      if (src_log) core::log("rui src: commit %lu ms, track %d, %d tiles, texts %llx%s", (unsigned long)ms, t, tiles,
                       (unsigned long long)texts, whole ? " (whole)" : "");
    }
    s.stats.page = 4;
    s.stats.voice = (uint8_t)t;
  }
  if (ms - s.last_tx >= kAliveMs) {
    send_short(kAlive, nullptr, 0);
    s.last_tx = ms;
    s.stats.keepalives++;
  }
}

// The splash at t ms: the stock title bar (the product's name, USB at its right), the icon (sliding in from the left
// for kSlideMs), CONNECTED in the Rytm's bold font, the voices and send FX, the network address (or the link), a line
// running down over kSplashMs, the bottom bar naming how the pages open.
CORE_COLD void splash_draw(Canvas& c, uint32_t t) {
  char b[kTextLen + 1];
  c.clear();
  header(c, kStatusTitle, 0, 0, "USB");
  int dx = 0;
  if (t < kSlideMs) {   // ease out: fast first, settling
    int r = (int)(kSlideMs - t);
    dx = (kIconX + kIconW) * r * r / (int)(kSlideMs * kSlideMs);
  }
  for (int y = 0; y < kIconH; ++y) {
    uint64_t row = kUsbIcon[y];
    for (int x = 0; x < kIconW; ++x)
      if (row >> x & 1) c.pixel(kIconX - dx + x, kIconY + y);
  }
  c.put_text(kSlotFree, kRBold, 54, 19, "CONNECTED");
  sniprintf(b, sizeof b, CORE_STR("%d VOICES  %d FX"), kVoices, fx::kSlots);
  c.put_text(kSlotFree + 1, kRSmall, 54, 33, b);
  const uint8_t* ip = ssrc.st.ip;
  if ((ssrc.st.eth == 2 || ssrc.st.wifi == 2) && (ip[0] | ip[1] | ip[2] | ip[3]))
    sniprintf(b, sizeof b, CORE_STR("%u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
  else
    strcpy(b, "USB AUDIO+MIDI");
  c.put_text(kSlotFree + 2, kRSmall, 54, 41, b);
  int w = t >= kSplashMs ? 0 : (int)((kSplashMs - t) * 124 / kSplashMs);
  if (w > 0) c.hline(2, 2 + w - 1, 52);
  footer(c, "TRK + YES: SIDECAR PAGES");
}

// The splash's frames: at once, then every kSlideStepMs while the icon slides, every kSplashStepMs after (only the
// tiles that changed: the line's end, ~1 tile a frame); 36 after the first frame, 36 0 at the end.
CORE_COLD void splash_tick(uint32_t ms) {
  bool first = !s.splash_clock;
  if (first) s.splash_clock = true, s.splash_t0 = ms, s.last_draw = ms - kSplashStepMs;
  uint32_t t = ms - s.splash_t0;
  if (t >= kSplashMs) {
    uint8_t b = 0;
    send_short(kSplash, &b, 1);
    s.splash = false;
    s.sent_valid = false;   // the Rytm's shadow holds the splash; the page sends its whole frame on opening anyway
    return;
  }
  if (!first && ms - s.last_draw < (t < kSlideMs + kSlideStepMs ? kSlideStepMs : kSplashStepMs)) return;
  s.last_draw = ms;
  splash_draw(work, t);
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint64_t texts = 0;
  int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
  if (s.sent_valid) work.diff_text(sent, &texts);
  else
    for (int i = 0; i < kTextSlots; ++i)
      if (work.txt[i].s[0]) texts |= 1ull << i;
  if (tiles || texts) send_part(mask, texts, !s.sent_valid);
  else send_short(kAlive, nullptr, 0);
  s.stats.tiles += (uint32_t)tiles;
  sent = work;
  s.sent_valid = true;
  s.last_tx = ms;
  if (first) {
    uint8_t b = kSplashTicks;
    send_short(kSplash, &b, 1);
    s.stats.splashes++;
  }
}

}  // namespace

CORE_COLD void remote_sysex(const uint8_t* d, int len) {
  if (len < 7 || d[5] != 1 || d[len - 1] != 0xF7) { s.stats.bad++; return; }
  const uint8_t* b = d + 6;
  int n = len - 7;
  if (d[4] == kSrcView || d[4] == kSrcVals || d[4] == kSrcDiag) src_capable = true;
  switch (d[4]) {
    case kMode:
      if (n < 3) break;
      if (b[1]) {
        if (!s.open) s.stats.opens++;
        if (s.src_track >= 0) s.src_told = -1;   // 1027: the page comes first; after it the view's whole frame again
        s.open = true;
        s.voice = -1;   // the SIDECAR page first; 1026: page 1 = the LFO page (TRK + LFO, also again while open)
        if (b[0] == kPageLfo)
          for (int k = 0; k < extra_pages(); ++k)
            if (!strcmp(extra_page(k).title, "LFO")) s.voice = kExtra0 - k;
        s.page_changed = true;
        s.sent_valid = false;   // the Rytm's shadow: unknown, the whole frame goes
        s.last_draw = s.last_tx = 0;
        s.clock_set = false;
        s.echo_valid = false;
        s.mask_valid = false;   // the Rytm forgets the mask when the page left by its timeout: always again
        s.touched = -1;
        s.splash = false;       // the user opened the page: it replaces the splash (the Rytm ended it)
        s.leds_on = false;      // the Rytm shows its own LEDs until a page sends 34
      } else {
        if (s.open) s.stats.closes++;
        s.open = false;
        s.clock_set = false;   // 1027: an SRC view still on starts again (a whole frame)
        s.leds_on = false;   // the Rytm restores its own LEDs on leaving (a 34 counts only while the page is open)
        tap_enable(false);
      }
      return;
    case kKey:
      if (n < 3) break;
      s.stats.keys++;
      if (s.open) on_key(b[0] << 7 | b[1], b[2] != 0);
      return;
    case kEnc:
      if (n < 3) break;
      s.stats.encs++;
      if (s.open) on_enc(b[0], (int)b[1] - 64, b[2] != 0);
      return;
    case kMachines: {   // 1025: <t0> <n> {<machine 0..52> <vb | 7F>} x n
      if (n < 2 || b[0] >= 12 || b[1] < 1 || b[0] + b[1] > 12 || n != 2 + 2 * b[1]) break;
      if (!have_tracks) memset(track_vb, kNotOurs, sizeof track_vb);
      // One track turned JUNO / 6-OP FM (the SRC list, n 1): its PAT list (16 / 17) pushed now. The Rytm keeps one
      // list cache for both (and the U slots' lists); the 17 that follows every 16 at a project load evicts JUNO's,
      // and the Rytm asks again only once in 200 draws [measured, rytm-emu rui1025: ~38 s of factory names].
      if (b[1] == 1 && track_vb[b[0]] != track_vb_of(b[0], b[3])) {
        if ((b[3] & 0x78) == kJuno) send_juno_names();
        else if ((b[3] & 0x78) == kFm) send_fm_names();
      }
      for (int i = 0; i < b[1]; ++i) track_vb[b[0] + i] = track_vb_of(b[0] + i, b[3 + 2 * i]);
      have_tracks = true;
      s.stats.machines++;
      return;
    }
    case kSrcView:   // 1027: <track> <on>
      if (n < 2 || b[0] >= 12 || b[1] > 1) break;
      s.stats.src_views++;
      if (src_log) core::log("rui src: 29 %lu ms, track %d, %s", (unsigned long)now_ms(), b[0], b[1] ? "on" : "off");
      if (b[1]) {
        s.src_track = (int8_t)b[0];
        s.src_told = -1;   // a whole frame at the next tick: the Rytm shows ours only after a 39 since this 29
        s.src_last = -1;   // a splash running (a hello just before) ends first, then the view's whole frame
      } else if (b[0] == s.src_track) {
        s.src_track = -1, s.src_told = -1;
        if (!s.open) s.sent_valid = false;   // the Rytm's own screen is back: its shadow is not ours
      }
      return;
    case kSrcDiag: {   // 1027d: logged when the state or the top view changes (or with srclog on)
      if (n < 8) break;
      static uint32_t last = 0xFFFFFFFFu;
      uint32_t vt = (uint32_t)b[2] << 28 | (uint32_t)b[3] << 21 | (uint32_t)b[4] << 14 | (uint32_t)b[5] << 7 | b[6];
      uint32_t key = (uint32_t)b[0] << 24 ^ vt ^ (uint32_t)b[1] << 16;
      if (key == last && !src_log) return;
      last = key;
      int st = b[0];
      core::log("rui: 2B %lu ms: SRC view %s%s%s%s%s%s%s, views %d, top %08lx (%d ms)", (unsigned long)now_ms(),
                st & 1 ? "on" : "off", st & 2 ? " covered" : "", st & 4 ? " committed" : "", st & 8 ? " host-idle" : "",
                st & 16 ? " page-open" : "", st & 32 ? " splash" : "", st & 64 ? " SHOWN" : " not-shown", b[1],
                (unsigned long)vt, b[7] * 50);
      return;
    }
    case kSrcVals: {   // 1027: <track> <first> <n> {<int> <frac>} x n
      if (n < 5 || b[0] >= 12 || b[1] > 15 || b[2] < 1 || b[1] + b[2] > 16 || n != 3 + 2 * b[2]) break;
      int t = b[0];
      for (int i = 0; i < b[2]; ++i) {
        s.srcv[t][b[1] + i] = b[3 + 2 * i] & 0x7F;
        s.src_known[t] |= (uint16_t)(1u << (b[1] + i));
      }
      if (b[2] == 1) s.src_last = (int8_t)b[1];
      if (t == s.src_track) s.input = true;   // drawn at once (the input path), a burst coalesced to its latest values
      s.stats.src_vals++;
      if (src_log) core::log("rui src: 2A %lu ms, track %d, first %d, n %d, P%d = %d.%d", (unsigned long)now_ms(), t, b[1], b[2],
                       b[1] + 1, b[3], b[4]);
      return;
    }
    case kKit:   // 1026: the Rytm's active kit: the LFOs are per kit (core/lfo.h)
      if (n < 1 || b[0] > 127) break;
      lfo::kit_changed(b[0]);
      return;
    case kHello: {   // 1023: <uptime >> 7> <uptime & 0x7F> <caps>: the splash when the Rytm was up before the link
      if (n < 3) break;
      s.stats.hellos++;
      int up = b[0] << 7 | b[1];
      // A new link (a replug, the Rytm's or our restart): whatever the Rytm showed before is gone - its page left by its
      // 1 s timeout, its SRC view says 29 t 1 again after this hello if it is on. Stale state here (the page open, a
      // view's track, a splash running when the cable went) blocked the splash and the view until a restart.
      bool was_open = s.open, was_view = s.src_track >= 0, was_splash = s.splash;
      if (s.open) s.stats.closes++;
      s.open = false, s.leds_on = false, s.mask_valid = false, s.echo_valid = false, s.sent_valid = false;
      s.src_track = -1, s.src_told = -1, s.splash = false, s.clock_set = false;
      tap_enable(false);
      bool splash = (b[2] & 1) && !s.legacy && up >= kSplashMinUp;
      core::log("rui: hello, Rytm up %d.%d s, caps %d%s%s%s: %s", up / 20, (up % 20) / 2, b[2], was_open ? ", page was open" : "",
                was_view ? ", SRC view was on" : "", was_splash ? ", splash was running" : "", splash ? "splash" : "alive");
      if (splash) s.splash = true, s.splash_clock = false;
      else send_short(kAlive, nullptr, 0);   // an answer: the Rytm says hello again only while the host is silent
      return;
    }
    default:
      return;   // 38..3F are the host's own
  }
  s.stats.bad++;
}

CORE_COLD void remote_tick(uint32_t ms) {
  if (s.splash && !s.open) { splash_tick(ms); return; }
  if (!s.open && s.src_track >= 0) { src_tick(ms); return; }
  if (!s.open) return;
  wave_idle(ms);   // a sample overview being read (page_wave.cpp): one column at most
  uint32_t tick = s.legacy ? 50 : kTickMs;
  if (!s.clock_set) s.clock_set = true, s.last_draw = ms - tick, s.last_tx = ms;
  if (ms - s.last_draw >= tick || (s.input && !s.legacy && ms - s.last_draw >= kInputMs)) {
    s.last_draw = ms;
    s.input = false;
    frame(ms);
  }
  if (ms - s.last_tx >= kAliveMs) {
    send_short(kAlive, nullptr, 0);
    s.last_tx = ms;
    s.stats.keepalives++;
  }
}

CORE_COLD void remote_request(bool on) {
  uint8_t b[2] = {0, (uint8_t)(on ? 1 : 0)};
  send_short(kOpen, b, 2);
}

CORE_COLD bool remote_open() { return s.open; }

CORE_COLD void remote_status(const RemoteStatus& st) { ssrc.st = st; }
CORE_COLD void remote_legacy(bool on) { s.legacy = on; }
CORE_COLD void remote_src_log(bool on) { src_log = on; }

CORE_COLD void remote_params(int vi, int m, float* p, int np) {
  vi &= 7;
  reported[vi] = true;
  if (s.self) return;
  if (s.held_m[vi] != m) s.held[vi] = 0, s.held_m[vi] = (uint8_t)m;   // another machine: its edits go
  for (int k = 0; k < np && k < 16; ++k) {
    int rv = (int)p[k];
    uint16_t bit = (uint16_t)(1u << k);
    if (s.held[vi] & bit) {
      if (s.rytm_seen[vi] && rv != s.rytm_v[vi][k]) s.held[vi] &= (uint16_t)~bit;   // the Rytm's value moved: it wins
      else p[k] = s.held_v[vi][k];
    }
    s.rytm_v[vi][k] = (uint8_t)(rv < 0 ? 0 : rv > 127 ? 127 : rv);
  }
  s.rytm_seen[vi] = true;
}

#if CORE_UI_TOOLS   // the renderer and the benchmark: the Mac build only (core/Makefile)
namespace {
class DemoSource : public Source {
 public:
  uint8_t v[16] = {};
  int8_t pk[2 * 64];
  uint8_t sl[3] = {32, 64, 96};
  DemoSource() {   // a decaying two-tone burst, then a second hit: something to see
    for (int i = 0; i < 64; ++i) {
      int env = i < 32 ? 127 - i * 3 : 127 - (i - 32) * 4;
      if (env < 4) env = 4;
      int a = env * (sin256(i * 37) + 127) / 254;
      pk[2 * i] = (int8_t)-a, pk[2 * i + 1] = (int8_t)(a * 3 / 4 + 4 > 127 ? 127 : a * 3 / 4 + 4);
    }
  }
  int param(int k) const override { return v[k & 15] & 0x7F; }
  int peaks(int, const int8_t** p, const uint8_t** s, int* ns) const override {
    *p = pk, *s = sl, *ns = 3;
    return 64;
  }
};
DemoSource demo;
PageView render_view;

const Page* page_of(int which, int voice, const Source** src) {
  if (which == 1) { vsrc.vi = voice & 7; *src = &vsrc; return page_for_voice(machine_of_state(voice & 7), voice & 7); }
  if (which == 2 || which == 3) { *src = &demo; return &kDemoPages[which - 2]; }
  return nullptr;
}
}  // namespace

CORE_COLD bool remote_render(int which, int voice, const uint8_t* vals, Canvas& c) {
  if (vals) memcpy(demo.v, vals, 16);
  if (which == 5) { splash_draw(c, vals ? vals[0] * 25u : 1000u); return true; }   // the splash at vals[0] x 25 ms
  if (which >= 6) {   // 6 + k: extra page k (extra.h: LFO, REC, SCOPE), its live part as it is now
    if (which - 6 >= extra_pages()) return false;
    extra_page(which - 6).draw(c, 2 + kVoices + (which - 6), 2 + kVoices + extra_pages(), now_ms() | 1);
    return true;
  }
  if (which == 0 || which == 4) { ssrc.draw(c, which ? 1 : 0, which ? 1 : 0, 2); return true; }
  const Source* src = nullptr;
  const Page* p = page_of(which, voice, &src);
  if (!p) return false;
  char right[4] = "";
  if (which == 1) sniprintf(right, sizeof right, "%s", voice_label(voice));
  render_view.invalidate();
  render_view.draw(c, *p, *src, right, which >= 2 ? which - 2 : which, 2);
  if (which == 1) {   // the bottom bar as the host draws it on that page (encoder A)
    Canvas keep = work;
    work = c;
    voice_footer(*p, voice_label(voice & 7), *src, 0);
    c = work;
    work = keep;
  }
  return true;
}

CORE_COLD void remote_bench(int which, int voice, int n, float* full_us, float* one_us) {
  static Canvas a, b;
  *full_us = *one_us = 0;
  if (n <= 0) return;
  uint32_t m[4];
  uint64_t tm;
  if (which >= 6 && which - 6 < extra_pages()) {   // an extra page: drawn whole each frame
    uint32_t t0 = cycles();
    for (int i = 0; i < n; ++i) extra_page(which - 6).draw(a, 2, 4, now_ms() | 1), a.diff_tiles(b, m), a.diff_text(b, &tm);
    *full_us = *one_us = (float)(cycles() - t0) * (1e6f / (float)cycles_per_second()) / n;
    return;
  }
  if (which == 0 || which == 4) {
    uint32_t t0 = cycles();
    for (int i = 0; i < n; ++i) ssrc.draw(a, which ? 1 : 0, 0, 2), a.diff_tiles(b, m), a.diff_text(b, &tm);
    *full_us = *one_us = (float)(cycles() - t0) * (1e6f / (float)cycles_per_second()) / n;
    return;
  }
  const Source* src = nullptr;
  const Page* p = page_of(which, voice, &src);
  if (!p) return;
  uint32_t t0 = cycles();
  for (int i = 0; i < n; ++i) {
    render_view.invalidate();
    render_view.draw(a, *p, *src, "V1", 1, 2);
    a.diff_tiles(b, m);
    a.diff_text(b, &tm);
  }
  uint32_t t1 = cycles();
  // one change a round: the demo's / the voice's first encoder's parameter moved by one, then back
  int k = p->enc[0] == kNone ? 0 : p->enc[0];
  for (int i = 0; i < n; ++i) {
    if (which >= 2) demo.v[k] = (uint8_t)((demo.v[k] + 1) & 0x7F);
    else if (which == 1) v[voice & 7].p[k] = (float)(((int)v[voice & 7].p[k] + 1) & 0x7F);
    render_view.draw(a, *p, *src, "V1", 1, 2);
    a.diff_tiles(b, m);
    a.diff_text(b, &tm);
    b = a;
  }
  uint32_t t2 = cycles();
  float us = 1e6f / (float)cycles_per_second();
  *full_us = (float)(t1 - t0) * us / n;
  *one_us = (float)(t2 - t1) * us / n;
}

#endif  // CORE_UI_TOOLS

CORE_COLD const RemoteStats& remote_stats() { return s.stats; }
CORE_COLD const uint8_t* remote_frame() { return sent.fb; }
CORE_COLD void preview_sent(uint8_t out[kBytes]) { preview(sent, out); }
CORE_COLD int sent_texts(uint8_t* out, int max) {
  int n = 0;
  for (int i = 0; i < kTextSlots && n < max; ++i) {
    const TextItem& t = sent.txt[i];
    if (!t.s[0]) continue;
    uint8_t* o = out + 36 * n++;
    o[0] = t.x, o[1] = t.y, o[2] = t.font, o[3] = t.flags;
    memset(o + 4, 0, 32);
    memcpy(o + 4, t.s, strlen(t.s));
  }
  return n;
}

CORE_COLD void remote_forget_voices() { memset(reported, 0, sizeof reported); }
CORE_COLD int remote_track_machine(int t) { return have_tracks && t >= 0 && t < 12 ? track_vb[t] : -1; }

CORE_COLD void remote_reset() {
  s = State();
  memset(reported, 0, sizeof reported);
  have_tracks = false;
  src_capable = false;
  view.invalidate();
  work.clear();
  sent.clear();
}

}  // namespace ui
}  // namespace core
