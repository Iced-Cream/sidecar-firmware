// core/ui/page_arp.cpp - see extra.h: the ARP page on the Rytm's screen (core/arp/arp.h, docs/ARP.md). Cold. CORE_POLY builds only.
//
// The selected track's arpeggiator: A MODE, B SPD, C RNG, D GATE, E SWNG. Each turn is arp::set: played from then on and written into
// the track's sound on the Rytm (45), so it is saved with the sound and the kit. Opened by UP + FLTR (30 page 4; rytm-mods 0026) or from
// the SIDECAR MENU. An analog track: why not. A Rytm whose hello does not say it keeps the settings (caps bit 3): a warning, as they would not stay.
#if CORE_POLY
#include <string.h>

#include "arp/arp.h"
#include "machines.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

UI_ROM const char* const kTrackNames[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
UI_ROM const char* const kLab[5] = {"MODE", "SPD", "RNG", "GATE", "SWNG"};
UI_ROM const char* const kLong[5] = {"MODE", "SPEED", "RANGE", "GATE", "SWING"};
constexpr int kInfoY = 12, kRow0 = 28;
constexpr int kCapsArp = 8;   // the hello's caps bit: 0026 stores 45

CORE_RAM2 int touched = -1;

CORE_COLD int track_now() {
  uint32_t g = 0;
  int t = remote_selected_track(&g);
  return t >= 0 && t < 12 ? t : 0;
}
CORE_COLD void value_text(const arp::Cfg& c, int e, char* b, int cap) {
  switch (e) {
    case 0: sniprintf(b, cap, "%s", arp::mode_name(c.mode)); break;
    case 1: sniprintf(b, cap, "%s", arp::speed_name(c.speed)); break;
    case 2: sniprintf(b, cap, CORE_STR("%d OCT"), c.range + 1); break;
    case 3: sniprintf(b, cap, CORE_STR("%d%%"), arp::gate_pct(c.gate)); break;
    default: sniprintf(b, cap, CORE_STR("%d%%"), arp::swing_pct(c.swing)); break;
  }
}
CORE_COLD void arp_draw(Canvas& c, int page, int npages, uint32_t ms) {
  (void)ms;
  c.clear();
  int t = track_now();
  header(c, "ARP", page, npages, kTrackNames[t]);
  int vb = remote_track_machine(t);
  if (vb == 0x7F) {
    c.put_text(kSlotFree, kRSmall, 64, 22, "ANALOG TRACK: NO ARP", kAlignCentre);
    c.put_text(kSlotFree + 1, kRSmall, 64, 30, "SIDECAR MACHINES ONLY", kAlignCentre);
    footer(c, "NO: BACK");
    return;
  }
  arp::Cfg cfg = arp::get(t);
  char info[32];
  if (!(remote_caps() & kCapsArp)) sniprintf(info, sizeof info, "OLD RYTM BUILD: NOT SAVED");
  else if (vb >= 0) sniprintf(info, sizeof info, "%s", machine_long(machine_of((uint8_t)vb)));
  else info[0] = 0;
  c.put_text(kSlotFree, kRSmall, 64, kInfoY, info, kAlignCentre);
  for (int e = 0; e < 5; ++e) {
    char v[8];
    value_text(cfg, e, v, sizeof v);
    int x = (e & 3) * kCellW + kCellW / 2, y = kRow0 + (e >= 4 ? 15 : 0);
    bool on = e == touched;
    if (on) c.fill(Rect{(int16_t)(x - 15), (int16_t)(y - 1), 30, 7}, kOn);
    c.put_text(kSlotLabels + e, kRSmall, x, y, kLab[e], kAlignCentre | (on ? kTextInvert : 0));
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, v, kAlignCentre);
  }
  if (touched >= 0) {
    char v[8], b[kTextLen + 1];
    value_text(cfg, touched, v, sizeof v);
    sniprintf(b, sizeof b, CORE_STR("%s %s:%s"), kTrackNames[t], kLong[touched], v);
    footer(c, b);
  } else {
    footer(c, "A..E: SET  NO: BACK");
  }
}
CORE_COLD int step(int v, int d, int n) {
  v += d > 0 ? 1 : -1;
  return v < 0 ? 0 : v >= n ? n - 1 : v;
}
CORE_COLD void arp_enc(int e, int d, bool) {
  if (!d || e < 0 || e > 4) return;
  int t = track_now();
  if (remote_track_machine(t) == 0x7F) return;
  touched = e;
  arp::Cfg c = arp::get(t);
  switch (e) {
    case 0: c.mode = (uint8_t)step(c.mode, d, arp::kModes); break;
    case 1: c.speed = (uint8_t)step(c.speed, d, arp::kSpeeds); break;
    case 2: c.range = (uint8_t)step(c.range, d, arp::kRanges); break;
    case 3: c.gate = (uint8_t)step(c.gate, d, arp::kGates); break;
    default: c.swing = (uint8_t)step(c.swing, d, arp::kSwings); break;
  }
  arp::set(t, c);
}
CORE_COLD bool arp_key(int code) {
  if (code == 0x42) return true;   // UP: the Sidecar's modifier
  if (code == 0x40 || code == 0x41 || code == 0x43 || code == 0x84) return true;   // the page stands alone, as POLY
  if (code == 0x45) touched = -1;  // NO: SIDECAR (remote.cpp)
  return false;
}
CORE_COLD bool arp_shown() {
  for (int t = 0; t < 12; ++t) {
    int vb = remote_track_machine(t);
    if (vb >= 0 && vb != 0x7F) return true;
  }
  return false;
}

}  // namespace

UI_ROM const ExtraPage kArpPage = {"ARP", arp_shown, arp_draw, arp_enc, arp_key, 0x1F, false, nullptr, nullptr, nullptr, true};

}  // namespace ui
}  // namespace core
#endif
