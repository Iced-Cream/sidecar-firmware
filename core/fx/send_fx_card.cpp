// core/fx/send_fx_card.cpp - send_fx.h's card files: /.rytm/sendfx.txt (global: the route) and
// /.rytm/fxslots/<project>.txt (the project's user-FX slot map). Cold: boot, a project change, the idle step.
#include "fx/send_fx.h"
#include "pool/pool.h"

namespace core {
namespace fx {

static const char kGlobal[] = "/.rytm/sendfx.txt";

// one line (without the '\n') from f; false at the end
static CORE_COLD bool line(File& f, char* out, int max) {
  int n = 0, c;
  bool any = false;
  while ((c = f.read()) >= 0) {
    any = true;
    if (c == '\n') break;
    if (c != '\r' && n < max - 1) out[n++] = (char)c;
  }
  out[n] = 0;
  return any;
}

CORE_COLD void card_load() {
  File f;
  control.route = kRouteSlot;
  if (!f.open(kGlobal, kRead)) return;
  char b[64];
  while (line(f, b, sizeof b))
    if (strcmp(b, "out=main") == 0) control.route = kRouteMain;
    else if (strcmp(b, "out=fx") == 0) control.route = kRouteFx;
  f.close();
  log(CORE_STR("sendfx: out %s"), control.route == kRouteMain ? "forced main" : control.route == kRouteFx ? "forced fx" : "per slot (P1)");
}

CORE_COLD void card_save() {
  card_dirty = false;
  File f;
  if (!f.open(kGlobal, kWrite)) {
    log(CORE_STR("sendfx: cannot write %s"), kGlobal);
    return;
  }
  const char* s = control.route == kRouteMain ? "out=main\n" : control.route == kRouteFx ? "out=fx\n" : "out=slot\n";
  f.write(s, strlen(s));
  f.close();
}

static CORE_COLD int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

CORE_COLD void slots_load(const char* project) {
  for (int u = 0; u < kUserSlots; ++u) user[u].tag = 0, user[u].name[0] = 0;
  char fn[72], safe[48];
  pool_file(safe, sizeof safe);   // "/.rytm/pools/<safe>.txt": the same safe name
  sniprintf(fn, sizeof fn, "/.rytm/fxslots/%s", safe + 13);
  (void)project;
  File f;
  if (!f.open(fn, kRead)) return;
  char b[64];
  for (int u = 0; u < kUserSlots && line(f, b, sizeof b); ++u) {
    int t = 0, i = 0;
    for (; i < 4 && hexv(b[i]) >= 0; ++i) t = t << 4 | hexv(b[i]);
    if (i != 4 || b[4] != ' ' || (t & 0x3FFF) == 0) continue;
    user[u].tag = (uint16_t)(t & 0x3FFF);
    strncpy(user[u].name, b + 5, sizeof user[u].name - 1);
    user[u].name[sizeof user[u].name - 1] = 0;
  }
  f.close();
}

}  // namespace fx
}  // namespace core
