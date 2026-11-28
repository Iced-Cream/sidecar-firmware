// core/pidisplay/pidisplay.cpp - see pidisplay.h. The modal follows core/screen's (the same box, the client's text under it
// blanked); the idle frame follows the connect splash's lines.
#include "pidisplay/pidisplay.h"

#include <string.h>

#include "platform.h"
#include "version.h"

namespace core {
namespace pidisplay {
using namespace ui;

namespace {
constexpr uint32_t kPopupMs = 800, kPopupMaxMs = 30000;
constexpr int kModalX = 24, kModalY = 16, kModalW = 80, kModalH = 32;
constexpr int kSlotModal = kTextSlots - 2;
constexpr int kTile = 8;
const char* const kLabel[kKeys] = {"CLOSE", "LEFT", "UP", "DOWN", "RIGHT", "NO", "YES"};

struct Entry {
  int slot;
  const screen::Client* c;
};

struct S {
  bool have;
  Panel p;
  Layout l;
  Entry e[kDepth];
  int n;
  bool shown_valid, dirty, bar_dirty, clock_set;
  uint32_t last_tick, last_draw;
  bool popup, popup_clock;
  uint32_t popup_t0, popup_ms;
  char popup_l1[kTextLen + 1], popup_l2[kTextLen + 1];
  bool finger, swallow;
  int pressed = -1;                // the soft key held, -1 none
  const screen::Client* press_c;   // the client that took its press, and the code it got
  int press_slot, press_code;
  char ip[20];
  KeyMap map;
  Stats st;
};
S s;
Canvas work, view, shown;
uint16_t g_tile[kTile * kMaxScale * kTile * kMaxScale];
uint16_t g_fill[64 * 64];

void put(int x, int y, int w, int h, const uint16_t* px, int stride) {
  if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > s.p.w || y + h > s.p.h) return;
  s.p.put(s.p.ctx, x, y, w, h, px, stride);
}
// a rectangle of one colour, in chunks of the fill buffer
void fill_rect(int x, int y, int w, int h, uint16_t col) {
  if (w <= 0 || h <= 0) return;
  int cw = w < 64 ? w : 64, ch = h < 64 ? h : 64;
  for (int i = 0; i < cw * ch; ++i) g_fill[i] = col;
  for (int yy = y; yy < y + h; yy += ch)
    for (int xx = x; xx < x + w; xx += cw) {
      int ww = x + w - xx < cw ? x + w - xx : cw, hh = y + h - yy < ch ? y + h - yy : ch;
      put(xx, yy, ww, hh, g_fill, cw);
    }
}

void compute_layout() {
  Layout& l = s.l;
  int sc = s.p.w / kW, sv = (s.p.h - kMinBar) / kH;
  l.scale = sc < sv ? sc : sv;
  if (l.scale > kMaxScale) l.scale = kMaxScale;
  if (l.scale < 1) { l = Layout{}; return; }
  int cw = kW * l.scale, ch = kH * l.scale;
  int cap = s.p.h / 5 > kMinBar ? s.p.h / 5 : kMinBar;   // a tall panel: the bar kept to a fifth, the canvas centred above
  l.bar_h = s.p.h - ch > cap ? cap : s.p.h - ch;
  l.bar_y = s.p.h - l.bar_h;
  l.cx = (s.p.w - cw) / 2;
  l.cy = (l.bar_y - ch) / 2;
  l.key_w = s.p.w / kKeys;
}

// the labels' scale: kMedium at the largest whole scale at which every label fits its key (one size across the bar)
int label_scale() {
  const Layout& l = s.l;
  int ls = (l.bar_h - 12) / (2 * kMedium.h);
  if (ls > 4) ls = 4;
  for (int k = 0; k < kKeys; ++k)
    while (ls > 1 && text_width(kMedium, kLabel[k]) * ls > l.key_w - 12) --ls;
  return ls < 1 ? 1 : ls;
}
void draw_key(int k, int ls) {
  const Layout& l = s.l;
  int x = k * l.key_w, w = k == kKeys - 1 ? s.p.w - x : l.key_w;
  bool down = s.pressed == k;
  fill_rect(x, l.bar_y, w, l.bar_h, kKeyEdge);
  fill_rect(x + 2, l.bar_y + 2, w - 4, l.bar_h - 4, down ? kKeyDown : kKeyBg);
  // the label, its pixels as blocks
  static Canvas lab;
  lab.clear();
  text(lab, kMedium, 0, 0, kLabel[k]);
  int tw = text_width(kMedium, kLabel[k]), th = kMedium.h;
  int ox = x + (w - tw * ls) / 2, oy = l.bar_y + (l.bar_h - th * ls) / 2;
  uint16_t ink = s.n ? kInk : kKeyOff;
  for (int yy = 0; yy < th; ++yy)
    for (int xx = 0; xx < tw; ++xx)
      if (lab.get(xx, yy)) fill_rect(ox + xx * ls, oy + yy * ls, ls, ls, ink);
}
void draw_bar() {
  int ls = label_scale();
  for (int k = 0; k < kKeys; ++k) draw_key(k, ls);
  s.bar_dirty = false;
}

void idle_draw(Canvas& c) {
  c.put_text(0, kRSmall, 2, 1, "SIDECAR");
  c.hline(0, kW - 1, 8);
  c.put_text(1, kRSmall, 64, 24, "NO PAGE OPEN", kAlignCentre);
  if (s.ip[0]) c.put_text(2, kRSmall, 64, 36, s.ip, kAlignCentre);
  char b[kTextLen + 1];
  sniprintf(b, sizeof b, "ENGINE %s", SIDECAR_VERSION);
  c.put_text(3, kRSmall, 64, 46, b, kAlignCentre);
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

// tile bit t of diff_tiles: column t % 16, page t / 16 = glass rows 56 - 8 page .. 63 - 8 page
void put_tile(int t) {
  const Layout& l = s.l;
  int x0 = (t % kTileCols) * kTile, y0 = (kTilePages - 1 - t / kTileCols) * kTile, sc = l.scale, w = kTile * sc;
  for (int yy = 0; yy < kTile; ++yy)
    for (int xx = 0; xx < kTile; ++xx) {
      uint16_t col = view.get(x0 + xx, y0 + yy) ? kInk : kPaper;
      uint16_t* d = g_tile + yy * sc * w + xx * sc;
      for (int a = 0; a < sc; ++a, d += w)
        for (int b = 0; b < sc; ++b) d[b] = col;
    }
  put(l.cx + x0 * sc, l.cy + y0 * sc, w, w, g_tile, w);
}

void frame(uint32_t ms) {
  work.clear();
  work.clear_text();
  if (s.n) {
    const screen::Client* c = s.e[s.n - 1].c;
    if (c->draw) c->draw(work, ms);
  } else {
    idle_draw(work);
  }
  if (s.popup) modal_draw(work);
  uint8_t fb[kBytes];
  preview(work, fb);
  memcpy(view.fb, fb, kBytes);
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  int tiles = s.shown_valid ? view.diff_tiles(shown, mask) : kTiles;
  if (!s.shown_valid) s.st.full++;
  for (int t = 0; t < kTiles; ++t)
    if (mask[t >> 5] >> (t & 31) & 1) put_tile(t);
  if (tiles) s.st.frames++;
  s.st.tiles += (uint32_t)tiles;
  memcpy(shown.fb, view.fb, kBytes);
  s.shown_valid = true;
}

int find(int slot, const screen::Client* c) {
  for (int i = 0; i < s.n; ++i)
    if (s.e[i].c == c && s.e[i].slot == slot) return i;
  return -1;
}
// the top changed: the new top gets opened (true) and a whole frame
void became_top() {
  s.shown_valid = false, s.dirty = true;
  if (s.n) {
    const screen::Client* c = s.e[s.n - 1].c;
    if (c->opened) c->opened(true, 0);
  }
}
}  // namespace

bool attach(const Panel* p) {
  s.have = false;
  if (!p || !p->put) { s.l = Layout{}; return p == nullptr; }
  s.p = *p;
  compute_layout();
  if (s.l.scale < 1) return false;
  s.have = true;
  s.shown_valid = false, s.dirty = true, s.bar_dirty = true, s.clock_set = false;
  fill_rect(0, 0, s.p.w, s.p.h, kPaper);
  return true;
}
bool attached() { return s.have; }
Layout layout() { return s.have ? s.l : Layout{}; }

bool push(int slot, const screen::Client* c) {
  if (!c) return false;
  int i = find(slot, c);
  if (i == s.n - 1 && i >= 0) { s.dirty = true; return true; }   // already on top
  if (i < 0 && s.n == kDepth) return false;
  if (i >= 0) {
    for (; i < s.n - 1; ++i) s.e[i] = s.e[i + 1];
    --s.n;
  }
  if (!s.n) s.bar_dirty = true;
  s.e[s.n++] = Entry{slot, c};
  s.st.pushes++;
  became_top();
  return true;
}
bool close(int slot, const screen::Client* c) {
  int i = find(slot, c);
  if (i < 0) return false;
  bool was_top = i == s.n - 1;
  for (; i < s.n - 1; ++i) s.e[i] = s.e[i + 1];
  --s.n;
  s.st.closes++;
  if (s.press_c == c && s.press_slot == slot) s.press_c = nullptr;
  if (!s.n) s.bar_dirty = true;
  if (c->opened) c->opened(false, 0);
  if (was_top) became_top();
  return true;
}
bool close_top() { return s.n ? close(s.e[s.n - 1].slot, s.e[s.n - 1].c) : false; }
void clear() {
  while (s.n) close_top();
}
int depth() { return s.n; }
const screen::Client* top(int* slot) {
  if (slot) *slot = s.n ? s.e[s.n - 1].slot : kPiWide;
  return s.n ? s.e[s.n - 1].c : nullptr;
}
bool on_stack(int slot, const screen::Client* c) { return find(slot, c) >= 0; }
void redraw() { s.dirty = true; }

bool popup(const char* l1, const char* l2, uint32_t hold_ms) {
  sniprintf(s.popup_l1, sizeof s.popup_l1, "%s", l1 ? l1 : "");
  sniprintf(s.popup_l2, sizeof s.popup_l2, "%s", l2 ? l2 : "");
  s.popup_ms = hold_ms > kPopupMaxMs ? kPopupMaxMs : hold_ms;
  s.popup = true, s.popup_clock = false, s.dirty = true;
  s.st.popups++;
  return true;
}
void popup_end() {
  if (!s.popup) return;
  s.popup = false, s.dirty = true;
}
bool popup_up() { return s.popup; }

void set_keymap(KeyMap m) { s.map = m; }
int rytm_keys(int slot, Key k) {
  switch (k) {
    case kLeft: return 0x40;
    case kRight: return 0x41;
    case kUp: return 0x42;
    case kDown: return 0x43;
    case kYes: return 0x44;
    case kNo: return 0x45;
    default: return -1;
  }
}
int key_at(int x, int y) {
  if (!s.have || y < s.l.bar_y || y >= s.p.h || x < 0 || x >= s.p.w) return -1;
  int k = x / s.l.key_w;
  return k >= kKeys ? kKeys - 1 : k;
}

void touch(int x, int y, bool down) {
  if (down == s.finger) return;   // a move: the key pressed stays the one under the finger's first touch
  s.finger = down;
  if (down) {
    s.st.touches++;
    if (s.popup) {   // the touch only ends the popup
      popup_end();
      s.swallow = true;
      return;
    }
    int k = key_at(x, y);
    if (k < 0) return;
    s.pressed = k, s.bar_dirty = true;
    if (k == kClose || !s.n) return;
    const Entry& e = s.e[s.n - 1];
    int code = (s.map ? s.map : rytm_keys)(e.slot, (Key)k);
    if (code < 0 || !e.c->key) return;
    s.press_c = e.c, s.press_slot = e.slot, s.press_code = code;
    s.st.keys++;
    e.c->key(code, true);
    return;
  }
  if (s.swallow) { s.swallow = false; return; }
  if (s.pressed < 0) return;
  int k = s.pressed;
  s.pressed = -1, s.bar_dirty = true;
  if (k == kClose) { close_top(); return; }
  // the release reaches the client that took the press only while it is still on top (a covered client gets no keys)
  const screen::Client* c = s.press_c;
  s.press_c = nullptr;
  if (c && s.n && s.e[s.n - 1].c == c && s.e[s.n - 1].slot == s.press_slot) c->key(s.press_code, false);
}

void set_address(const char* ip) {
  sniprintf(s.ip, sizeof s.ip, "%s", ip ? ip : "");
  if (!s.n) s.dirty = true;
}

void tick(uint32_t ms) {
  if (!s.have) return;
  if (s.clock_set && ms - s.last_tick < kTickMs) return;
  bool first = !s.clock_set;
  s.clock_set = true, s.last_tick = ms;
  if (s.popup) {
    if (!s.popup_clock) s.popup_clock = true, s.popup_t0 = ms;
    else if (ms - s.popup_t0 >= (s.popup_ms ? s.popup_ms : kPopupMs)) s.popup = false, s.dirty = true;
  }
  if (s.bar_dirty) draw_bar();
  if (!s.dirty && s.shown_valid && !first && ms - s.last_draw < kPollMs) return;
  s.dirty = false, s.last_draw = ms;
  frame(ms);
}

Stats stats() { return s.st; }
void reset() {
  s = S{};
  s.pressed = -1;
  s.map = rytm_keys;
  shown.clear();
}
}  // namespace pidisplay
}  // namespace core
