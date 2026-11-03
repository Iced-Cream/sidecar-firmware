// pi/engine/tests/test_tap_names.cpp - core/ui/tap_names.h, the scope's names that rec_pi.cpp (the stems) and SCOPE
// share (rc040 review L11). Built with -nostdinc -nostdinc++ (Makefile): the header must need no system header, since
// the kernel build cannot resolve ui/extra.h's <new>. The definitions here have the header's exact types: a declaration
// that changes its return type no longer compiles here, as it no longer compiles in page_scope.cpp. The rule also checks
// that rec_pi.cpp includes the header and declares nothing by hand.
#include "ui/tap_names.h"

extern "C" int puts(const char*);

namespace core {
namespace ui {
const char* tap_source_name(int src) { return src == 0 ? "MAIN" : "-"; }
int tap_channel_of_voice(int vi) { return 2 + vi; }
}  // namespace ui
}  // namespace core

int main() {
  const char* (*name)(int) = &core::ui::tap_source_name;
  int (*ch)(int) = &core::ui::tap_channel_of_voice;
  if (name(0)[0] != 'M' || ch(3) != 5) return 1;
  puts("test_tap_names: PASS");
  return 0;
}
