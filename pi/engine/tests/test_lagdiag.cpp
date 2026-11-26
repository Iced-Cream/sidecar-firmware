// pi/engine/tests/test_lagdiag.cpp - the lag diagnostics (lagdiag.h): the "Lag now" marker and its snapshot (part 1),
// the Rytm report-stream timing (part 2) and the Pi-to-Rytm accounting (part 3).
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "lagdiag.h"

static void marker_tests() {
  // the command: exactly "lagnow", never an argument (no free text from the client reaches the log)
  CHECK(lagdiag::is_marker_cmd("lagnow"));
  CHECK(!lagdiag::is_marker_cmd("lagnow=hello"));
  CHECK(!lagdiag::is_marker_cmd("lagnow "));
  CHECK(!lagdiag::is_marker_cmd("lagno"));
  CHECK(!lagdiag::is_marker_cmd(""));
  CHECK(!lagdiag::is_marker_cmd(nullptr));

  // one marker a second at most; N counts the accepted ones per boot
  lagdiag::Marker m;
  CHECK(m.press(5000) == 1);
  CHECK(m.press(5400) == 0);   // 400 ms later: refused, not counted
  CHECK(m.press(5999) == 0);
  CHECK(m.refused() == 2);
  CHECK(m.press(6000) == 2);   // a second after the first accepted one
  CHECK(m.press(7500) == 3);
  // the 32-bit millisecond clock wraps: 1 s still counts across it
  lagdiag::Marker w;
  CHECK(w.press(0xFFFFFF00u) == 1);
  CHECK(w.press(0x000000FFu) == 0);   // 510 ms later
  CHECK(w.press(0x00000300u) == 2);   // 1.0 s later

  char b[96];
  lagdiag::fmt_marker(b, sizeof b, 2, 3723450);   // 1 h 2 min 3.45 s, in ms
  CHECK(std::string(b) == "lag-marker 2 at 01:02:03.45");
  lagdiag::fmt_marker(b, sizeof b, 17, 59999);
  CHECK(std::string(b) == "lag-marker 17 at 00:00:59.99");
  lagdiag::fmt_marker(b, 12, 1, 0);   // a short buffer: cut, terminated, never over
  CHECK(strlen(b) == 11);

  // the one-shot snapshot of the per-second counters (the engine: line's numbers, and the last rxrep / tx lines)
  lagdiag::Snap s = {};
  s.fifo_min = 71, s.under = 411, s.block_max_us = 17, s.c1 = 53, s.c2 = 23, s.c3 = 119, s.pass_max_us = 121;
  s.midi_bytes = 8391, s.sysex = 103262, s.sent_bytes = 545408, s.sent_msgs = 20631, s.sent_fail = 0, s.dropped = 0;
  strcpy(s.rx, "rxrep: v5 n94 g10700 m0003");
  strcpy(s.tx, "txacc: ka n1 b6");
  lagdiag::fmt_snap(b, 0, s);   // (no room: nothing written, no crash)
  char big[400];
  unsigned n = lagdiag::fmt_snap(big, sizeof big, s);
  CHECK(n == strlen(big));
  CHECK(std::string(big) ==
        "lag-snap: fifo min 71 under 411 | block max 17 us | cpu c1 5.3 c2 2.3 c3 11.9 % | pass max 121 us | "
        "midi 8391 B sysex 103262 | to-host 545408 B 20631 msgs fail 0 dropped 0 | rx: rxrep: v5 n94 g10700 m0003 | tx: txacc: ka n1 b6");
  s.rx[0] = s.tx[0] = 0;   // no stream, nothing sent lately: those parts left out
  lagdiag::fmt_snap(big, sizeof big, s);
  CHECK(std::string(big).find("rx:") == std::string::npos && std::string(big).find("tx:") == std::string::npos);
  n = lagdiag::fmt_snap(big, 40, s);   // cut to the buffer
  CHECK(n == 39 && strlen(big) == 39);
}

// ---- part 2: the Rytm's report stream ---------------------------------------------------------------------------------

// A Rytm 03 (PARAMS, 41 bytes with the tag; short: 23) or 04 (TRIG+PARAMS, 43; short: 25) report for voice v: F0 7D 52 54 cmd
// (machine | v) [note vel for 04] then P1..P16 (P1..P8) as 7-bit pairs, [tag pair], F7
static std::vector<uint8_t> report(uint8_t cmd, unsigned v, const std::vector<unsigned>& words, bool tag = true) {
  std::vector<uint8_t> r = {0xF0, 0x7D, 0x52, 0x54, cmd, (uint8_t)(0x08 | v)};
  if (cmd == 0x04 || cmd == 0x44) r.push_back(60), r.push_back(100);
  for (unsigned w : words) r.push_back((uint8_t)(w >> 7 & 0x7F)), r.push_back((uint8_t)(w & 0x7F));
  if (tag) r.push_back(0), r.push_back(0);
  r.push_back(0xF7);
  return r;
}
static std::vector<unsigned> zeros(unsigned n) { return std::vector<unsigned>(n, 0); }

