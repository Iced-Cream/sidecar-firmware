// plugins/noiseplethora/shim/Plugin.hpp - the base class Noise Plethora's plugin
// classes derive from, written for this build (NP's own Plugin.hpp/PluginFactory.hpp
// are part of its CC BY-NC-SA framework and are not used: no registry, no
// std::map/std::function, no function-local statics). The Makefile copies the
// P_*.hpp plugin headers it builds into build/np/, without NP's Plugin.hpp, so
// their #include "Plugin.hpp" finds this one. REGISTER_PLUGIN is empty.
#pragma once
#include <Audio.h>

class Plugin {
public:
  Plugin() {}
  virtual ~Plugin() {}
  Plugin(const Plugin &) = delete;
  Plugin &operator=(const Plugin &) = delete;
  virtual void init() {}
  virtual void process(float k1, float k2) { (void)k1; (void)k2; }
  virtual AudioStream &getStream() = 0;   // every NP plugin overrides it
  virtual unsigned char getPort() { return 0; }
};

// NP squares its knobs with pow(knob, 2) (float, int). At -O2 GCC folds the double pow(x, 2.0)
// into x * x; at -Os (cold code, the Makefile's NP_COLD) it would call libm's double pow (about
// 9 KB with soft-double helpers). This exact match does the same double multiply at any level:
// (double)k * k is exact for a float k, so the result is bit-identical to the folded pow.
inline double pow(float b, int e) {
  double r = 1.0;
  for (int i = 0; i < e; ++i) r *= b;     // NP only uses e == 2: 1.0 * b * b == (double)b * b
  return r;
}

#define REGISTER_PLUGIN(NAME)
