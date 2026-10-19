// core/tests/test_sysex.cpp - the builders' exact bytes (as voiceloop sent them before the core).
#include <cstring>

#include "check.h"
#include "sysex.h"

int main() {
  using namespace core;
  uint8_t m[256];
  {   // 15: voice, CUT RES ENV ATK DEC REL
    const uint8_t k[6] = {1, 2, 3, 4, 5, 0x85};
    CHECK(sysex_juno_knobs(m, 9, k) == 13);
    const uint8_t want[13] = {0xf0, 0x7d, 0x52, 0x54, 0x15, 9 & 7, 1, 2, 3, 4, 5, 5, 0xf7};
    CHECK(memcmp(m, want, 13) == 0);
  }
  {   // 16: two names (one empty), replace, total 2
    const char* names[2] = {"Brass\x01", nullptr};
    int n = sysex_names(m, 0x16, true, 2, 0, names, 2);
    CHECK(n == 7 + 2 * 14 + 1 && m[4] == 0x16 && m[5] == 1 && m[6] == 2);
    CHECK(m[7] == 0 && memcmp(m + 8, "Brass_", 6) == 0 && m[14] == 0 && m[20] == 0);
    CHECK(m[21] == 1 && m[22] == 0 && m[34] == 0 && m[35] == 0xf7);
    CHECK(sysex_names(m, 0x17, false, 0, 0, names, 0) == 8 && m[5] == 0 && m[6] == 0 && m[7] == 0xf7);
  }
  {   // 14: a row on the card, a row not on it; the name cut by the caller (len)
    PoolRow r[2] = {{1, 300, 2, "kick.wav", 4}, {5, 0x3fff, 0x3fff, "abcdefghijklmnopq", 17}};
    int n = sysex_pool(m, true, r, 2);
    CHECK(n == 7 + 2 * 18 + 1 && m[4] == 0x14 && m[5] == 1 && m[6] == 2);
    CHECK(m[7] == 1 && m[8] == (300 >> 7) && m[9] == (300 & 0x7f) && m[10] == 0 && m[11] == 2);
    CHECK(memcmp(m + 12, "kick", 4) == 0 && m[16] == 0 && m[24] == 0);
    CHECK(m[25] == 5 && m[26] == 0x7f && m[27] == 0x7f && memcmp(m + 30, "abcdefghijklm", 13) == 0);
    CHECK(m[43] == 0xf7);
  }
  {   // 0E: header and two entries (a folder, a big file clipped to 28 bits)
    ListEntry e[2] = {{1, 200, 0, "Drums"}, {0, 3, 0xffffffffu, "x"}};
    int n = sysex_listing(m, 129, 1, 0, 2, e, 2);
    const uint8_t head[13] = {0xf0, 0x7d, 0x52, 0x54, 0x0e, 1, 1, 0, 1, 0, 0, 0, 2};
    CHECK(memcmp(m, head, 13) == 0);
    CHECK(m[13] == 1 && m[14] == 1 && m[15] == 72 && m[16] == 0 && m[19] == 0 && m[20] == 5 && memcmp(m + 21, "Drums", 5) == 0);
    CHECK(m[26] == 0 && m[27] == 0 && m[28] == 3 && m[29] == 0x7f && m[30] == 0x7f && m[31] == 0x7f && m[32] == 0x7f);
    CHECK(m[33] == 1 && m[34] == 'x' && m[35] == 0xf7 && n == 36);
  }
  {   // the empty user slots (core/slots.cpp's replies)
    CHECK(sysex_slot_unchanged(m, 3) == 10);
    const uint8_t u[10] = {0xf0, 0x7d, 0x52, 0x54, 0x1a, 1, 3, 1, 0x7f, 0xf7};
    CHECK(memcmp(m, u, 10) == 0);
    CHECK(sysex_slot_empty(m, 2) == 43 && m[4] == 0x1a && m[6] == 2 && m[7] == 1 && m[12] == 1 && m[42] == 0xf7);
    for (int i = 8; i < 42; ++i) CHECK(i == 12 || m[i] == 0);
    CHECK(sysex_installed_empty(m) == 12);
    const uint8_t l[12] = {0xf0, 0x7d, 0x52, 0x54, 0x1b, 1, 0, 0x21, 1, 1, 0, 0xf7};
    CHECK(memcmp(m, l, 12) == 0);
    CHECK(sysex_slots(m, 0, 0) == 13 && m[4] == 0x1d && m[11] == 1 && m[12] == 0xf7);
    CHECK(sysex_slots(m, 4, 1) == 15 && m[12] == 4 && m[13] == 1 && m[14] == 0xf7);
  }
  printf("test_sysex: PASS\n");
  return 0;
}
