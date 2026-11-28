// core/tests/test_pidisplay.cpp - core/pidisplay (S8k, the Pi-attached display) on the host, against a fake panel: the
// layout per panel size, the canvas scaled onto the panel (pixels and changed tiles only), the idle frame, the Pi-wide
// stack's rules (S8h design section 2: latest on top, covered keep state and get no draws or keys, no duplicates, close
// anywhere, closing the top shows the one below with a whole frame, a client may open itself, depth 8), the soft keys
// (to the top client only, CLOSE, the key map), Pi-wide popups over the top. Screenshots: build/pidisplay_*.ppm.
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "check.h"
#include "pidisplay/pidisplay.h"

using namespace core;
using namespace core::ui;
namespace pd = core::pidisplay;

namespace {
struct Fake {
  int w, h;
  std::vector<uint16_t> px;
  int puts = 0;
  uint16_t at(int x, int y) const { return px[(size_t)y * w + x]; }
};
void fake_put(void* ctx, int x, int y, int w, int h, const uint16_t* p, int stride) {
  Fake* f = (Fake*)ctx;
  CHECK(x >= 0 && y >= 0 && x + w <= f->w && y + h <= f->h);
  for (int yy = 0; yy < h; ++yy) memcpy(&f->px[(size_t)(y + yy) * f->w + x], p + (size_t)yy * stride, (size_t)w * 2);
  f->puts++;
}
Fake g_f;
pd::Panel g_panel;
void make_panel(int w, int h) {
  g_f.w = w, g_f.h = h, g_f.px.assign((size_t)w * h, 0x1234), g_f.puts = 0;
  g_panel = pd::Panel{w, h, fake_put, &g_f};
}
void shot(const char* name) {
  char path[96];
  snprintf(path, sizeof path, "build/pidisplay_%s.ppm", name);
  FILE* fp = fopen(path, "wb");
  if (!fp) return;
  fprintf(fp, "P6\n%d %d\n255\n", g_f.w, g_f.h);
  for (uint16_t v : g_f.px) {
    unsigned char rgb[3] = {(unsigned char)((v >> 11) << 3), (unsigned char)(((v >> 5) & 63) << 2), (unsigned char)((v & 31) << 3)};
    fwrite(rgb, 1, 3, fp);
  }
  fclose(fp);
}
// the panel pixel at the centre of canvas pixel (x, y)
uint16_t canvas_px(int x, int y) {
  pd::Layout l = pd::layout();
  return g_f.at(l.cx + x * l.scale + l.scale / 2, l.cy + y * l.scale + l.scale / 2);
}
uint32_t g_ms = 1000;
void run(uint32_t ms = 200) {
  for (uint32_t t = 0; t < ms; t += 5) g_ms += 5, pd::tick(g_ms);
}
void tap(int k) {
  pd::Layout l = pd::layout();
  int x = k * l.key_w + l.key_w / 2, y = l.bar_y + l.bar_h / 2;
  CHECK(pd::key_at(x, y) == k);
  pd::touch(x, y, true);
  pd::touch(x, y, false);
}

// test clients: each counts its calls; draw() puts its mark pixel and its name
struct Rec {
  const char* name;
  int mx, my;
  int draws, opens, closes, keys_down, keys_up, last_key;
};
Rec g_rec[12];
screen::Client g_cl[12];
void (*g_on_yes)(int who) = nullptr;
template <int I> void draw_i(Canvas& c, uint32_t) {
  Rec& r = g_rec[I];
  r.draws++;
  c.pixel(r.mx, r.my);
  c.put_text(5, kRSmall, 64, 20, r.name, kAlignCentre);
}
template <int I> void key_i(int code, bool down) {
  Rec& r = g_rec[I];
  (down ? r.keys_down : r.keys_up)++;
  r.last_key = code;
  if (down && code == 0x44 && g_on_yes) g_on_yes(I);
}
template <int I> void opened_i(bool on, int) { (on ? g_rec[I].opens : g_rec[I].closes)++; }
template <int I> void setup_one() {
  static char nm[8];
  snprintf(nm, sizeof nm, "PAGE %d", I);
  g_rec[I] = Rec{nm, 10 + I, 40, 0, 0, 0, 0, 0, -1};
  g_cl[I] = screen::Client{nm, draw_i<I>, key_i<I>, nullptr, opened_i<I>, nullptr};
}
template <int... I> void setup_all(std::integer_sequence<int, I...>) { (setup_one<I>(), ...); }
void setup() { setup_all(std::make_integer_sequence<int, 12>{}); }
const screen::Client* A = &g_cl[0];
const screen::Client* B = &g_cl[1];
const screen::Client* C = &g_cl[2];

void fresh(int w = 800, int h = 480) {
  pd::reset();
  setup();
  g_on_yes = nullptr;
  make_panel(w, h);
  CHECK(pd::attach(&g_panel));
}
}  // namespace

