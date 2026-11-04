// core/screen/screen.cpp - see screen.h. The frame transport (tiles, text slots, commit), the splash, the popup and the
// pairing modal follow core/ui/remote.cpp's, which they were taken from.
#include "screen/screen.h"

#include <string.h>

#include "hooks.h"
#include "platform.h"
#include "version.h"

namespace core {
namespace screen {
using namespace ui;

namespace {
enum : uint8_t {
  kMode = 0x30, kKey = 0x31, kEnc = 0x32, kMask = 0x37, kRect = 0x38, kCommit = 0x39, kOpen = 0x3A, kAlive = 0x3C,
  kText = 0x3D, kTextClear = 0x3E, kHello = 0x35, kSplash = 0x36,
};
constexpr uint8_t kCatchKeys[] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x84,   // LEFT RIGHT UP DOWN YES NO PAGE
                                  0x30, 0x31, 0x32, 0x33, 0x34, 0x35,         // TRIG SRC SMPL FLTR AMP LFO
                                  0x10, 0xB0, 0xB4, 0xB5, 0xB1, 0x81, 0x82,   // FX GLOBAL SAMPLER PLAYMODE QPER SONG CHAIN
                                  0x86};                                      // TRK
constexpr uint8_t kKeyPushA = 0x70;
constexpr uint32_t kTickMs = 25, kAliveMs = 250;
constexpr uint32_t kSplashMs = 3000, kSplashStepMs = 100;
constexpr int kSplashMinUp = 60;   // 3 s of the Rytm's 50 ms ticks
constexpr uint32_t kHelloOpenMs = 600;
constexpr uint8_t kSplashTicks = 64;
constexpr uint32_t kPopupMs = 800, kSplashTicksMax = 127, kPopupWindowMs = 6000, kPopupAliveMs = 500;
constexpr int kModalX = 24, kModalY = 16, kModalW = 80, kModalH = 32;
constexpr int kSlotModal = kTextSlots - 2, kSlotFree = 32;
constexpr RFont kRBold = {7, 10, 0, 7};

struct S {
  const Client* client;
  bool open, sent_valid, mask_valid, dirty, clock_set;
  uint32_t open_ms, now_ms, last_draw, last_tx;
  int caps;
  bool caps36, greeted;
  bool splash, splash_clock;
  uint32_t splash_t0;
  bool popup, popup_over, popup_clock, modal_shown;
  int modal_key = -1;
  uint32_t popup_t0, popup_ms, popup_36, popup_last, popups, status_popups;
  char popup_l1[kTextLen + 1], popup_l2[kTextLen + 1];
  char ip[20];
  uint8_t seq;
  uint8_t mask[kMaskBytes];
  Stats st;
};
S s = {};
Canvas work, sent;

void send(const uint8_t* m, int n) {
  to_rytm(m, n);
  s.st.bytes += (uint32_t)n;
}
int head(uint8_t* m, uint8_t cmd) {
  m[0] = 0xF0, m[1] = 0x7D, m[2] = 0x52, m[3] = 0x54, m[4] = cmd, m[5] = 1;
  return 6;
}
void send_short(uint8_t cmd, const uint8_t* b, int nb) {
  uint8_t m[8 + kMaskBytes];
  int n = head(m, cmd);
  for (int i = 0; i < nb; ++i) m[n++] = b[i] & 0x7F;
  m[n++] = 0xF7;
  send(m, n);
}
// 38: tile columns c0..c0+nc-1 of framebuffer page p, column-major, 7 -> 8 packed
void send_rect(int c0, int p, int nc) {
  uint8_t raw[8 * kTileCols], m[16 + 8 * kTileCols + kTileCols * 8 / 7 + 2];
  int nr = 0;
  for (int x = c0 * 8; x < (c0 + nc) * 8; ++x) raw[nr++] = work.fb[p + 8 * x];
  int n = head(m, kRect);
  m[n++] = s.seq, m[n++] = (uint8_t)c0, m[n++] = (uint8_t)p, m[n++] = (uint8_t)nc, m[n++] = 1;
  for (int i = 0; i < nr; i += 7) {
    int g = nr - i < 7 ? nr - i : 7;
    uint8_t hi = 0;
    for (int j = 0; j < g; ++j) hi |= (uint8_t)((raw[i + j] >> 7) << j);
    m[n++] = hi;
    for (int j = 0; j < g; ++j) m[n++] = raw[i + j] & 0x7F;
  }
  m[n++] = 0xF7;
  send(m, n);
}
void send_text(int i) {
  const TextItem& t = work.txt[i];
  uint8_t m[16 + kTextLen];
  int n = head(m, kText);
  m[n++] = s.seq, m[n++] = (uint8_t)i, m[n++] = t.x & 0x7F, m[n++] = t.y & 0x3F, m[n++] = t.font & 0x7F;
  m[n++] = t.flags & 0x7F;
  for (const char* c = t.s; *c; ++c) m[n++] = (uint8_t)*c & 0x7F;
  m[n++] = 0xF7;
  send(m, n);
}
void send_part(const uint32_t tiles[4], uint64_t texts, bool clear_first) {
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
    }
  for (int i = 0; i < kTextSlots; ++i)
    if (texts >> i & 1) send_text(i);
  uint8_t b = s.seq;
  send_short(kCommit, &b, 1);
  s.st.frames++;
}
// work -> the Rytm: the changed tiles and text slots (all of them when the Rytm's shadow is unknown), one commit
void flush(uint32_t ms) {
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint64_t texts = 0;
  int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
  int ntext = s.sent_valid ? work.diff_text(sent, &texts) : 0;
  if (!s.sent_valid)
    for (int i = 0; i < kTextSlots; ++i)
      if (work.txt[i].s[0]) texts |= 1ull << i, ++ntext;
  if (tiles || ntext) {
    send_part(mask, texts, !s.sent_valid);
    s.last_tx = ms;
    s.st.tiles += (uint32_t)tiles;
  }
  sent = work;
  s.sent_valid = true;
}
void send_mask() {
  uint8_t b[kMaskBytes] = {};
  if (s.client && s.client->mask) {
    memcpy(b, s.client->mask, sizeof b);
  } else {
    for (uint8_t k : kCatchKeys) b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
    for (int e = 0; e < 9; ++e) {   // A..H and LEVEL, with their pushes
      int k = kKeyPushA + e;
      b[e / 7] |= (uint8_t)(1u << (e % 7));
      b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
    }
  }
  if (s.mask_valid && !memcmp(b, s.mask, sizeof b)) return;
  int n = sizeof b;
  while (n > 3 && !b[n - 1]) --n;
  send_short(kMask, b, n);
  memcpy(s.mask, b, sizeof b);
  s.mask_valid = true;
}

void title(Canvas& c) {
  c.put_text(0, kRSmall, 2, 1, "SIDECAR");
  c.hline(0, kW - 1, 8);
}
void splash_draw(Canvas& c, uint32_t t) {
  c.clear();
  title(c);
  c.put_text(kSlotFree, kRBold, 64, 19, "CONNECTED", kAlignCentre);
  char b[kTextLen + 1];
  sniprintf(b, sizeof b, "%s", s.ip[0] ? s.ip : "USB AUDIO+MIDI");
  c.put_text(kSlotFree + 1, kRSmall, 64, 36, b, kAlignCentre);
  sniprintf(b, sizeof b, "ENGINE %s", SIDECAR_VERSION);
  c.put_text(kSlotFree + 2, kRSmall, 64, 44, b, kAlignCentre);
  int w = t >= kSplashMs ? 0 : (int)((kSplashMs - t) * 124 / kSplashMs);
  if (w > 0) c.hline(2, 2 + w - 1, 56);
}
void splash_tick(uint32_t ms) {
  bool first = !s.splash_clock;
  if (first) s.splash_clock = true, s.splash_t0 = ms, s.last_draw = ms - kSplashStepMs;
  uint32_t t = ms - s.splash_t0;
  if (t >= kSplashMs) {
    uint8_t b = 0;
    send_short(kSplash, &b, 1);
    s.splash = false;
    s.sent_valid = false;
    return;
  }
  if (!first && ms - s.last_draw < kSplashStepMs) return;
  s.last_draw = ms;
  splash_draw(work, t);
  flush(ms);
  if (first) {
    uint8_t b = kSplashTicks;
    send_short(kSplash, &b, 1);
    s.st.splashes++;
  }
}
void popup_draw(Canvas& c) {
  c.clear();
  title(c);
  c.rect(Rect{4, 16, 120, 32}, kOn);
  c.rect(Rect{6, 18, 116, 28}, kOn);
  c.put_text(kSlotFree, kRSmall, 64, s.popup_l2[0] ? 23 : 29, s.popup_l1, kAlignCentre);
  if (s.popup_l2[0]) c.put_text(kSlotFree + 1, kRSmall, 64, 34, s.popup_l2, kAlignCentre);
}
void modal_draw(Canvas& c) {
  for (int i = 0; i < kTextSlots; ++i) {   // the client's text under the box: blanked while it is up
    if (!c.txt[i].s[0] || i == kSlotModal || i == kSlotModal + 1) continue;
    int y = c.txt[i].y;
    if (y + 6 <= kModalY || y >= kModalY + kModalH) continue;
    int w = (int)strlen(c.txt[i].s) * 5, x = text_left(c.txt[i], w);
    if (x + w <= kModalX || x >= kModalX + kModalW) continue;
    c.put_text(i, kRSmall, 0, 0, nullptr);
  }
  c.fill(Rect{kModalX + 1, kModalY + 1, kModalW - 2, kModalH - 2}, kOff);
  c.rect(Rect{kModalX, kModalY, kModalW, kModalH}, kOn);
  c.rect(Rect{kModalX + 2, kModalY + 2, kModalW - 4, kModalH - 4}, kOn);
  c.put_text(kSlotModal, kRSmall, 64, s.popup_l2[0] ? 23 : 29, s.popup_l1, kAlignCentre);
  c.put_text(kSlotModal + 1, kRSmall, 64, 34, s.popup_l2[0] ? s.popup_l2 : nullptr, kAlignCentre);
}
bool modal_tick(uint32_t ms) {
  if (!s.popup || !s.popup_over) return false;
  if (!s.popup_clock) s.popup_clock = true, s.popup_t0 = ms, ++s.popups;
  if (ms - s.popup_t0 < (s.popup_ms ? s.popup_ms : kPopupMs)) return true;
  s.popup = false, s.popup_clock = false;
  return false;
}
void popup_hold(uint32_t ms, uint32_t left) {
  uint32_t ticks = left / 50 + 4;
  uint8_t b = (uint8_t)(ticks < kSplashTicksMax ? ticks : kSplashTicksMax);
  send_short(kSplash, &b, 1);
  s.popup_36 = ms, s.last_tx = ms;
}
void popup_key_closed() {
  if (s.open || !s.popup || !s.popup_over || !s.popup_clock) return;
  s.popup = false, s.popup_clock = false;
  s.sent_valid = false;
}
void popup_tick(uint32_t ms) {
  const uint32_t hold = s.popup_ms ? s.popup_ms : kPopupMs;
  if (!s.popup_clock) {
    s.popup_clock = true, s.popup_t0 = ms;
    popup_draw(work);
    s.sent_valid = false;
    flush(ms);
    popup_hold(ms, hold);
    ++s.popups;
    if (!s.popup_over) ++s.status_popups;
    return;
  }
  if (ms - s.popup_t0 < hold) {
    if (ms - s.popup_36 >= kPopupWindowMs) popup_hold(ms, hold - (ms - s.popup_t0));
    else if (ms - s.last_tx >= kPopupAliveMs) send_short(kAlive, nullptr, 0), s.last_tx = ms;
    return;
  }
  uint8_t b = 0;
  send_short(kSplash, &b, 1);
  s.popup = false;
  s.sent_valid = false;
}
void frame(uint32_t ms) {
  work.clear();
  work.clear_text();
  if (s.client && s.client->draw) s.client->draw(work, ms);
  bool on = modal_tick(ms);
  if (on) modal_draw(work);
  s.modal_shown = on;
  send_mask();
  flush(ms);
}
void close_page() {
  if (s.open) s.st.closes++;
  s.open = false;
  if (s.client && s.client->opened) s.client->opened(false, 0);
}
}  // namespace