static void feed(lagdiag::RxDiag& d, uint64_t us, const std::vector<uint8_t>& m) { d.on_sysex(us, m.data(), (int)m.size()); }

static void rx_tests() {
  using lagdiag::RxDiag;
  // the report's length and layout are voices.cpp voice_report's: sizes of the messages built here
  CHECK(report(0x03, 0, zeros(16)).size() == 41 && report(0x03, 0, zeros(8), false).size() == 23);
  CHECK(report(0x04, 0, zeros(16)).size() == 43 && report(0x04, 0, zeros(8), false).size() == 25);

  RxDiag d;
  RxDiag::Second s;
  CHECK(!d.take(&s));   // nothing yet

  // voice 5 at 94 reports a second: counts, and the longest gap between two reports of the SAME voice
  std::vector<unsigned> w = zeros(16);
  uint64_t t = 1000000;
  feed(d, t, report(0x03, 5, w));
  feed(d, t + 10700, report(0x03, 5, w));
  feed(d, t + 12000, report(0x03, 2, w));   // another voice in between: not a gap of voice 5's
  feed(d, t + 21400, report(0x03, 5, w));
  feed(d, t + 33000, report(0x03, 5, w));   // 11600 after the last of voice 5
  CHECK(!d.take(&s));                        // the second is not over
  d.tick(t + 1000000);                       // a second after the first report
  CHECK(d.take(&s));
  CHECK(s.n[5] == 4 && s.gap_us[5] == 11600 && s.n[2] == 1 && s.gap_us[2] == 0);
  CHECK(s.sx_n == 5);
  CHECK(!d.take(&s));   // taken once

  // which words changed (a 16-bit mask over the second); the first report of a voice has nothing to differ from
  RxDiag m;
  t = 5000000;
  feed(m, t, report(0x03, 1, w));
  std::vector<unsigned> w2 = w;
  w2[3] = 700;
  feed(m, t + 10000, report(0x03, 1, w2));   // word 3 (both bytes) differs
  std::vector<unsigned> w3 = w2;
  w3[12] = 1;   // the low byte alone
  feed(m, t + 20000, report(0x03, 1, w3));
  feed(m, t + 30000, report(0x03, 1, w3));   // unchanged
  m.tick(t + 1000000);
  CHECK(m.take(&s));
  CHECK(s.mask[1] == ((1u << 3) | (1u << 12)));
  // a short report (8 words) compares 8 words; a 04 carries its note first and the same words; the MIDI machine's 43 / 44 too
  RxDiag q;
  t = 9000000;
  feed(q, t, report(0x04, 3, zeros(8), false));
  std::vector<unsigned> v8 = zeros(8);
  v8[7] = 5;
  feed(q, t + 5000, report(0x04, 3, v8, false));
  feed(q, t + 10000, report(0x44, 3, v8, false));   // (the MIDI machine's 04: same layout)
  feed(q, t + 15000, report(0x43, 3, v8, false));
  q.tick(t + 1000000);
  CHECK(q.take(&s));
  CHECK(s.n[3] == 4 && s.mask[3] == (1u << 7));
  // a 16-word report after an 8-word one: the words 8..15 are new, not "changed"
  RxDiag z;
  feed(z, t, report(0x03, 0, zeros(8), false));
  std::vector<unsigned> big = zeros(16);
  big[9] = 99;
  feed(z, t + 1000, report(0x03, 0, big));
  feed(z, t + 2000, report(0x03, 0, big));
  z.tick(t + 1000000);
  CHECK(z.take(&s));
  CHECK(s.mask[0] == 0);

  // SysEx inter-arrival (every SysEx from the Rytm, reports or not): longest, and the jitter, the mean change between intervals
  RxDiag a;
  t = 2000000;
  const std::vector<uint8_t> other = {0xF0, 0x7D, 0x52, 0x54, 0x49, 1, 2, 0xF7};   // a 49: not a report
  feed(a, t, other);
  feed(a, t + 10000, report(0x03, 0, w));
  feed(a, t + 20000, other);
  feed(a, t + 31000, report(0x03, 0, w));   // intervals 10000 10000 11000
  a.tick(t + 1000000);
  CHECK(a.take(&s));
  CHECK(s.sx_n == 4 && s.sx_max_us == 11000 && s.sx_jit_us == 500);   // (0 + 1000) / 2
  CHECK(s.n[0] == 2);   // only the two reports are a voice's

  // only while the stream is active: a second with no report publishes nothing (other SysEx alone, nothing at all)
  RxDiag n;
  feed(n, t, other);
  feed(n, t + 500, other);
  n.tick(t + 1000000);
  CHECK(!n.take(&s));
  n.tick(t + 5000000);
  CHECK(!n.take(&s));
  // things that are not reports: too short, a foreign manufacturer, a report-looking command with no body
  RxDiag f;
  const std::vector<uint8_t> foreign = {0xF0, 0x41, 0x52, 0x54, 0x03, 0x08, 0xF7};
  feed(f, t, foreign);
  const std::vector<uint8_t> stub = {0xF0, 0x7D, 0x52, 0x54, 0x03, 0x08, 0xF7};
  feed(f, t + 100, stub);
  f.tick(t + 1000000);
  CHECK(!f.take(&s));

  // a pause is not a gap: after more than a second (the stream stopped and started again) the first report only restarts it
  RxDiag p;
  feed(p, t, report(0x03, 4, w));
  feed(p, t + 3000000, report(0x03, 4, w));   // 3 s later
  feed(p, t + 3010000, report(0x03, 4, w));
  p.tick(t + 4100000);
  CHECK(p.take(&s));   // (both seconds: the second one is the one taken; the first rolled at the second report)
  CHECK(s.n[4] == 2 && s.gap_us[4] == 10000);

  // the second after a published one: its first gap reaches back over the roll (the stream never paused)
  RxDiag r;
  t = 1000;
  for (uint64_t k = 0; k < 150; ++k) feed(r, t + k * 10000, report(0x03, 6, w));   // 1.5 s of 100 a second
  CHECK(r.take(&s));                     // the first second rolled by itself, at the report that came after it
  CHECK(s.n[6] == 100 && s.gap_us[6] == 10000);
  r.tick(t + 2000000);
  CHECK(r.take(&s) && s.n[6] == 50 && s.gap_us[6] == 10000);

  // the line: voices that reported only, one compact line, capped
  RxDiag::Second x = {};
  x.n[5] = 94, x.gap_us[5] = 10700, x.mask[5] = 0x0003;
  x.n[0] = 3, x.gap_us[0] = 400000, x.mask[0] = 0x0ff0;
  x.sx_n = 121, x.sx_max_us = 11200, x.sx_jit_us = 410;
  char b[lagdiag::kLine + 8];
  unsigned len = lagdiag::fmt_rx(b, sizeof b, x);
  CHECK(len == strlen(b));
  CHECK(std::string(b) == "rxrep: v0 n3 gap400000us m0ff0 | v5 n94 gap10700us m0003 | sx n121 max11200us jit410us");
  for (unsigned v = 0; v < 8; ++v) x.n[v] = 65535, x.gap_us[v] = 4000000000u, x.mask[v] = 0xffff;
  x.sx_n = 65535, x.sx_max_us = 4000000000u, x.sx_jit_us = 4000000000u;
  len = lagdiag::fmt_rx(b, lagdiag::kLine, x);   // every voice at its widest: capped, with the voices left out counted
  CHECK(len == strlen(b) && len < lagdiag::kLine);
  CHECK(std::string(b).find("sx n65535 max4000000000us jit4000000000us") != std::string::npos);
  CHECK(std::string(b).find("more") != std::string::npos);
  CHECK(std::string(b).find("v0 ") != std::string::npos);   // (the first voices kept)

  // the two cores: a writer rolling seconds, a reader taking them, never a torn one (every field of a second is one number)
  RxDiag live;
  std::atomic<bool> stop(false);
  std::thread core3([&] {
    uint64_t us = 0;
    while (!stop.load()) {
      for (int k = 0; k < 3; ++k) {   // three reports of one voice, then a second passes
        std::vector<unsigned> ww = zeros(16);
        ww[0] = (unsigned)(us / 1000000 % 100);
        feed(live, us, report(0x03, 7, ww));
        us += 400000;
      }
    }
  });
  unsigned seen = 0, torn = 0;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
    RxDiag::Second y;
    if (live.take(&y)) {
      ++seen;
      // 1.2 s of stream a turn: the seconds hold 2 or 3 reports of voice 7 and nothing of the others
      if (y.n[7] < 2 || y.n[7] > 3 || y.sx_n != y.n[7] || y.n[0] != 0 || y.gap_us[7] != 400000) ++torn;
    }
  }
  stop = true;
  core3.join();
  CHECK(seen > 100 && torn == 0);

  // the cost of a report on the receive path (the host's figure is printed; the Pi's is estimated in the report)
  RxDiag cost;
  const std::vector<uint8_t> rep = report(0x03, 5, zeros(16));
  auto c0 = std::chrono::steady_clock::now();
  const unsigned N = 200000;
  for (unsigned k = 0; k < N; ++k) cost.on_sysex(1000000ull + k * 10000ull, rep.data(), (int)rep.size());
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count() / N;
  printf("test_lagdiag: rx report %.0f ns (this build of the host)\n", ns);
}

