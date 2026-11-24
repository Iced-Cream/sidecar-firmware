#pragma once
#include <map>
#include "types.h"
// circle-pcie-probe.patch: config reads only; the test fills s_cfg[bus][offset] (an empty map: no host bridge)
class CBcmPCIeHostBridge {
 public:
  static inline std::map<unsigned, u32> s_cfg[2];
  static u32 ProbeConfig(unsigned bus, unsigned slot, unsigned func, unsigned where) {
    if (s_cfg[0].empty() || bus > 1 || slot || func || where > 0xffc) return ~0U;
    auto it = s_cfg[bus].find(where & ~3U);
    return it == s_cfg[bus].end() ? 0 : it->second;
  }
  // circle-pcie-write.patch: the test counts the writes (s_write_ok false: the write does not take)
  static bool ProbeConfigWrite(unsigned bus, unsigned slot, unsigned func, unsigned where, u32 v) {
    if (s_cfg[0].empty() || bus > 1 || slot || func || where > 0xffc) return false;
    ++s_writes;
    if (s_write_ok) s_cfg[bus][where & ~3U] = (s_cfg[bus][where & ~3U] & 0xffff0000u) | (v & 0xffff);
    return true;
  }
  static inline int s_writes = 0;
  static inline bool s_write_ok = true;
  static unsigned ProbeFindCap(unsigned bus, unsigned slot, unsigned func, unsigned id, bool ext) {
    if (!ext) {
      if (!((ProbeConfig(bus, slot, func, 4) >> 16) & 0x10)) return 0;
      unsigned p = ProbeConfig(bus, slot, func, 0x34) & 0xfc;
      for (unsigned n = 0; p >= 0x40 && n < 48; n++) {
        u32 h = ProbeConfig(bus, slot, func, p);
        if ((h & 0xff) == id) return p;
        p = (h >> 8) & 0xfc;
      }
      return 0;
    }
    unsigned p = 0x100;
    for (unsigned n = 0; p >= 0x100 && p <= 0xffc && n < 64; n++) {
      u32 h = ProbeConfig(bus, slot, func, p);
      if (h == 0 || h == ~0U) return 0;
      if ((h & 0xffff) == id) return p;
      p = (h >> 20) & 0xffc;
    }
    return 0;
  }
};
