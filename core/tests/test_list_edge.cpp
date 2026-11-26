// core/tests/test_list_edge.cpp - a list's name stays on the Rytm's screen: PLAITS' 8-letter engine names in column 0
// ("GRANULAR", "HARMONIC", "WAVETABL") lost their first letter on the unit, the name centred at x 16 starting left of
// x 0 where the Rytm drops the glyph. list() keeps the name's first glyph at x >= 0 (at kRSmall's widest advance), and
// the text-fit check (Canvas::fit_report, CORE_UI_TOOLS) reports centred text that starts left of x 0.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "ui/ui.h"
#include "ui/widgets.h"

using namespace core::ui;

static int reports = 0;
static void count_report(const char*, int, int) { ++reports; }

int main() {
  static Canvas c;
  static const char names[] = "GRANULAR\0HARMONIC\0WAVETABL\0PHASEDST\0VA\0";
  for (int sel = 0; sel < 5; ++sel)
    for (int col : {0, 3}) {
      list(c, cell(col, 0), names, sel, 0);
      const TextItem& t = c.txt[0];
      int w = (int)strlen(t.s) * kRSmall.advance - 1;
      CHECK(!strcmp(t.s, list_name(names, sel)));
      CHECK(text_left(t, w) >= 1 && text_left(t, w) + w <= kW - 1);   // on the screen with a pixel to spare, both edges
      if (col == 0 && sel == 4) CHECK(t.x == 16);                 // a short name: centred in its cell as before
    }
  // the check: a centred 8-letter name at x 16 (as list() drew it) is reported, the same name at x 20 is not
  auto keep = Canvas::fit_report;
  Canvas::fit_report = count_report;
  c.put_text(1, kRSmall, 16, 10, "GRANULAR", kAlignCentre);
  CHECK(reports == 1);
  c.put_text(1, kRSmall, 20, 10, "GRANULAR", kAlignCentre);
  c.put_text(1, kRSmall, 16, 10, "2-OP FM", kAlignCentre);   // 7 letters: on the unit's screen whole
  CHECK(reports == 1);
  Canvas::fit_report = keep;
  printf("test_list_edge: PASS\n");
  return 0;
}
