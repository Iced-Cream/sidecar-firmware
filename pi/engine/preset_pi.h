// pi/engine/preset_pi.h - the display's JUNO / 6-OP FM preset pages (pi/ui/sidecar_preset.h) filled from the preset index and
// the project's lists (core/presets/preset_index.h's view: a window copied and checked against the walk; the card is not
// read) on core 0. Hosted (the core's flags).
#pragma once
#include "../ui/sidecar_preset.h"

namespace preset_pi {

// The window the page asks for (its kind, folder and first row), the tabs' row counts, the last add's word. o->gen changes
// only when something to draw changed. Call ~10 times a second while the POOL page or the preset page is shown.
void Fill(sidecar_preset_view_t* o);

}  // namespace preset_pi
