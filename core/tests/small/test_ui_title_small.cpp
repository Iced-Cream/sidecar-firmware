// core/tests/small/test_ui_title_small.cpp - the Teensy build's status page title is "SIDECAR MINI" (RYTM_SENDFX_LARGE=0),
// and it fits the title bar beside the page dots: 2 px margin + 12 characters at the Rytm font's 5 px advance, dots from x = 78.
#include <string.h>

#include "check.h"
#include "ui/remote.h"

static_assert(!RYTM_SENDFX_LARGE, "build with -DRYTM_SENDFX_LARGE=0");

int main() {
  CHECK(!strcmp(core::ui::kStatusTitle, "SIDECAR MINI"));
  CHECK(2 + (int)strlen(core::ui::kStatusTitle) * core::ui::kRSmall.advance <= 128 - 2 - 12 * 4);
  puts("test_ui_title_small: ok");
  return 0;
}