// ---- part 3: what the Pi sends the Rytm --------------------------------------------------------------------------------

static std::vector<uint8_t> rytm_sx(uint8_t cmd, unsigned body) {   // F0 7D 52 54 <cmd> <body bytes> F7
  std::vector<uint8_t> r = {0xF0, 0x7D, 0x52, 0x54, cmd};
  for (unsigned i = 0; i < body; ++i) r.push_back((uint8_t)(i & 0x7F));
  r.push_back(0xF7);
  return r;
}

static void tx_tests() {
  using lagdiag::TxDiag;
  auto kind = [](const std::vector<uint8_t>& r) { return lagdiag::classify(r.data(), (int)r.size()); };
  CHECK(kind(rytm_sx(0x3C, 1)) == lagdiag::kKeepalive);
  CHECK(kind(rytm_sx(0x38, 20)) == lagdiag::kFrame && kind(rytm_sx(0x3D, 8)) == lagdiag::kFrame);
  CHECK(kind(rytm_sx(0x3E, 0)) == lagdiag::kFrame && kind(rytm_sx(0x36, 3)) == lagdiag::kFrame);
  CHECK(kind(rytm_sx(0x39, 3)) == lagdiag::kCommit);
  CHECK(kind(rytm_sx(0x48, 4)) == lagdiag::kWrite);
  CHECK(kind(rytm_sx(0x1D, 9)) == lagdiag::kOtherSx);                        // a host-link reply
  CHECK(kind({0xB0, 0x10, 0x20}) == lagdiag::kOther);                         // an LFO's CC
  CHECK(kind({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xF7}) == lagdiag::kOther);   // a piece of a long SysEx
  CHECK(kind({0xF0, 0x7D, 0xF7}) == lagdiag::kOtherSx);                       // too short to name a command
  CHECK(kind({0xF0, 0x41, 0x52, 0x54, 0x3C, 0xF7}) == lagdiag::kOtherSx);     // another maker's 3C is not our keepalive
  CHECK(lagdiag::classify(nullptr, 0) == lagdiag::kOther);

  TxDiag t;
  TxDiag::Second s;
  CHECK(!t.take(&s));   // nothing sent
  auto rec = [&](const std::vector<uint8_t>& r) { t.record(r.data(), (int)r.size()); };
  rec(rytm_sx(0x3C, 1));   // 7 bytes
  CHECK(!t.take(&s));      // a keepalive alone is not worth a line (they come every ~0.7 s)
  rec(rytm_sx(0x3C, 1));
  for (int i = 0; i < 3; ++i) rec(rytm_sx(0x38, 20));   // 26 bytes each
  rec(rytm_sx(0x39, 2));
  rec(rytm_sx(0x48, 4));
  rec(rytm_sx(0x48, 4));
  rec(rytm_sx(0x1D, 9));
  rec({0xB0, 1, 2});
  t.pending(120);
  t.pending(4096);
  t.pending(300);   // the peak, not the last
  t.waited(250);
  t.waited(840);
  t.waited(90);
  CHECK(t.take(&s));
  CHECK(s.n[lagdiag::kKeepalive] == 1 && s.bytes[lagdiag::kKeepalive] == 7);   // (the first one went with the quiet second before)
  CHECK(s.n[lagdiag::kFrame] == 3 && s.bytes[lagdiag::kFrame] == 78);
  CHECK(s.n[lagdiag::kCommit] == 1 && s.bytes[lagdiag::kCommit] == 8);
  CHECK(s.n[lagdiag::kWrite] == 2 && s.bytes[lagdiag::kWrite] == 20);
  CHECK(s.n[lagdiag::kOtherSx] == 1 && s.bytes[lagdiag::kOtherSx] == 15);
  CHECK(s.n[lagdiag::kOther] == 1 && s.bytes[lagdiag::kOther] == 3);
  CHECK(s.pend_max == 4096 && s.wait_max_us == 840);
  char b[lagdiag::kLine + 8];
  unsigned len = lagdiag::fmt_tx(b, sizeof b, s);
  CHECK(len == strlen(b));
  CHECK(std::string(b) == "txacc: ka n1 b7 | frm n3 b78 | c39 n1 b8 | w48 n2 b20 | sx n1 b15 | oth n1 b3 | pend 4096 B | wait 840us");
  CHECK(!t.take(&s));   // taken: starts over

  // types that sent nothing are left out; a second that sent nothing but waited long is a line too
  t.waited(2500);
  t.pending(10);
  CHECK(t.take(&s));
  len = lagdiag::fmt_tx(b, sizeof b, s);
  CHECK(std::string(b) == "txacc: pend 10 B | wait 2500us");
  t.waited(1999);
  CHECK(!t.take(&s));   // a normal wait (a bulk send is a fraction of a millisecond): no line
  rec(rytm_sx(0x39, 2));
  CHECK(t.take(&s));
  len = lagdiag::fmt_tx(b, sizeof b, s);
  CHECK(std::string(b) == "txacc: c39 n1 b8 | pend 0 B | wait 0us");
  // widest figures stay inside the cap
  TxDiag::Second w = {};
  for (unsigned k = 0; k < lagdiag::kKinds; ++k) w.n[k] = 4000000000u, w.bytes[k] = 4000000000u;
  w.pend_max = w.wait_max_us = 4000000000u;
  len = lagdiag::fmt_tx(b, lagdiag::kLine, w);
  CHECK(len == strlen(b) && len < lagdiag::kLine);
}

