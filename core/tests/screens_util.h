// core/tests/screens_util.h - helpers for the page-drawing tests (0.5.0 screens): text items in a frame, a text item
// found by its string, a frame written as a PNG (the host's render: ui::preview, no firmware data).
#pragma once
#include <cstring>
#include <string>

#include "rui_png.h"
#include "ui/ui.h"

inline int scr_items(const core::ui::Canvas& c) {
  int n = 0;
  for (int i = 0; i < core::ui::kTextSlots; ++i) n += c.txt[i].s[0] != 0;
  return n;
}
// the first item with exactly this text (null: none)
inline const core::ui::TextItem* scr_find(const core::ui::Canvas& c, const char* s) {
  for (int i = 0; i < core::ui::kTextSlots; ++i)
    if (c.txt[i].s[0] && !strcmp(c.txt[i].s, s)) return &c.txt[i];
  return nullptr;
}
inline bool scr_has(const core::ui::Canvas& c, const char* s) { return scr_find(c, s) != nullptr; }
// the frame as a PNG in $RUI_OUT (default build/rui_screens)
inline void scr_shot(const char* name, const core::ui::Canvas& c) {
  static const std::string dir = rui_png_dir("RUI_OUT", "build/rui_screens");
  uint8_t fb[core::ui::kBytes];
  core::ui::preview(c, fb);
  rui_png(dir, name, fb);
}

// a Source for the voice pages' tests: P1..P16 as set, voice 0
#include "ui/page.h"
struct ScrSource : core::ui::Source {
  int p[16] = {};
  int vi = 0;
  int param(int k) const override { return p[k & 15]; }
  int voice() const override { return vi; }
};
