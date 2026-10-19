// core/engine.cpp - see engine.h. Boot and between-requests work: cold.
#include "engine.h"

#include "fx/send_fx.h"
#include "lfo.h"

namespace core {

CORE_COLD bool engine_begin() {
  // Plaits' scratch (128 KB, before the voices take it) sorts an index rebuild
  card_ok = cards.begin(log, shared, sizeof shared[0] * kVoices);   // (a pointer on CORE_HEAP_BUFFERS)
  sample_ram_init();
  if (card_ok) junolib.begin(&cards.sd(), log);
  if (card_ok) dxlib.begin(&cards.sd(), log);
  if (card_ok) project_load();
  if (card_ok) lfo::load();   // the project's LFOs (core/lfo.h)
  if (card_ok) fx::card_load();
  if (card_ok) fxin::load();
  if (card_ok) sendfx_project();
  voices_init();
  return card_ok;
}

CORE_COLD bool idle_prepare() { return prepare_pending(); }

CORE_COLD bool idle_save() {
  if (fx::card_dirty) {
    fx::card_save();
    return true;
  }
  if (fxin::save()) return true;
  if (lfo::save()) return true;
  if (!project_dirty) return false;
  project_save();
  return true;
}

// With the sample RAM: a stream within ~85 ms of running dry first, then a load step (one open or one 8 KB read),
// then the streams' top-ups.
CORE_COLD void idle_stream() {
  if (psram) {
    if (!refill_neediest(4096) && !psram->step()) refill_neediest(0xffffffffu);
  } else {
    refill_neediest(0xffffffffu);
  }
}

}  // namespace core