void set_client(const Client* c) {
  s.client = c;
  s.dirty = true, s.mask_valid = false;
}
const Client* client() { return s.client; }
void request(bool on) {
  uint8_t b[2] = {0, (uint8_t)(on ? 1 : 0)};
  send_short(kOpen, b, 2);
}
void redraw() { s.dirty = true; }
bool open() { return s.open; }
int caps() { return s.caps; }
uint32_t hellos() { return s.st.hellos; }
bool greeted() { return s.greeted; }
void set_address(const char* ip) { sniprintf(s.ip, sizeof s.ip, "%s", ip ? ip : ""); }

bool popup(const char* l1, const char* l2, uint32_t hold_ms, bool over) {
  if (!s.caps36) return false;
  if (!over && (s.open || s.splash)) return false;
  if (!over && s.popup && s.popup_over) return false;
  uint32_t ms = now_ms();
  if (s.popup && s.popups && !strcmp(s.popup_l1, l1 ? l1 : "") && ms - s.popup_last < 2000) return true;
  sniprintf(s.popup_l1, sizeof s.popup_l1, "%s", l1 ? l1 : "");
  sniprintf(s.popup_l2, sizeof s.popup_l2, "%s", l2 ? l2 : "");
  s.popup_ms = hold_ms > kPopupMaxMs ? kPopupMaxMs : hold_ms;
  s.popup = true, s.popup_over = over, s.popup_clock = false, s.popup_last = ms;
  return true;
}
void popup_end() {
  if (!s.popup) return;
  if (!s.popup_clock) { s.popup = false; return; }
  s.popup_ms = 1;
  s.popup_t0 = now_ms() - 2;
}
uint32_t popups() { return s.popups; }
uint32_t status_popups() { return s.status_popups; }

