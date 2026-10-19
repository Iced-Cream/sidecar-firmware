// extras/meta_test.cpp - slot and store generations on the Mac.
// build: c++ -std=c++17 -O1 -Wall -I../src meta_test.cpp ../src/rmp_meta.cpp ../src/rmp_manifest.cpp ../src/rmp_loader.cpp -o /tmp/meta_test && /tmp/meta_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_meta.h"
#include "rmp_proto.h"

int main() {
  rmp_meta m;
  rmp_meta_init(&m);
  for (int s = 0; s < 5; ++s) CHECK(m.gen[s] == 1);
  CHECK(m.store_gen == 1 && rmp_meta_take(&m) && !rmp_meta_take(&m));   // a 1D is due at boot, once
  const uint32_t empty = rmp_meta_print(RMP_PST_EMPTY, 0, "", 0, nullptr);
  CHECK(!rmp_meta_set(&m, 0, empty) && m.gen[0] == 1 && !rmp_meta_take(&m));   // still empty: no bump
  rmp_desc d; memset(&d, 0, sizeof d); memcpy(d.name, "SINE", 4); d.nknobs = 1; d.knob[0].p = 1;
  uint32_t loading = rmp_meta_print(RMP_PST_LOADING, 0, "rytm.sine_test", 0x010000, nullptr);
  uint32_t ready = rmp_meta_print(RMP_PST_READY, 0, "rytm.sine_test", 0x010000, &d);
  CHECK(loading != ready && loading != empty);
  CHECK(rmp_meta_set(&m, 0, loading) && m.gen[0] == 2 && rmp_meta_take(&m));
  CHECK(rmp_meta_set(&m, 0, ready) && m.gen[0] == 3);
  CHECK(!rmp_meta_set(&m, 0, ready) && m.gen[0] == 3);   // the same again: no bump
  // the description, the flags, the version: each changes the fingerprint
  rmp_desc e = d; e.knob[0].max = 100;
  CHECK(rmp_meta_print(RMP_PST_READY, 0, "rytm.sine_test", 0x010000, &e) != ready);
  CHECK(rmp_meta_print(RMP_PST_READY, RMP_FLAG_PRESETS, "rytm.sine_test", 0x010000, &d) != ready);
  CHECK(rmp_meta_print(RMP_PST_READY, 0, "rytm.sine_test", 0x010001, &d) != ready);
  CHECK(rmp_meta_print(RMP_PST_READY, 0, "rytm.sine_tesu", 0x010000, &d) != ready);
  // a state change alone (the same description) bumps the generation
  uint32_t muted = rmp_meta_print(RMP_PST_MUTED, 0, "rytm.sine_test", 0x010000, &d);
  CHECK(muted != ready && rmp_meta_set(&m, 0, muted) && m.gen[0] == 4);
  CHECK(rmp_meta_set(&m, 0, ready) && m.gen[0] == 5);
  // a null id is the empty id
  CHECK(rmp_meta_print(RMP_PST_EMPTY, 0, nullptr, 0, nullptr) == empty);
  // nknobs beyond RMP_DESC_KNOBS is clamped (never reads past knob[])
  rmp_desc big = d; big.nknobs = 200;
  rmp_desc full = d; full.nknobs = RMP_DESC_KNOBS;
  CHECK(rmp_meta_print(RMP_PST_READY, 0, "rytm.sine_test", 0x010000, &big) ==
        rmp_meta_print(RMP_PST_READY, 0, "rytm.sine_test", 0x010000, &full));
  // a missing package's description (from its id): stable, and differs by id and from no description
  rmp_desc f1, f2, f3;
  rmp_desc_from_id("befaco.np.atari", &f1); rmp_desc_from_id("befaco.np.atari", &f2); rmp_desc_from_id("befaco.np.tari", &f3);
  uint32_t miss = rmp_meta_print(RMP_PST_MISSING, 0, "befaco.np.atari", 0x010000, &f1);
  CHECK(miss == rmp_meta_print(RMP_PST_MISSING, 0, "befaco.np.atari", 0x010000, &f2));
  CHECK(miss != rmp_meta_print(RMP_PST_MISSING, 0, "befaco.np.atari", 0x010000, nullptr));
  CHECK(miss != rmp_meta_print(RMP_PST_MISSING, 0, "befaco.np.atari", 0x010000, &f3));
  // wraps 127 -> 1, never 0 (0 is the Rytm's "knows nothing")
  m.gen[4] = 126;
  CHECK(rmp_meta_set(&m, 4, 11) && m.gen[4] == 127);
  CHECK(rmp_meta_set(&m, 4, 12) && m.gen[4] == 1);
  // the store
  rmp_meta_take(&m);
  rmp_meta_store(&m);
  CHECK(m.store_gen == 2 && rmp_meta_take(&m));
  m.store_gen = 127; rmp_meta_store(&m); CHECK(m.store_gen == 1);
  // a bump (a 1E answered before the slot is fingerprinted): a new generation, the print invalidated - the
  // settled slot's next print bumps it again, whether another print or the same one (a 1A sent at the bumped
  // generation before the slot settled is never left uncorrected); after that the same print does not
  rmp_meta_take(&m);
  m.gen[1] = 7; m.print[1] = ready;
  rmp_meta_bump(&m, 1);
  CHECK(m.gen[1] == 8 && m.print[1] != ready && rmp_meta_take(&m));
  CHECK(rmp_meta_set(&m, 1, muted) && m.gen[1] == 9 && rmp_meta_take(&m));
  rmp_meta_bump(&m, 1);
  CHECK(m.gen[1] == 10 && rmp_meta_take(&m));
  CHECK(rmp_meta_set(&m, 1, muted) && m.gen[1] == 11 && rmp_meta_take(&m));   // the same print settles: a push
  CHECK(!rmp_meta_set(&m, 1, muted) && m.gen[1] == 11 && !rmp_meta_take(&m));
  // two bumps before the slot settles (two 1Es while its description or scan is pending): still invalidated
  rmp_meta_bump(&m, 1); rmp_meta_bump(&m, 1);
  CHECK(m.gen[1] == 13 && m.print[1] != muted && rmp_meta_take(&m));
  CHECK(rmp_meta_set(&m, 1, muted) && m.gen[1] == 14 && rmp_meta_take(&m));   // the same print settles: a push
  m.gen[1] = 127; rmp_meta_bump(&m, 1); CHECK(m.gen[1] == 1);
  rmp_meta_bump(&m, 5); rmp_meta_bump(&m, -1);   // out of range: nothing
  // a seed (the Teensy's boot): every generation 1..127, derived from it (the same seed, the same values), other
  // seeds other values; seed 1 is the default's every-generation-1
  {
    rmp_meta a, b, c, one;
    rmp_meta_init(&a, 0xDEADBEEFu); rmp_meta_init(&b, 0xDEADBEEFu); rmp_meta_init(&c, 0x12345678u); rmp_meta_init(&one, 1);
    bool differ = false, not_all_1 = false;
    for (int s = 0; s < 5; ++s) {
      CHECK(a.gen[s] >= 1 && a.gen[s] <= 127 && a.gen[s] == b.gen[s] && one.gen[s] == 1);
      CHECK(a.print[s] == empty);
      differ |= a.gen[s] != c.gen[s];
      not_all_1 |= a.gen[s] != 1;
    }
    CHECK(a.store_gen >= 1 && a.store_gen <= 127 && a.store_gen == b.store_gen && one.store_gen == 1 && rmp_meta_take(&a));
    CHECK(differ && not_all_1);
    for (uint32_t seed = 0; seed < 5000; ++seed) {   // never 0 (the Rytm's "knows nothing"), never past 127
      rmp_meta t; rmp_meta_init(&t, seed * 2654435761u);
      for (int s = 0; s < 5; ++s) CHECK(t.gen[s] >= 1 && t.gen[s] <= 127);
      CHECK(t.store_gen >= 1 && t.store_gen <= 127);
    }
  }
  // a slot out of range does nothing
  CHECK(!rmp_meta_set(&m, 5, 99) && !rmp_meta_set(&m, -1, 99));
  printf("ok\n");
  return 0;
}
