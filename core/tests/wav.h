// core/tests/wav.h - test helpers: write a PCM WAV (16 or 24 bit, or a float one the index must skip) on the Mac.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "platform.h"

inline void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(v >> (8 * i)); }
inline void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v); b.push_back(v >> 8); }

// frames of `ch` channels; sample(i, c) -> int16 full scale; bits 16 or 24; fmt 1 PCM, 3 float.
template <class F>
void write_wav(const char* card_path, int frames, int ch, int bits, int rate, F sample, int fmt = 1) {
  std::vector<uint8_t> d;
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) {
      int32_t s = sample(i, c);
      if (bits == 24) { d.push_back(0); d.push_back(s); d.push_back(s >> 8); }
      else put16(d, (uint16_t)s);
    }
  std::vector<uint8_t> b = {'R', 'I', 'F', 'F'};
  put32(b, 36 + d.size());
  b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(b, 16); put16(b, fmt); put16(b, ch); put32(b, rate); put32(b, rate * ch * bits / 8);
  put16(b, ch * bits / 8); put16(b, bits);
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  put32(b, d.size());
  b.insert(b.end(), d.begin(), d.end());
  FILE* f = fopen(core::host_path(card_path).c_str(), "wb");
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
