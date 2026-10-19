// extras/loader_a64_test.cpp - RMPA (AArch64) packages: verify (the target check, data_start), the two-part
// placement (code and the writable part far apart), every ABS64 word moved by its part's base, every ADRP to the
// data part re-aimed at the same byte, refusals (misaligned bases, a corrupt ADRP, an RMP1 host).
// build: c++ -std=c++17 -O1 -I../src loader_a64_test.cpp ../src/rmp_loader.cpp -o /tmp/loader_a64_test
//        /tmp/loader_a64_test ../../../../plugins/sine_test/sine_test.a64.rmp   (make -C plugins/sine_test a64)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_api.h"
#include "rmp_loader.h"

static std::vector<uint8_t> read(const char* p) {
  FILE* f = fopen(p, "rb");
  CHECK(f);
  std::vector<uint8_t> v;
  uint8_t b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int64_t adrp_imm(uint32_t insn) {
  int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
  return imm & (1 << 20) ? imm - (1 << 21) : imm;
}

int main(int argc, char** argv) {
  auto pkg = read(argc > 1 ? argv[1] : "../../../../plugins/sine_test/sine_test.a64.rmp");
  rmp_info info;
  rmp_host_target = RMP_TARGET_ARM32;   // a Teensy: refused
  CHECK(rmp_verify(pkg.data(), (uint32_t)pkg.size(), &info) == RMP_ERR_TARGET);
  CHECK(rmp_verify_header_any(pkg.data(), (uint32_t)pkg.size(), &info) == RMP_OK && info.target == RMP_TARGET_A64);
  rmp_host_target = RMP_TARGET_A64;
  CHECK(rmp_verify(pkg.data(), (uint32_t)pkg.size(), &info) == RMP_OK);
  CHECK(info.data_start % RMP_A64_ALIGN == 0 && info.data_start <= info.image_size && info.data_start >= RMP_PLUGIN_BYTES_A64);
  // two places, 1 GB apart (as bases: the bytes live in ordinary buffers)
  std::vector<uint8_t> code(info.data_start), data(info.image_size - info.data_start + info.bss_size + 16);
  const uintptr_t cb = 0x40000000u, db = 0x80000000u;
  CHECK(rmp_place_a64(&info, code.data(), cb + 4096, data.data(), db) == RMP_OK);   // (any page-aligned bases)
  CHECK(rmp_place_a64(&info, code.data(), cb + 8, data.data(), db) == RMP_ERR_RELOC);
  CHECK(rmp_place_a64(&info, code.data(), cb, data.data(), db + 8) == RMP_ERR_RELOC);
  CHECK(rmp_place_a64(&info, code.data(), cb, data.data(), db) == RMP_OK);
  int abs = 0, adrp = 0;
  for (uint32_t k = 0; k < info.reloc_count; ++k) {
    uint32_t wt = rd32(info.relocs + 8 * k), off = wt & 0x0FFFFFFFu, type = wt >> 28;
    if (type == RMP_REL_ABS64) {
      uint64_t was = rd64(info.image + off);
      uint64_t now = off < info.data_start ? rd64(code.data() + off) : rd64(data.data() + off - info.data_start);
      CHECK(now == (was < info.data_start ? was + cb : was - info.data_start + db));
      ++abs;
    } else {
      CHECK(type == RMP_REL_A64_ADRP_DATA && off < info.data_start);
      uint32_t was = rd32(info.image + off), now = rd32(code.data() + off);
      int64_t page_was = (int64_t)(off & ~4095u) + (adrp_imm(was) << 12);          // the data page it named at 0
      int64_t page_now = (int64_t)((cb + off) & ~(uintptr_t)4095) + (adrp_imm(now) << 12);
      CHECK(page_now == (int64_t)db + (page_was - (int64_t)info.data_start));
      CHECK((now & 0x9F00001Fu) == (was & 0x9F00001Fu));   // the same register, still an ADRP
      ++adrp;
    }
  }
  CHECK(abs >= 6);   // the entry table's six functions at least
  // the entry table's function pointers point into the code part
  const uint8_t* t = code.data() + info.entry_offset;
  CHECK(rd32(t) == RMP_PLUGIN_MAGIC);
  for (int f = 0; f < 6; ++f) {
    uint64_t p = rd64(t + 16 + 8 * f);
    CHECK(p >= cb && p < cb + info.data_start);
  }
  // rmp_place: the two parts back to back
  std::vector<uint8_t> one(info.image_size + info.bss_size);
  CHECK(rmp_place(&info, one.data(), cb) == RMP_OK);
  CHECK(memcmp(one.data() + info.entry_offset, info.image + info.entry_offset, 4) == 0);
  // a corrupt ADRP (not an ADRP any more) is refused
  if (adrp) {
    std::vector<uint8_t> bad = pkg;
    for (uint32_t k = 0; k < info.reloc_count; ++k) {
      uint32_t wt = rd32(info.relocs + 8 * k);
      if (wt >> 28 == RMP_REL_A64_ADRP_DATA) { bad[RMP_HEADER_BYTES + (wt & 0x0FFFFFFFu) + 3] = 0; break; }
    }
    rmp_info bi;
    CHECK(rmp_verify_header(bad.data(), (uint32_t)bad.size(), &bi) == RMP_OK);
    bi.image = bad.data() + RMP_HEADER_BYTES;
    bi.relocs = bi.image + bi.image_size;
    CHECK(rmp_place_a64(&bi, code.data(), cb, data.data(), db) == RMP_ERR_RELOC);
  }
  // a bad data_start (not page-aligned) is a format error
  std::vector<uint8_t> ds = pkg;
  uint32_t odd = info.data_start + 8;
  memcpy(ds.data() + 76, &odd, 4);
  CHECK(rmp_verify_header(ds.data(), (uint32_t)ds.size(), &info) == RMP_ERR_FORMAT);
  printf("ok (%d ABS64, %d ADRP to the data part)\n", abs, adrp);
  return 0;
}
