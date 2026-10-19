// core/tests/test_ui.cpp - core/ui: the canvas in the Rytm's framebuffer layout, the fonts, the tile diff, the pages and
// the remote UI host against a model of rytm-mods 0026's terminal (staging, seq-checked commit into the shadow): the
// whole frame on open, then only changed tiles, the keepalive, page keys, an encoder edit through voice_report, the
// held edit against the Rytm's reports, nothing once closed.
#include <cstring>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "engine.h"
#include "version.h"
#include "fx/send_fx.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core::ui;

// ---- 0026's terminal, as rui.c does it (1021: text slots staged and committed with the pixels, echo bindings)
static uint8_t staging[1024], shadow[1024];
static TextItem txt_st[kTextSlots], txt_sh[kTextSlots];
static int bind_slot[16], bind_val[16], binds = 0, texts_in = 0;
static char bind_pre[16][12];
static int st_seq = -1, commits = 0, rects = 0, tiles_in = 0, alive = 0, bad = 0;
static std::vector<std::vector<uint8_t>> other;
// 1022: the catch mask (37) as rui.c decodes it: encoder i bit i % 7 of e(i / 7), key c bit c % 7 of k(c / 7)
static unsigned mask_enc = 0;
static bool mask_key[256];
static int masks_in = 0;
static int splash_msgs = 0, splash_last = -1;   // 1023: 36s and the last one's ticks
static bool caught(int code) { return mask_key[code & 0xFF]; }
static void terminal(const uint8_t* d, int len) {
  if (len == 7 && d[4] == 0x26) return;   // the IN L/R source ask on the first message (host_link.cpp, fxin): not the remote UI's
  if (len < 7 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54 || d[5] != 1 || d[len - 1] != 0xF7) {
    other.emplace_back(d, d + len);
    return;
  }
  const uint8_t* b = d + 6;
  int n = len - 7;
  for (int i = 0; i < len - 1; ++i) CHECK(i == 0 || d[i] < 0x80);   // 7-bit body
  if (d[4] == 0x38) {
    int c0 = b[1], p0 = b[2], nc = b[3], np = b[4], left = nc * 8 * np;
    if (c0 + nc > 16 || p0 + np > 8 || n - 5 != left + (left + 6) / 7) { ++bad; st_seq = -1; return; }
    const uint8_t* p = b + 5;
    uint8_t* col = staging + c0 * 64 + p0;
    int pg = 0;
    while (left > 0) {
      int g = left < 7 ? left : 7;
      unsigned hi = *p++;
      left -= g;
      while (g-- > 0) {
        col[pg] = (uint8_t)(*p++ | (hi & 1) << 7);
        hi >>= 1;
        if (++pg == np) pg = 0, col += 8;
      }
    }
    st_seq = b[0];
    ++rects;
    tiles_in += nc * np;
  } else if (d[4] == 0x39) {
    if (st_seq == b[0]) memcpy(shadow, staging, 1024), memcpy(txt_sh, txt_st, sizeof txt_sh), ++commits;
    st_seq = -1;
  } else if (d[4] == 0x3D) {
    if (n < 6 || b[1] >= kTextSlots || n - 6 > kTextLen) { ++bad; return; }
    TextItem& t = txt_st[b[1]];
    t.x = b[2], t.y = b[3], t.font = b[4], t.flags = b[5];
    memcpy(t.s, b + 6, n - 6);
    t.s[n - 6] = 0;
    st_seq = b[0];
    ++texts_in;
  } else if (d[4] == 0x3E) {
    for (auto& t : txt_st) t.s[0] = 0;
    st_seq = b[0];
  } else if (d[4] == 0x3F) {
    if (n < 6 || b[0] >= 16 || n - 6 > 11) { ++bad; return; }
    bind_slot[b[0]] = b[1], bind_val[b[0]] = b[2];
    memcpy(bind_pre[b[0]], b + 6, n - 6);
    bind_pre[b[0]][n - 6] = 0;
    ++binds;
  } else if (d[4] == 0x37) {
    if (n < 3 || n > 40) { ++bad; return; }
    mask_enc = 0;
    memset(mask_key, 0, sizeof mask_key);
    for (int i = 0; i < 3; ++i) mask_enc |= (unsigned)b[i] << (7 * i);
    for (int j = 0; j < n - 3; ++j)
      for (int i = 0; i < 7; ++i)
        if (b[3 + j] >> i & 1) mask_key[7 * j + i] = true;
    ++masks_in;
  } else if (d[4] == 0x36) {
    if (n < 1) { ++bad; return; }
    splash_last = b[0];
    ++splash_msgs;
  } else if (d[4] == 0x3C) {
    ++alive;
  } else {
    other.emplace_back(d, d + len);
  }
}
static void reset_counts() { commits = rects = tiles_in = alive = texts_in = binds = 0; }
// the Rytm's text slots equal the host's (what it committed last)
static bool texts_equal() {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots), k = 0;
  for (int i = 0; i < kTextSlots; ++i) {
    if (!txt_sh[i].s[0]) continue;
    if (k >= n || strcmp((const char*)t + 36 * k + 4, txt_sh[i].s) || t[36 * k] != txt_sh[i].x || t[36 * k + 1] != txt_sh[i].y)
      return false;
    ++k;
  }
  return k == n;
}
static bool shown_text(const char* s) {
  for (auto& t : txt_sh)
    if (!strcmp(t.s, s)) return true;
  return false;
}