int main() {
  // ---- layout per panel size
  fresh(800, 480);
  pd::Layout l = pd::layout();
  CHECK(l.scale == 6 && l.cx == 16 && l.cy == 0 && l.bar_y == 384 && l.bar_h == 96 && l.key_w == 114);
  fresh(1024, 600);
  l = pd::layout();
  CHECK(l.scale == 8 && l.cx == 0 && l.bar_y == 512 && l.bar_h == 88);
  fresh(1280, 720);
  CHECK(pd::layout().scale == 10 && pd::layout().bar_h == 80);
  fresh(480, 800);   // a portrait panel: the bar kept to a fifth, the canvas centred above it
  l = pd::layout();
  CHECK(l.scale == 3 && l.bar_h == 160 && l.bar_y == 640 && l.cy == (640 - 192) / 2 && l.cx == 48);
  pd::reset();
  make_panel(160, 90);
  CHECK(!pd::attach(&g_panel) && !pd::attached());   // too small for the canvas and a bar
  CHECK(pd::attach(nullptr) && !pd::attached());

  // ---- no panel: the stack is kept, nothing drawn
  pd::reset();
  setup();
  CHECK(pd::push(0, A) && pd::depth() == 1 && g_rec[0].opens == 1);
  run();
  CHECK(g_rec[0].draws == 0);

  // ---- the idle frame and the bar, at attach
  fresh();
  run(30);
  CHECK(pd::stats().full == 1 && pd::stats().tiles == kTiles);
  CHECK(canvas_px(0, 8) == pd::kInk && canvas_px(0, 7) == pd::kPaper);   // the title rule
  CHECK(g_f.at(0, 0) == pd::kPaper && g_f.at(799, 383) == pd::kPaper);    // the border beside the canvas
  CHECK(g_f.at(0, 384) == pd::kKeyEdge && g_f.at(57, 390) == pd::kKeyBg);
  bool off_label = false;   // the empty stack: the keys' labels dimmed
  for (int y = 384; y < 480; ++y)
    for (int x = 114; x < 228; ++x) off_label |= g_f.at(x, y) == pd::kKeyOff;
  CHECK(off_label);
  shot("idle");

  // ---- a client on top: its frame scaled 6x, the bar's labels lit
  fresh();
  CHECK(pd::push(0, A));
  run(30);
  CHECK(g_rec[0].opens == 1 && g_rec[0].draws >= 1);
  CHECK(canvas_px(10, 40) == pd::kInk && canvas_px(11, 40) == pd::kPaper && canvas_px(10, 41) == pd::kPaper);
  {
    pd::Layout q = pd::layout();
    int x0 = q.cx + 10 * 6, y0 = 40 * 6;
    CHECK(g_f.at(x0, y0) == pd::kInk && g_f.at(x0 + 5, y0 + 5) == pd::kInk && g_f.at(x0 + 6, y0) == pd::kPaper);
  }
  bool lit = false;
  for (int y = 384; y < 480; ++y)
    for (int x = 114; x < 228; ++x) lit |= g_f.at(x, y) == pd::kInk;
  CHECK(lit);
  shot("page");
  // a change of one pixel: one tile out
  uint32_t tiles = pd::stats().tiles;
  g_rec[0].mx = 100;
  pd::redraw();
  run(30);
  CHECK(pd::stats().tiles == tiles + 2);   // the old mark's tile and the new one's
  // polled without redraw(): a client that only changes its drawing is still shown within kPollMs
  g_rec[0].my = 10;
  run(150);
  CHECK(canvas_px(100, 10) == pd::kInk && canvas_px(100, 40) == pd::kPaper);

  // ---- the stack: latest on top, the covered one keeps its state and gets no draws or keys
  fresh();
  pd::push(0, A);
  run(30);
  CHECK(pd::push(1, B) && pd::depth() == 2);
  int slot = 9;
  CHECK(pd::top(&slot) == B && slot == 1);
  int a_draws = g_rec[0].draws;
  run(300);
  CHECK(g_rec[0].draws == a_draws && g_rec[1].draws >= 2);
  CHECK(g_rec[0].opens == 1 && g_rec[0].closes == 0 && g_rec[1].opens == 1);   // covered: no callback
  CHECK(canvas_px(11, 40) == pd::kInk && canvas_px(10, 40) == pd::kPaper);      // B's mark, not A's
  tap(pd::kYes);
  CHECK(g_rec[1].keys_down == 1 && g_rec[1].keys_up == 1 && g_rec[1].last_key == 0x44);
  CHECK(g_rec[0].keys_down == 0 && g_rec[0].keys_up == 0);
  tap(pd::kLeft);
  tap(pd::kRight);
  tap(pd::kUp);
  tap(pd::kDown);
  tap(pd::kNo);
  CHECK(g_rec[1].keys_down == 6 && g_rec[1].last_key == 0x45);
  // asked again while covered: moved to the top, no duplicate, opened (true) again with a whole frame
  uint32_t full = pd::stats().full;
  CHECK(pd::push(0, A) && pd::depth() == 2 && pd::top() == A && g_rec[0].opens == 2);
  run(30);
  CHECK(pd::stats().full == full + 1 && canvas_px(10, 40) == pd::kInk);
  // pushed again while on top: nothing changes
  CHECK(pd::push(0, A) && pd::depth() == 2 && g_rec[0].opens == 2);
  // the same client for the other box is another entry
  CHECK(pd::push(1, A) && pd::depth() == 3 && pd::on_stack(0, A) && pd::on_stack(1, A));
  CHECK(pd::close(1, A) && pd::depth() == 2 && pd::top() == A);
  run(30);
  // close a covered entry: the top is left alone
  int a_opens = g_rec[0].opens;
  full = pd::stats().full;
  CHECK(pd::close(1, B) && pd::depth() == 1 && pd::top() == A && g_rec[1].closes == 1);
  run(30);
  CHECK(g_rec[0].opens == a_opens && pd::stats().full == full);
  CHECK(!pd::close(1, B));   // not on the stack
  // close the top: empty, the idle frame back
  CHECK(pd::close(0, A) && pd::depth() == 0 && g_rec[0].closes == 2 && !pd::top());   // (its slot 1 entry closed once before)
  run(30);
  CHECK(canvas_px(0, 8) == pd::kInk && canvas_px(10, 40) == pd::kPaper);

  // ---- CLOSE: the top closed, the one below shown again with a whole frame and opened (true)
  fresh();
  pd::push(0, A);
  pd::push(1, B);
  run(30);
  full = pd::stats().full;
  tap(pd::kClose);
  CHECK(pd::depth() == 1 && pd::top() == A && g_rec[1].closes == 1 && g_rec[0].opens == 2);
  CHECK(g_rec[1].keys_down == 0);   // CLOSE is not a key for the client
  run(30);
  CHECK(pd::stats().full == full + 1 && canvas_px(10, 40) == pd::kInk && canvas_px(11, 40) == pd::kPaper);
  tap(pd::kClose);
  CHECK(pd::depth() == 0);
  tap(pd::kClose);   // empty: nothing
  tap(pd::kYes);
  CHECK(pd::depth() == 0);

  // ---- a client opens a page itself (Q3): on top at once; the YES release reaches neither (A covered, C took no press)
  fresh();
  pd::push(0, A);
  g_on_yes = [](int who) { if (who == 0) pd::push(0, C); };
  tap(pd::kYes);
  CHECK(pd::top() == C && pd::depth() == 2);
  CHECK(g_rec[0].keys_down == 1 && g_rec[0].keys_up == 0 && g_rec[2].keys_up == 0);

  // ---- depth: at most 8
  fresh();
  for (int i = 0; i < pd::kDepth; ++i) CHECK(pd::push(0, &g_cl[i]));
  CHECK(!pd::push(0, &g_cl[8]) && pd::depth() == pd::kDepth);
  CHECK(pd::push(0, &g_cl[0]) && pd::top() == &g_cl[0] && pd::depth() == pd::kDepth);   // a move needs no room
  pd::clear();
  CHECK(pd::depth() == 0);
  for (int i = 0; i < pd::kDepth; ++i) CHECK(g_rec[i].closes == 1);
  CHECK(!pd::push(0, nullptr));

  // ---- the key map (S8h's per-box tables plug in here)
  fresh();
  pd::set_keymap([](int s, pd::Key k) { return s == 1 ? 0x100 + (int)k : pd::rytm_keys(s, k); });
  pd::push(1, B);
  tap(pd::kYes);
  CHECK(g_rec[1].last_key == 0x100 + pd::kYes);
  pd::push(0, A);
  tap(pd::kYes);
  CHECK(g_rec[0].last_key == 0x44);
  pd::set_keymap([](int, pd::Key k) { return k == pd::kNo ? -1 : 7; });
  int downs = g_rec[0].keys_down;
  tap(pd::kNo);
  CHECK(g_rec[0].keys_down == downs);   // no code for it: nothing sent
  // a touch outside the bar, and a slide off a key: the press stays the first key's
  pd::set_keymap(nullptr);
  pd::touch(400, 100, true);
  pd::touch(400, 100, false);
  CHECK(g_rec[0].keys_down == downs);
  {
    pd::Layout q = pd::layout();
    int y = q.bar_y + 10;
    pd::touch(6 * q.key_w + 5, y, true);    // YES
    pd::touch(5 * q.key_w + 5, y, true);    // moved onto NO: a move
    pd::touch(5 * q.key_w + 5, y, false);
    CHECK(g_rec[0].keys_down == downs + 1 && g_rec[0].keys_up >= 1 && g_rec[0].last_key == 0x44);
  }

  // ---- Pi-wide popups: over the top entry, the client's text under the box blanked, not in the stack
  fresh();
  pd::push(0, A);
  run(30);
  CHECK(pd::popup("SIDECAR PAIR CODE", "1234 5678", 30000));
  run(30);
  CHECK(pd::popup_up() && pd::depth() == 1 && pd::top() == A);
  CHECK(canvas_px(24, 16) == pd::kInk && canvas_px(26, 18) == pd::kInk && canvas_px(25, 30) == pd::kPaper);
  CHECK(canvas_px(10, 40) == pd::kInk);   // the page under it, outside the box, still drawn
  shot("popup");
  // a touch only ends it: no key for the client
  downs = g_rec[0].keys_down;
  tap(pd::kYes);
  CHECK(!pd::popup_up() && g_rec[0].keys_down == downs);
  run(30);
  CHECK(canvas_px(24, 16) == pd::kPaper);   // the top redrawn without it
  // it times out
  pd::popup("HELLO", nullptr, 500);
  run(100);
  CHECK(pd::popup_up());
  run(600);
  CHECK(!pd::popup_up() && canvas_px(24, 16) == pd::kPaper);
  // over the idle frame too, and popup_end
  pd::clear();
  pd::popup("PAIR", "1111 2222", 30000);
  run(30);
  CHECK(canvas_px(24, 16) == pd::kInk);
  pd::popup_end();
  run(30);
  CHECK(canvas_px(24, 16) == pd::kPaper && canvas_px(0, 8) == pd::kInk);

  // ---- the address on the idle frame
  fresh();
  pd::set_address("192.168.1.20");
  run(30);
  shot("idle_ip");
  CHECK(pd::stats().frames >= 1);

  printf("test_pidisplay: PASS\n");
  return 0;
}