// ---- the review's fixes (H1, M1, L4)
static void rx_fix_tests() {
  using lagdiag::RxDiag;
  RxDiag::Second s;
  std::vector<unsigned> w = zeros(16);
  // H1: the control loop's order. A pass stamps its start, handles its messages (stamped later), then ticks. The tick's time
  // is before the message that opened the second: that must not close the second (an unsigned underflow once did).
  RxDiag a;
  feed(a, 1000040, report(0x03, 5, w));
  a.tick(1000000);   // the pass start, 40 us before the message
  CHECK(!a.take(&s));
  // the whole loop: a pass every 150 us, a voice-5 report every 10.7 ms handled 40 us into its pass, tick with the pass start
  RxDiag b;
  uint64_t next = 0;
  unsigned reports = 0;
  for (uint64_t t0 = 5000000; t0 < 7600000; t0 += 150) {
    if (t0 >= 5000000 + next && reports < 400) {
      feed(b, t0 + 40, report(0x03, 5, w));
      ++reports;
      next += 10700;
    }
    b.tick(t0);
  }
  CHECK(b.take(&s));
  CHECK(s.n[5] >= 90 && s.n[5] <= 95 && s.gap_us[5] >= 10600 && s.gap_us[5] <= 11000 && s.sx_n == s.n[5]);

  // M1: the voice with the longest gap is on the line first, whatever its number, and is never one of the "+N more"
  RxDiag::Second x = {};
  for (unsigned v = 0; v < 8; ++v) x.n[v] = 94, x.gap_us[v] = 10000 + v, x.mask[v] = 0xffff;
  x.gap_us[6] = 250000;
  x.sx_n = 700, x.sx_max_us = 12000, x.sx_jit_us = 300;
  char bf[lagdiag::kLine + 8];
  lagdiag::fmt_rx(bf, lagdiag::kLine, x);
  CHECK(std::string(bf).find("rxrep: v6 n94 gap250000us") == 0);
  CHECK(std::string(bf).find("more") != std::string::npos && strlen(bf) < lagdiag::kLine);

  // L4: the Rytm went away: its words are not compared with the next report (no spurious changed bits)
  RxDiag c;
  feed(c, 1000000, report(0x03, 2, w));
  c.forget();
  std::vector<unsigned> w2 = w;
  w2[5] = 99;
  feed(c, 1010000, report(0x03, 2, w2));
  feed(c, 1020000, report(0x03, 2, w2));
  c.tick(2100000);
  CHECK(c.take(&s));
  CHECK(s.n[2] == 3 && s.mask[2] == 0 && s.gap_us[2] == 10000);
}

int main() {
  marker_tests();
  rx_tests();
  rx_fix_tests();
  tx_tests();
  printf("test_lagdiag: PASS\n");
  return 0;
}