static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  core::engine_sysex(m, 10);
}
static void report03(int vi, const uint8_t p[16]) {
  uint8_t m[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)vi};
  for (int k = 0; k < 16; ++k) m[6 + 2 * k] = p[k], m[7 + 2 * k] = 0;
  m[38] = 0xF7;
  core::engine_sysex(m, 39);
}
// The Rytm's report of voice vi on machine m (the voice byte's flags), its P1..P16 as they are: a 0x03, or a trig
static void report_machine(int vi, int m, bool trig = false) {
  uint8_t r[41] = {0xF0, 0x7D, 0x52, 0x54, (uint8_t)(trig ? 0x04 : 0x03), (uint8_t)(m | vi)};
  int n = 6;
  if (trig) r[n++] = 48, r[n++] = 100;
  for (int k = 0; k < 16; ++k) r[n++] = (uint8_t)core::v[vi].p[k] & 0x7F, r[n++] = 0;
  r[n++] = 0xF7;
  core::engine_sysex(r, n);
}

int main() {
  // ---- canvas: the layout, fills, the diff
  Canvas c;
  c.pixel(0, 0);
  c.pixel(127, 63);
  CHECK(c.fb[7] == 0x01 && c.fb[8 * 127] == 0x80 && c.get(0, 0) && !c.get(1, 0));
  c.clear();
  c.fill(Rect{0, 0, 8, 8}, kOn);   // the top-left tile = page 7, columns 0..7
  for (int x = 0; x < 8; ++x) CHECK(c.fb[7 + 8 * x] == 0xFF);
  Canvas z;
  uint32_t m[4];
  CHECK(c.diff_tiles(z, m) == 1 && (m[3] >> 16 & 1) && !m[0] && !m[1] && m[3] == 1u << 16);   // tile 7 * 16 + 0 = 112
  c.clear();
  c.dither(Rect{0, 0, 128, 64});
  int on = 0;
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 128; ++x) {
      on += c.get(x, y);
      CHECK(c.get(x, y) == (((x + y) & 1) == 0));
    }
  CHECK(on == 4096);
  c.clear();
  c.clip(Rect{10, 10, 5, 5});
  c.line(0, 0, 127, 63);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 128; ++x) CHECK(!c.get(x, y) || (x >= 10 && x < 15 && y >= 10 && y < 15));
  c.unclip();
  c.clear();
  c.line(0, 0, 9, 3);
  CHECK(c.get(0, 0) && c.get(9, 3));
  c.clear();
  CHECK(text_width(kSmall, "ABC") == 11 && text_width(kMedium, "AB") == 11);
  text(c, kMedium, 0, 0, "A");
  CHECK(!c.get(0, 0) && c.get(1, 0) && c.get(0, 1) && c.get(0, 6) && c.get(4, 6));   // .###. / #...#
  CHECK(sin256(0) == 0 && sin256(64) == 127 && sin256(128) == 0 && sin256(192) == -127 && cos256(0) == 127);
  CHECK(list_count("A\0BB\0") == 2 && !strcmp(list_name("A\0BB\0", 1), "BB") && !*list_name("A\0BB\0", 2));

  // ---- the host against the terminal
  char tmpl[] = "/tmp/core_ui_XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::hooks.to_rytm = terminal;
  core::engine_begin();
  // ---- every page draws; a list parameter steps one zone a detent
  for (int which = 0; which < 5; ++which) {
    uint8_t vals[16] = {40, 90, 1, 20, 60, 90, 40, 64, 3, 64, 1, 100, 10, 110, 0, 0};
    CHECK(remote_render(which, 0, vals, c));
    int px = 0;
    for (int i = 0; i < 1024; ++i) px += __builtin_popcount(c.fb[i]);
    CHECK(px > (which == 0 || which == 4 ? 200 : 300));   // the status pages: two bars and a few badges
  }
  const Page* plaits = page_for_machine(0x00);
  CHECK(plaits && !page_for_machine(0x10) && plaits->enc[0] == 0);   // DAISYSP: no page (JUNO, FM, DSP SAMPLE: page_*.cpp)
  CHECK(step_value(*plaits, 0, 0, 1, false) == (1 * 128 + 64) / 17 && core::eng_zone(step_value(*plaits, 0, 64, 3, false), 17) == 8 + 3);
  CHECK(step_value(*plaits, 1, 64, -3, false) == 61 && step_value(*plaits, 1, 64, 2, true) == 80 && step_value(*plaits, 1, 126, 5, false) == 127);

  remote_reset();
  report_machine(0, 0x00);   // track 1 reported on PLAITS (the only voice page: the others never reported)
  remote_tick(1000);
  CHECK(!rects && !alive && !remote_open());   // closed: nothing
  from_rytm(0x30, 0, 1, 0);                    // TRK + YES on the Rytm
  CHECK(remote_open());
  remote_tick(1000);
  CHECK(commits == 1 && tiles_in == 128 && rects == 8 && !memcmp(shadow, remote_frame(), 1024));   // the whole frame
  CHECK(remote_stats().page == 0 && texts_equal() && shown_text("SIDECAR") && !strcmp(kStatusTitle, "SIDECAR") && shown_text("FX1") && shown_text("OFF") &&
        !shown_text("NET -") && !shown_text("OB -"));   // SIDECAR: the tracks and the FX only
  CHECK(binds == 8);   // the SIDECAR page: every encoder's echo none
  // the SIDECAR page's mask: navigation and the stock-screen keys, all nine encoders (A..H, LEVEL) and their pushes;
  // transport, pads, FUNC, TEMPO, MUTE, the trig and bank keys pass
  CHECK(masks_in == 1 && mask_enc == 0x1FF && caught(0x70) && caught(0x77) && caught(0x78) &&
        caught(0x41) && caught(0x45) && caught(0x84) && caught(0x44) &&
        caught(0x34) && caught(0xB0) && caught(0x86) && !caught(0x51) && !caught(0x52) && !caught(0x50) &&
        !caught(0x90) && !caught(0x9B) && !caught(0x80) && !caught(0x83) && !caught(0x11) && !caught(0x00) &&
        !caught(0xC0));
  reset_counts();
  remote_tick(1020);   // within the tick: nothing
  remote_tick(1060);   // a tick, nothing changed: nothing
  CHECK(!rects && !commits && !alive);
  remote_tick(1260);
  CHECK(alive == 1 && !rects);   // 250 ms without a message: 3C
  reset_counts();
  from_rytm(0x31, 0, 0x41, 1);   // RIGHT: SYSTEM
  from_rytm(0x31, 0, 0x41, 0);
  remote_tick(1290);
  CHECK(commits == 1 && remote_stats().page == 2 && texts_equal() && shown_text("SYSTEM " SIDECAR_VERSION) && shown_text("NET -") &&
        shown_text("OB -") && shown_text("U1 -") && !shown_text("FX1") && !memcmp(shadow, remote_frame(), 1024));
  reset_counts();
  from_rytm(0x31, 0, 0x84, 1);   // PAGE: voice 1's PLAITS page
  from_rytm(0x31, 0, 0x84, 0);
  remote_tick(1320);
  CHECK(commits == 1 && remote_stats().page == 1 && remote_stats().voice == 0 && !memcmp(shadow, remote_frame(), 1024));
  CHECK(texts_equal() && shown_text("PLAITS") && shown_text("HRM") && shown_text("BD"));
  // 1022: the PLAITS page takes its encoders (and their pushes) too, nothing else changed (SYSTEM had SIDECAR's mask:
  // not sent again)
  {
    unsigned want = 0;
    for (int e = 0; e < 8; ++e)
      if (plaits->enc[e] != kNone) want |= 1u << e;
    CHECK(masks_in == 2 && mask_enc == want && want && caught(0x70) && caught(0x41) && !caught(0x51) && !caught(0x90) &&
          !caught(0x78) && !(mask_enc & 0x100));
  }
  CHECK(binds == 7 && bind_slot[0] == 0x7F && bind_slot[1] == kSlotFoot && !strcmp(bind_pre[1], "BD HRM:") &&
        bind_slot[7] == kSlotFoot);   // ENG is a list: no echo (unchanged: not sent again); HRM .. LEV echo
  int page_tiles = tiles_in;
  CHECK(page_tiles > 0 && page_tiles <= 128);
  // an encoder: B (HRM, P2) +5 -> the voice's P2 through voice_report, the knob redrawn, a few tiles
  float before = core::v[0].p[1];
  reset_counts();
  from_rytm(0x32, 1, 64 + 5, 0);
  CHECK(core::v[0].p[1] == before + 5 && core::v[0].patch.harmonics > (before + 4) / 127.0f);
  remote_tick(1328);   // a turn: the frame at once (8 ms after the last), not at the next 25 ms tick
  char foot[16];
  snprintf(foot, sizeof foot, "BD HRM:%d", (int)before + 5);
  CHECK(commits >= 1 && commits <= 2 && tiles_in > 0 && tiles_in <= 24 && !memcmp(shadow, remote_frame(), 1024));
  CHECK(texts_equal() && shown_text(foot) && bind_val[1] == (int)before + 5);
  CHECK(commits == 1 || remote_stats().split == 1);   // the touched cell and the bottom bar first when more changed
  printf("ui: whole frame 128 tiles, page change %d tiles, one knob %d tiles in %d rects, %d commits\n", page_tiles,
         tiles_in, rects, commits);
  // the Rytm reports the voice again with its own unchanged P2: the edit holds; its P2 moved: the Rytm wins
  uint8_t p[16] = {45, (uint8_t)before, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100};
  report03(0, p);
  CHECK(core::v[0].p[1] == before + 5);
  p[1] = 30;
  report03(0, p);
  CHECK(core::v[0].p[1] == 30);
  // ENG: a detent is a model
  from_rytm(0x32, 0, 64 + 1, 0);
  CHECK(core::eng_zone(core::v[0].p[0], 17) == core::eng_zone(45, 17) + 1);
  // NO: back to SIDECAR; LEFT from SIDECAR wraps to the last voice page
  from_rytm(0x31, 0, 0x45, 1);
  remote_tick(1440);
  CHECK(remote_stats().page == 0);
  // NO on SIDECAR: the page leaves, as TRK + YES (3A 0 0; the Rytm answers 30 off), then TRK + YES opens it again
  other.clear();
  from_rytm(0x31, 0, 0x45, 1);
  CHECK(other.size() == 1 && other[0].size() == 9 && other[0][4] == 0x3A && other[0][6] == 0 && other[0][7] == 0);
  other.clear();
  from_rytm(0x30, 0, 0, 3);
  CHECK(!remote_open());
  from_rytm(0x30, 0, 1, 0);
  remote_tick(1442);
  CHECK(remote_open() && remote_stats().page == 0);
  from_rytm(0x31, 0, 0x41, 1);   // RIGHT: SYSTEM
  remote_tick(1450);
  CHECK(remote_stats().page == 2);
  // the SYSTEM page's numbers: the recorder, the network (never more than up / down and the address), U1..U5
  RemoteStatus rs = {};
  rs.ob = 1, rs.cpu[0] = 12, rs.cpu[1] = 34, rs.cpu[2] = -1, rs.rec = 3, rs.rec_secs = 75, rs.eth = 2;
  rs.ip[0] = 192, rs.ip[1] = 168, rs.ip[2] = 1, rs.ip[3] = 20;
  strcpy(rs.user[0], "CLUST1");
  strcpy(rs.user[1], "---");
  remote_status(rs);
  core::fx::control.knob[0][0] = 4, core::fx::control.knob[0][1] = 127;   // FX1 PINGPONG, P1 POST (route: per slot)
  remote_tick(1480);
  CHECK(shown_text("ETH UP 192.168.1.20") && shown_text("REC 1:15") && shown_text("U1 CLUST1") && shown_text("U2 ---") &&
        shown_text("12%") && shown_text("34%") && shown_text("OB OK"));
  // the whole of a long name; Wi-Fi up and its address
  strcpy(rs.user[4], "LONGNAM8");
  rs.eth = 1, rs.wifi = 2, rs.ip[2] = 100, rs.ip[3] = 200;
  remote_status(rs);
  remote_tick(1510);
  CHECK(shown_text("U5 LONGNAM8") && shown_text("WIFI UP 192.168.100.200"));
  // LEFT: back to SIDECAR. Every row fits, nothing cut (1021: "PLAIT" in the last column; "FX1 " + a name ran into POST)
  from_rytm(0x31, 0, 0x40, 1);
  remote_tick(1520);
  int plaits_shown = 0;
  for (auto& t : txt_sh) plaits_shown += !strcmp(t.s, "PLAITS");
  CHECK(remote_stats().page == 0 && plaits_shown == 1 && shown_text("PINGPONG") && shown_text("POST") &&
        shown_text("FX2") && shown_text("OFF") && !shown_text("PRE"));
  core::fx::control.knob[0][0] = 0, core::fx::control.knob[0][1] = 0;
  from_rytm(0x31, 0, 0x40, 1);
  remote_tick(1530);
  CHECK(remote_stats().page == 3 && !memcmp(shadow, remote_frame(), 1024));   // LEFT on SIDECAR: the last page, SCOPE
  from_rytm(0x31, 0, 0x40, 1);
  remote_tick(1545);
  CHECK(remote_stats().page == 3 && remote_stats().voice == 0 && !memcmp(shadow, remote_frame(), 1024));   // LFO (core/lfo.h)
  from_rytm(0x31, 0, 0x40, 1);
  remote_tick(1560);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 0 && !memcmp(shadow, remote_frame(), 1024));   // the only voice page
  // closed by the Rytm (timeout / escape): nothing more, not even keepalives
  from_rytm(0x30, 0, 0, 2);
  reset_counts();
  for (uint32_t t = 1540; t < 4000; t += 10) remote_tick(t);
  CHECK(!remote_open() && !rects && !alive && !commits && !bad);
  // reopened: the whole frame again (the shadow may hold anything)
  from_rytm(0x30, 0, 1, 1);
  remote_tick(5000);
  CHECK(tiles_in == 128 && commits == 1 && remote_stats().opens == 3 && remote_stats().closes == 2);
  CHECK(mask_enc == 0x1FF && caught(0x78) && caught(0x41) && remote_stats().masks == (uint32_t)masks_in);   // the mask again on reopening

  // ---- 1023: the connect splash. The Rytm's hello (35 <uptime>) when it was up >= 3 s: the splash frame, 36 64, the
  // line running down in small frames, 36 0 at 3 s; an early hello (the Rytm booting with the Sidecar), caps 0 or a
  // hello while the page is open: a 3C only. The user opening the page during the splash replaces it.
  from_rytm(0x30, 0, 0, 2);
  reset_counts();
  from_rytm(0x35, 0, 20, 1);   // up 1 s
  remote_tick(6000);
  remote_tick(6100);
  CHECK(!splash_msgs && alive == 1 && !commits);
  reset_counts();
  from_rytm(0x35, 1, 0, 1);    // up 128 ticks = 6.4 s
  remote_tick(7000);
  CHECK(commits == 1 && tiles_in == 128 && splash_msgs == 1 && splash_last == 64 && shown_text("CONNECTED") &&
        !remote_open() && !memcmp(shadow, remote_frame(), 1024));
  for (uint32_t t = 7010; t < 9990; t += 10) remote_tick(t);
  printf("ui: splash: %d commits, %d tiles after the first frame\n", commits, tiles_in - 128);
  CHECK(commits >= 25 && commits <= 40 && tiles_in - 128 <= 3 * commits && splash_msgs == 1 &&
        !memcmp(shadow, remote_frame(), 1024));
  remote_tick(10000);
  CHECK(splash_msgs == 2 && splash_last == 0 && remote_stats().splashes == 1 && remote_stats().hellos == 2);
  reset_counts();
  for (uint32_t t = 10010; t < 12000; t += 10) remote_tick(t);
  CHECK(!commits && !alive && !rects);   // over: nothing more while the page is closed
  from_rytm(0x35, 1, 0, 0);    // caps 0: a Rytm that cannot show it
  remote_tick(12010);
  CHECK(alive == 1 && !commits && splash_msgs == 2);
  reset_counts();
  from_rytm(0x35, 1, 0, 1);
  remote_tick(13000);
  CHECK(commits == 1 && splash_msgs == 3);
  from_rytm(0x30, 0, 1, 0);    // TRK + YES during the splash
  for (uint32_t t = 13010; t < 17000; t += 10) remote_tick(t);
  CHECK(remote_open() && splash_msgs == 3 && remote_stats().page == 0 && shown_text(kStatusTitle) && !shown_text("CONNECTED"));
  reset_counts();
  from_rytm(0x35, 1, 0, 1);    // a hello while the page is open: a new link (the Rytm's page left by its timeout, its 30
  remote_tick(17010);          // off lost with the cable): closed here too, the splash (0026 ignores a 36 while open)
  CHECK(!remote_open() && splash_msgs == 4);
  for (uint32_t t = 17020; t < 21000; t += 10) remote_tick(t);
  CHECK(splash_last == 0);
  CHECK(other.empty() && !bad);   // nothing but remote-UI messages went out, all well formed

  // ---- the voice pages follow the machine the Rytm's track plays, as its reports name it (the voice byte), live:
  // nothing reported yet (an analog track, or one just set on the SRC page: 0021 reports a voice only while it sounds)
  // -> no page and "-" on SIDECAR, never a guessed PLAITS; each later report moves the page to its machine at once
  remote_reset();
  from_rytm(0x30, 0, 1, 0);
  remote_tick(20000);
  CHECK(remote_open() && remote_stats().page == 0 && !shown_text("PLAITS") && shown_text("-"));
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // PAGE: SYSTEM
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // PAGE again: no voice page, an extra page (SCOPE)
  remote_tick(20100);
  CHECK(remote_stats().page != 1);
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: back to SIDECAR
  report_machine(2, core::kJuno, true);                        // track 3 trigs on JUNO (mod 0021's EXT machine 41)
  remote_tick(20200);
  CHECK(remote_stats().page == 0 && shown_text("JUNO"));      // SIDECAR names it
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // SYSTEM, then the first voice page: track 3's JUNO
  remote_tick(20300);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 2 && shown_text("JUNO") && shown_text("CUT"));
  static const struct { int m; const char* title; } kFollow[] = {
      {core::kFm, "6-OP FM"}, {core::kPlaits, "PLAITS"}, {core::kDspSample, "DSP SAMPLE"}, {core::kJuno, "JUNO"}};
  uint32_t t = 20400;
  for (const auto& f : kFollow) {   // the machine changed on the Rytm, the page open: the next frame is the new one's
    report_machine(2, f.m, true);
    remote_tick(t += 100);
    CHECK(remote_stats().page == 1 && remote_stats().voice == 2 && shown_text(f.title));
  }
  report_machine(2, 0x50, true);    // U1, a user machine: no page here - the page leaves for SIDECAR
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1 && !shown_text("JUNO"));
  report_machine(2, core::kDaisy);  // DAISYSP: no page either
  report_machine(5, core::kFm);     // track 6 on 6-OP FM: its page
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 5 && shown_text("6-OP FM"));
  remote_forget_voices();   // the platform's boot bench reported voices itself: forgotten, no voice page left
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1);

  // ---- 1025: the Rytm's 33 names each track's machine at once (no trig needed) and decides alone from then on
  auto machines = [](int t0, int n, const uint8_t* mv) {   // F0 7D 52 54 33 01 <t0> <n> {<machine> <vb>} x n F7
    uint8_t m[40] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t0, (uint8_t)n};
    memcpy(m + 8, mv, 2 * n);
    m[8 + 2 * n] = 0xF7;
    core::engine_sysex(m, 9 + 2 * n);
  };
  // the PAT lists (16 JUNO / 17 FM) share one cache on the Rytm: a track that turns JUNO / 6-OP FM gets its list pushed
  auto sent = [](uint8_t cmd) {
    int k = 0;
    for (auto& m : other) k += m.size() > 5 && m[4] == cmd;
    return k;
  };
  uint8_t snap[24];
  for (int k = 0; k < 12; ++k) snap[2 * k] = (uint8_t)k, snap[2 * k + 1] = 0x7F;   // every track analog
  report_machine(1, core::kPlaits, true);   // an older build's path: a report alone ...
  other.clear();
  machines(0, 12, snap);                    // ... and then the snapshot, which decides
  CHECK(sent(0x16) == 0 && sent(0x17) == 0);   // a snapshot pushes no list
  CHECK(remote_track_machine(0) == 0x7F && remote_stats().machines == 1);
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);   // NO: SIDECAR
  remote_tick(t += 100);
  CHECK(remote_stats().page == 0 && !shown_text("PLAITS") && !shown_text("FM"));
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);   // SYSTEM, then no voice page
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1);
  const uint8_t juno_bd[2] = {41, 0x08};   // BD picked JUNO in the SRC list (no trig)
  other.clear();
  machines(0, 1, juno_bd);
  CHECK(sent(0x16) >= 1 && sent(0x17) == 0);   // JUNO's list pushed (PAT names at once, not after the Rytm's re-ask)
  other.clear();
  machines(0, 1, juno_bd);
  CHECK(sent(0x16) == 0);                      // the same machine again: not pushed again
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 0 && shown_text("JUNO"));
  const uint8_t fm_cp[2] = {40, 0x48 | 2};   // CP (track 4) on 6-OP FM: 0026's 33 says voice 2, its 04s voice 7 (RS/CP)
  other.clear();
  machines(3, 1, fm_cp);
  CHECK(sent(0x17) >= 1 && sent(0x16) == 0);   // the FM list pushed
  const uint8_t analog_bd[2] = {22, 0x7F};   // BD back to an analog machine: its page leaves at once
  machines(0, 1, analog_bd);
  remote_tick(t += 100);
  CHECK(remote_stats().page != 1 || remote_stats().voice != 0);
  from_rytm(0x31, 0, 0x45, 1), from_rytm(0x31, 0, 0x45, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 0 && shown_text("FM") && !shown_text("JUNO"));
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  from_rytm(0x31, 0, 0x84, 1), from_rytm(0x31, 0, 0x84, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 7 && shown_text("6-OP FM"));
  // SD (track 2) on JUNO: 0026's 33 says voice 1, the Rytm plays it on voice 2 (its 04s): the page is V3's, not V2's
  const uint8_t juno_sd[2] = {41, 0x08 | 1};
  machines(1, 1, juno_sd);
  CHECK(remote_track_machine(1) == (0x08 | 2));
  uint8_t mixed[24];   // the Pi's live snapshot [measured 2026-10-19]: 08 09 72 42 7f 7f 75 15 4e 7f 0f 7f
  const uint8_t live[12] = {0x08, 0x09, 0x72, 0x42, 0x7F, 0x7F, 0x75, 0x15, 0x4E, 0x7F, 0x0F, 0x7F};
  for (int k = 0; k < 12; ++k) mixed[2 * k] = 0, mixed[2 * k + 1] = live[k];
  machines(0, 12, mixed);
  const int want[12] = {0x08, 0x0A, 0x77, 0x47, 0x7F, 0x7F, 0x75, 0x15, 0x4C, 0x7F, 0x0E, 0x7F};   // voices by the 04s
  for (int k = 0; k < 12; ++k) CHECK(remote_track_machine(k) == want[k]);
  machines(3, 1, fm_cp);   // CP back on 6-OP FM for what follows
  const uint8_t past[4] = {0, 0x7F, 0, 0x7F};   // tracks 11 and 12: past track 11, refused, nothing changes
  machines(11, 2, past);
  CHECK(remote_track_machine(3) == (0x48 | 7) && remote_stats().machines == 8);

  // ---- 1027: the SRC view. With the page closed, the Rytm's stock SRC page of a track on a Sidecar machine shows the
  // host's frames (a whole frame answers each 29 t 1), drawn from the Rytm's own values (2A) and only from them:
  // no 37 mask, no 3F echo, no edit, never a 29 back; a track without a page gets nothing (3A 0 0 when its page went);
  // the TRK + YES page comes first.
  auto src_vals = [](int tr, int first, int n, const uint8_t* vals) {   // F0 7D 52 54 2A 01 <t> <first> <n> {int frac}
    uint8_t m[48] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)tr, (uint8_t)first, (uint8_t)n};
    for (int i = 0; i < n; ++i) m[9 + 2 * i] = vals[i], m[10 + 2 * i] = 0;
    m[9 + 2 * n] = 0xF7;
    core::engine_sysex(m, 10 + 2 * n);
  };
  auto src_view = [](int tr, int on) {
    const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)tr, (uint8_t)on, 0xF7};
    core::engine_sysex(m, 9);
  };
  auto sent_cmd = [](uint8_t cmd) {
    int k = 0;
    for (auto& m : other) k += m.size() > 5 && m[4] == cmd;
    return k;
  };
  auto shown_prefix = [](const char* pre) -> const char* {
    for (auto& tx : txt_sh)
      if (!strncmp(tx.s, pre, strlen(pre))) return tx.s;
    return nullptr;
  };
  from_rytm(0x30, 0, 0, 0);   // the page closed
  remote_tick(t += 100);
  float p0[16];
  for (int k = 0; k < 16; ++k) p0[k] = core::v[0].p[k];
  const uint8_t juno_vals[8] = {3, 100, 20, 90, 10, 60, 40, 127};   // BD's PAT CUT RES ENV ATK DEC REL LEN on its SRC page
  src_vals(0, 0, 8, juno_vals);
  other.clear(), reset_counts();
  int masks0 = masks_in, binds0 = binds, bad0 = remote_stats().bad;
  src_view(0, 1);   // BD (JUNO) shown
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(commits >= 1 && rects >= 1 && remote_stats().page == 4 && remote_stats().voice == 0 && sent_cmd(0x29) == 0);
  CHECK(shown_text("JUNO") && shown_text("BD") && texts_equal());
  CHECK(masks_in == masks0 && binds == binds0);   // every key and knob the Rytm's
  for (int k = 0; k < 16; ++k) CHECK(core::v[0].p[k] == p0[k]);   // the page only reads
  // a knob on the Rytm (2A n 1): the drawing follows, the bottom bar names it
  int c1 = commits, ti1 = tiles_in;
  const uint8_t cut[1] = {77};
  src_vals(0, 1, 1, cut);
  remote_tick(t += 10);
  CHECK(commits > c1 && tiles_in > ti1);
  const char* f = shown_prefix("BD CUT:");
  CHECK(f && !strcmp(f, "BD CUT:77"));
  for (int k = 0; k < 16; ++k) CHECK(core::v[0].p[k] == p0[k]);
  // nothing new: keepalives only
  c1 = commits;
  for (int i = 0; i < 12; ++i) remote_tick(t += 30);
  CHECK(commits == c1 && alive >= 1);
  // TRK + YES opens the remote page: the view waits, the page's own frames and mask; closed: the view's whole frame
  from_rytm(0x30, 0, 1, 0);
  remote_tick(t += 100);
  CHECK(remote_stats().page != 4 && masks_in > masks0);
  from_rytm(0x30, 0, 0, 0);
  other.clear();
  int r1 = rects;
  c1 = commits;
  remote_tick(t += 100);
  CHECK(commits > c1 && rects - r1 >= 8 && remote_stats().page == 4 && shown_text("BD CUT:77") && sent_cmd(0x3A) == 0);
  // the 29 again (after a hello): a whole frame again
  r1 = rects;
  src_view(0, 1);
  remote_tick(t += 100);
  CHECK(rects - r1 >= 8 && remote_stats().page == 4);
  // SD's view: SD is on JUNO too (voice 2); BT is analog: no page, nothing sent
  src_view(1, 1);
  remote_tick(t += 100);
  CHECK(shown_text("SD") && shown_text("JUNO"));
  other.clear(), c1 = commits;
  src_view(4, 1);
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(commits == c1 && other.empty());
  // BD's view, then BD turns analog while it shows: 3A 0 0 (stock back), nothing drawn after
  src_view(0, 1);
  remote_tick(t += 100);
  CHECK(commits > c1);
  machines(0, 1, analog_bd);
  other.clear(), c1 = commits;
  for (int i = 0; i < 4; ++i) remote_tick(t += 30);
  CHECK(commits == c1 && sent_cmd(0x3A) == 1 && other.size() == 1 && other[0][6] == 0 && other[0][7] == 0);
  src_view(0, 0);
  machines(0, 1, juno_bd);
  src_view(4, 0);   // another track's 29 0: BD's view ends only by its own
  src_view(0, 1), remote_tick(t += 100), src_view(0, 0);   // left: nothing more is sent
  other.clear(), reset_counts();
  for (int i = 0; i < 20; ++i) remote_tick(t += 30);
  CHECK(other.empty() && commits == 0 && alive == 0);
  // a replug while the view is on (and while the TRK + YES page is open): the hello resets both, the splash shows, then
  // the view's 29 t 1 (repeated after the hello) gets a whole frame once the splash ended
  src_view(0, 1);
  remote_tick(t += 100);
  CHECK(remote_stats().page == 4);
  from_rytm(0x30, 0, 1, 0);   // the page open when the cable goes
  remote_tick(t += 100);
  int sp0 = splash_msgs;
  from_rytm(0x35, 0, 100, 1);   // hello: up 5 s
  src_view(0, 1);
  CHECK(!remote_open());
  for (int i = 0; i < 10; ++i) remote_tick(t += 30);
  CHECK(splash_msgs == sp0 + 1 && splash_last == 64);   // the splash, not blocked by the stale page / view
  r1 = rects;
  for (int i = 0; i < 120; ++i) remote_tick(t += 30);   // past its 3 s: 36 0, then the view
  CHECK(splash_last == 0 && remote_stats().page == 4 && rects - r1 >= 8 && shown_text("JUNO") && shown_text("BD"));
  src_view(0, 0);
  // the Rytm has the SRC view (it sent 29 / 2A): TRK + YES lists SIDECAR, SYSTEM and the live pages, no voice page
  // (BD and SD are on JUNO, CP on 6-OP FM: an older build listed them)
  from_rytm(0x30, 0, 1, 0);
  for (int i = 0; i < 12; ++i) {
    from_rytm(0x31, 0, 0x41, 1), from_rytm(0x31, 0, 0x41, 0);
    remote_tick(t += 100);
    CHECK(remote_stats().page != 1 && remote_stats().page != 4);
  }
  from_rytm(0x30, 0, 0, 0);
  remote_tick(t += 100);
  // malformed 2A / 29 are refused
  const uint8_t bad_m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 12, 0, 0xF7};
  core::engine_sysex(bad_m, 9);
  uint8_t bad_n[12] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 15, 2, 1, 0, 0xF7};   // P16 + 1 past the end
  core::engine_sysex(bad_n, 12);
  CHECK(remote_stats().bad == (uint32_t)bad0 + 2);

  float full = 0, one = 0;
  for (int which = 0; which < 5; ++which) {
    remote_bench(which, 0, 2000, &full, &one);
    printf("ui: page %d: whole page drawn + diffed %.1f us, one change %.1f us (Mac)\n", which, full, one);
  }
  printf("test_ui: PASS\n");
  return 0;
}