bool sysex(const uint8_t* d, int len) {
  if (len < 6 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54 || d[4] < 0x30 || d[4] > 0x3F) return false;
  if (len < 7 || d[5] != 1 || d[len - 1] != 0xF7) { s.st.bad++; return true; }
  const uint8_t* b = d + 6;
  int n = len - 7;
  switch (d[4]) {
    case kMode:
      if (n < 3) break;
      if (b[1]) {
        if (!s.client) {   // no client: nothing to show, the Rytm's stock screen back
          request(false);
          return true;
        }
        if (!s.open) s.st.opens++;
        s.open = true, s.open_ms = s.now_ms;
        s.sent_valid = false, s.mask_valid = false, s.clock_set = false, s.dirty = true;
        s.splash = false, s.popup = s.popup && s.popup_over, s.popup_clock = false;
        if (s.client->opened) s.client->opened(true, b[0]);
      } else {
        close_page();
      }
      return true;
    case kKey:
      if (n < 3) break;
      s.st.keys++;
      if (b[2]) popup_key_closed();
      if (b[2] && s.open && s.popup && s.popup_over && s.modal_shown) {   // the modal over a page: a key takes it off
        s.modal_key = b[0] << 7 | b[1];
        popup_end();
        s.dirty = true;
        return true;
      }
      if (!b[2] && s.modal_key == (b[0] << 7 | b[1])) {
        s.modal_key = -1;
        return true;
      }
      if (s.open && s.client && s.client->key) s.client->key(b[0] << 7 | b[1], b[2] != 0), s.dirty = true;
      return true;
    case kEnc:
      if (n < 3) break;
      s.st.encs++;
      popup_key_closed();
      if (s.open && s.client && s.client->enc) s.client->enc(b[0], (int)b[1] - 64, b[2] != 0), s.dirty = true;
      return true;
    case kHello: {
      if (n < 3) break;
      s.st.hellos++;
      s.greeted = true;
      int up = b[0] << 7 | b[1];
      s.caps36 = (b[2] & 1) != 0, s.caps = b[2];
      if (s.open && s.now_ms - s.open_ms <= kHelloOpenMs) {   // the user opened the page just before this hello: kept
        s.sent_valid = false, s.mask_valid = false, s.clock_set = false;
        log("rui: hello, Rytm up %d.%d s, caps %d, page opened %d ms ago: kept", up / 20, (up % 20) / 2, b[2],
            (int)(s.now_ms - s.open_ms));
        return true;
      }
      if (s.open) close_page();
      s.sent_valid = false, s.mask_valid = false, s.splash = false, s.clock_set = false;
      if (!(s.popup && s.popup_over && s.caps36)) s.popup = false;   // the pairing code's modal is owed on this link too
      s.popup_clock = false;
      bool splash = (b[2] & 1) && up >= kSplashMinUp;
      log("rui: hello, Rytm up %d.%d s, caps %d: %s", up / 20, (up % 20) / 2, b[2], splash ? "splash" : "alive");
      if (splash) s.splash = true, s.splash_clock = false;
      else send_short(kAlive, nullptr, 0);
      return true;
    }
    default:
      return true;   // 33, 34, 36..3F: the host's own or not ours to act on
  }
  s.st.bad++;
  return true;
}

void tick(uint32_t ms) {
  s.now_ms = ms;
  if (s.popup && s.popup_over && !s.open) { popup_tick(ms); return; }
  if (s.splash && !s.open) { splash_tick(ms); return; }
  if (s.popup && !s.open) { popup_tick(ms); return; }
  if (!s.open) return;
  if (!s.clock_set) s.clock_set = true, s.last_draw = ms - kTickMs, s.last_tx = ms, s.dirty = true;
  bool modal = s.popup && s.popup_over;
  if ((s.dirty || modal || s.modal_shown) && ms - s.last_draw >= kTickMs) {
    s.last_draw = ms;
    s.dirty = false;
    frame(ms);
  }
  if (ms - s.last_tx >= kAliveMs) {
    send_short(kAlive, nullptr, 0);
    s.last_tx = ms;
    s.st.keepalives++;
  }
}

void resync() {
  s.sent_valid = false, s.mask_valid = false, s.clock_set = false, s.splash_clock = false, s.dirty = true;
  if (s.popup_over) s.popup_clock = false;
}
void reset() {
  const Client* c = s.client;
  s = S{};
  s.client = c;
  s.modal_key = -1;
}
Stats stats() { return s.st; }
}  // namespace screen
}  // namespace core
