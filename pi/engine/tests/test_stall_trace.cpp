// pi/engine/tests/test_stall_trace.cpp - stall_trace.h: the ring, its window, the once-a-stall dump and wrap-around
#include "../stall_trace.h"
#include "check.h"
#include <string>
#include <vector>

using namespace stall_trace;

int main() {
  Ring<8> r;
  std::vector<std::string> lines;
  auto out = [&](const char* s) { lines.push_back(s); };

  // window: from ref - before on; ms with one decimal, sign, mf, fields
  r.Add(1000000, 0x0100, kVendor, 1, 0x10, 0, 64, 0);        // -4000 ms: left out
  r.Add(2500000, 0x0200, kGadgetSetup, 0xA1, 0x81, 0, 0x0100, 0x0002);  // -2500 ms
  r.Add(4999950, 0x3ffe, kXhciCmd, 0, 12, 3);                 // -0.05 ms
  r.Add(5050000, 0x0123, kQuiet, 1, 0, 1, 50);                // +50 ms
  CHECK(r.Format(5000000, 3000000, 2, out) == 3);
  CHECK(lines.size() == 2);
  CHECK(lines[0] == "G-2500.0@0200 a1 81 0 0100 0002; X-0.0@3ffe 00 0c 3 0000 0000");
  CHECK(lines[1] == "Q+50.0@0123 01 00 1 0032 0000");

  // wrap: only the last N kept, oldest first
  lines.clear();
  for (unsigned i = 0; i < 20; ++i) r.Add(6000000 + i * 1000, (uint16_t)i, kVendorDone, 0, 0, 0, (uint16_t)i);
  CHECK(r.Format(6000000, 1000000, 100, out) == 8);
  CHECK(lines.size() == 1);
  CHECK(lines[0].rfind("v+12.0@000c", 0) == 0);
  CHECK(lines[0].find("v+19.0@0013") != std::string::npos);

  // max_entries: only the newest kept
  lines.clear();
  CHECK(r.Format(6000000, 1000000, 100, out, 3) == 3);
  CHECK(lines[0].rfind("v+17.0@0011", 0) == 0);

  // a torn entry (seq not matching) is skipped
  lines.clear();
  r.ev[(r.head - 1) & 7].seq = 0;
  CHECK(r.Format(6000000, 1000000, 100, out) == 7);

  // the dump once a stall: held off 2.5 s, at most max_dumps
  Ring<8> d;
  CHECK(d.DumpDue(100));
  CHECK(!d.DumpDue(100 + 2499999));
  CHECK(d.DumpDue(100 + 2500000));
  CHECK(d.dumps == 2);
  for (unsigned i = 0; i < 50; ++i) d.DumpDue(10000000u + i * 3000000u);
  CHECK(d.dumps == 40);
  CHECK(!d.DumpDue(500000000u));

  // the clock wraps (32-bit us): the window still works
  Ring<8> w;
  lines.clear();
  w.Add(0xFFFFFF00u, 1, kBoxCtl, 0x21, 0x0b, 1, 3, 1);
  w.Add(0x00000100u, 2, kResume, 1, 0, 1, 846);
  CHECK(w.Format(0x00000000u, 3000000, 5, out) == 2);
  CHECK(lines[0] == "B-0.2@0001 21 0b 1 0003 0001; r+0.2@0002 01 00 1 034e 0000");

  printf("test_stall_trace: ok\n");
  return 0;
}
