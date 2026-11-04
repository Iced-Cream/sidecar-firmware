// pi/engine/preset_pi.cpp - see preset_pi.h.
#include "preset_pi.h"

#include <string.h>

#include "platform.h"
#include "presets/preset_index.h"

namespace preset_pi {

namespace {
uint32_t mix(uint32_t h, uint32_t x) { return (h ^ x) * 16777619u; }
}  // namespace

void Fill(sidecar_preset_view_t* o) {
  namespace P = core::presets;
  static char names[PRE_ROWS][P::kViewName];
  static uint8_t flags[PRE_ROWS];
  static int16_t kids[PRE_ROWS];
  int kind = sidecar_preset_kind() ? 1 : 0, folder = sidecar_preset_folder(), first = sidecar_preset_first();
  o->kind = kind, o->folder = folder, o->first = first;
  o->count[0] = P::rows(P::kIxJuno), o->count[1] = P::rows(P::kIxFm);
  P::View v;
  int n = P::view(folder, kind, first, PRE_ROWS, names, flags, kids, &v);
  uint32_t h = 2166136261u;
  o->ready = n >= 0, o->n = n < 0 ? 0 : n;
  o->igen = n >= 0 ? v.gen : 0;
  o->total = n >= 0 ? v.total : 0, o->up = n >= 0 ? v.up : -1;
  o->built_in = n >= 0 && v.built_in, o->full = n >= 0 && v.full;
  o->title[0] = 0;
  if (n >= 0) sniprintf(o->title, sizeof o->title, "%s", v.title);
  for (int i = 0; i < o->n; ++i) {
    sniprintf(o->name[i], PRE_NAME, "%s", names[i]);
    o->flags[i] = flags[i], o->kid[i] = kids[i];
    for (const char* q = o->name[i]; *q; ++q) h = mix(h, (uint8_t)*q);
    h = mix(h, flags[i] * 977u + (uint16_t)kids[i]);
  }
  P::Result r = P::result();
  o->msg_seq = r.seq;
  P::result_text(o->msg, sizeof o->msg);
  h = mix(h, (uint32_t)(kind + 1) * 31u + (uint32_t)(folder + 2) * 8191u + (uint32_t)first * 131071u);
  h = mix(h, (uint32_t)o->ready | (uint32_t)o->total << 1 | (uint32_t)o->built_in << 20 | (uint32_t)o->full << 21);
  h = mix(h, o->igen * 7u + r.seq);
  h = mix(h, (uint32_t)o->count[0] * 977u + (uint32_t)o->count[1]);
  for (const char* q = o->title; *q; ++q) h = mix(h, (uint8_t)*q);
  o->gen = h | 1;
}

}  // namespace preset_pi
