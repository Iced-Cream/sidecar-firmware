// pi/engine/tests/test_records.cpp - g_MidiOut's record reader (records.h): whole records only, a half-written
// record kept across calls (a consumer switch), Drain, an empty and an oversize record, and two threads (core 3's
// byte-by-byte producer against core 0's reader).
#include <thread>
#include <vector>

#include "check.h"
#include "records.h"
#include "spsc.h"

typedef Spsc<uint8_t, 64> Ring;

static void put(Ring* q, const std::vector<uint8_t>& rec, size_t from = 0, size_t to = ~size_t(0)) {
  std::vector<uint8_t> all = {uint8_t(rec.size()), uint8_t(rec.size() >> 8)};
  all.insert(all.end(), rec.begin(), rec.end());
  for (size_t i = from; i < all.size() && i < to; ++i) CHECK(q->push(all[i]));
}

int main() {
  static Ring q;
  static RecordReader<Ring, 16> r(&q);
  int len = -1;
  CHECK(r.Next(&len) == nullptr);
  // a record in halves: nothing until it is whole, then exactly it (the half survives a consumer switch: Drain is
  // another consumer's call on the same reader)
  std::vector<uint8_t> a = {0xF0, 1, 2, 3, 0xF7};
  put(&q, a, 0, 4);
  CHECK(r.Next(&len) == nullptr);
  CHECK(r.Drain() == 0);
  put(&q, a, 4);
  const uint8_t* p = r.Next(&len);
  CHECK(p != nullptr && len == 5);
  for (int i = 0; i < 5; ++i) CHECK(p[i] == a[i]);
  CHECK(r.Next(&len) == nullptr);
  // the length split across calls
  std::vector<uint8_t> b = {0x90, 60, 100};
  put(&q, b, 0, 1);
  CHECK(r.Next(&len) == nullptr);
  put(&q, b, 1);
  p = r.Next(&len);
  CHECK(p != nullptr && len == 3 && p[0] == 0x90 && p[2] == 100);
  // an empty record is skipped; Drain drops whole records and leaves a half one for the next consumer
  put(&q, {});
  put(&q, b);
  put(&q, a, 0, 3);
  CHECK(r.Drain() == 3);
  put(&q, a, 3);
  p = r.Next(&len);
  CHECK(p != nullptr && len == 5 && p[0] == 0xF0 && p[4] == 0xF7);
  // an oversize record (> kMax 16) is skipped, counted, and the next one is still in step
  std::vector<uint8_t> big(20, 0x55);
  put(&q, big);
  put(&q, b);
  p = r.Next(&len);
  CHECK(p != nullptr && len == 3 && p[1] == 60 && r.Oversize() == 20);
  // two threads: records of 1..16 bytes, their content a running counter; the reader sees each whole and in order
  static Spsc<uint8_t, 256> q2;
  static RecordReader<Spsc<uint8_t, 256>, 16> r2(&q2);
  const int kRecords = 200000;
  std::thread prod([] {
    uint8_t c = 0;
    for (int n = 0; n < kRecords; ++n) {
      unsigned l = 1 + n % 16;
      while (q2.size() + 2 + l > 256) {}             // ToHost's room check: a record is pushed only whole
      q2.push(uint8_t(l));
      q2.push(0);
      for (unsigned i = 0; i < l; ++i) q2.push(c++);
    }
  });
  uint8_t want = 0;
  int got = 0;
  while (got < kRecords) {
    const uint8_t* rec = r2.Next(&len);
    if (!rec) continue;
    CHECK(len == 1 + got % 16);
    for (int i = 0; i < len; ++i) CHECK(rec[i] == want++);
    ++got;
  }
  prod.join();
  CHECK(r2.Next(&len) == nullptr && r2.Oversize() == 0);
  printf("test_records: PASS\n");
  return 0;
}
