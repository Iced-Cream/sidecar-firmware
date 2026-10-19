// MD5 (RFC 1321), for the DX7 banks' fingerprints (dx7_lib.h): small, no
// tables beyond the 64 constants. Not for security.
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef FLASHMEM   // the Teensy's: run from flash, a section per function (inline
                  // functions in one shared section would conflict)
#define MD5_FLASH(f) __attribute__((section(".flashmem.md5." #f)))
#else
#define MD5_FLASH(f)
#endif

namespace md5 {

class Md5 {
 public:
  Md5() {
    h_[0] = 0x67452301; h_[1] = 0xefcdab89; h_[2] = 0x98badcfe; h_[3] = 0x10325476;
    len_ = 0;
  }

  MD5_FLASH(update) void update(const uint8_t* p, size_t n) {
    size_t have = len_ & 63;
    len_ += n;
    while (n) {
      size_t k = 64 - have < n ? 64 - have : n;
      memcpy(buf_ + have, p, k);
      have += k;
      p += k;
      n -= k;
      if (have == 64) {
        block(buf_);
        have = 0;
      }
    }
  }

  MD5_FLASH(final) void final(uint8_t out[16]) {
    uint64_t bits = len_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while ((len_ & 63) != 56) update(&pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (8 * i));
    update(l, 8);
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) out[4 * i + j] = (uint8_t)(h_[i] >> (8 * j));
  }

 private:
  static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

  MD5_FLASH(block) void block(const uint8_t* b) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint8_t R[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
      m[i] = b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
    uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3];
    for (int i = 0; i < 64; ++i) {
      uint32_t f;
      int g;
      if (i < 16) { f = (bb & c) | (~bb & d); g = i; }
      else if (i < 32) { f = (d & bb) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = bb ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (bb | ~d); g = (7 * i) & 15; }
      uint32_t t = d;
      d = c;
      c = bb;
      bb = bb + rol(a + f + K[i] + m[g], R[(i >> 4) * 4 + (i & 3)]);
      a = t;
    }
    h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d;
  }

  uint32_t h_[4];
  uint64_t len_;
  uint8_t buf_[64];
};

}  // namespace md5
