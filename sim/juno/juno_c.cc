// C entry points for the simulator (sim/junovoice.py).
#include "juno_voice.h"

extern "C" {
void* juno_new(float sr) {
  juno::Voice* v = new juno::Voice();
  v->Init(sr);
  return v;
}
void juno_free(void* h) { delete (juno::Voice*)h; }
void juno_knobs(void* h, const float* k) { ((juno::Voice*)h)->SetKnobs(k); }
void juno_note(void* h, float note, float vel) { ((juno::Voice*)h)->NoteOn(note, vel); }
void juno_note_off(void* h) { ((juno::Voice*)h)->NoteOff(); }
void juno_render(void* h, float* out, int n) {
  for (int i = 0; i < n; i += 12) ((juno::Voice*)h)->Render(out + i, n - i < 12 ? n - i : 12);
}
const char* juno_name(void* h) { return ((juno::Voice*)h)->name(); }
// A patch from the project's JUNO list (18 bytes, a name); b null: PAT's
// factory patch again.
void juno_use(void* h, const uint8_t* b, const char* name) { ((juno::Voice*)h)->UsePatch(b, name); }
// Factory patch p's 18 bytes.
void juno_factory(int p, uint8_t* out) { memcpy(out, juno::kPatches[p & 127].b, 18); }
}
