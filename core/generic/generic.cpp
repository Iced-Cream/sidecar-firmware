// core/generic/generic.cpp - see generic.h.
#include "generic/generic.h"

#include <string.h>

#include "clock/clock.h"
#include "core_port.h"
#include "device/device.h"
#include "fmt/fmt.h"
#include "hooks.h"
#include "platform.h"
#include "proc/proc.h"
#include "screen/screen.h"

namespace core {
// core/host_link.h's link reset, which core/device calls when another box takes the link: here, the generic listeners
void link_reset(const char* why);

namespace gen {
namespace {
constexpr int kListeners = 4;
ProjectFn g_proj_fn[kListeners];
LinkFn g_link_fn[kListeners];
StoreFn g_store;
char g_project[24];
Stats g_st;
uint32_t g_reset_ms;
bool g_reset_any;
template <class F> bool add(F* tab, F f) {
  if (!f) return false;
  for (int i = 0; i < kListeners; ++i) if (tab[i] == f) return true;
  for (int i = 0; i < kListeners; ++i) if (!tab[i]) { tab[i] = f; return true; }
  return false;
}
void set_project(const uint8_t* d, int len) {
  char n[sizeof g_project];
  int k = 0;
  for (int i = 0; i < len && k < (int)sizeof n - 1 && d[i] != 0xF7 && d[i]; ++i) n[k++] = (char)(d[i] & 0x7F);
  n[k] = 0;
  ++g_st.projects;
  if (!strcmp(n, g_project)) return;
  memcpy(g_project, n, sizeof n);
  log("project: \"%s\"", g_project);
  for (ProjectFn f : g_proj_fn) if (f) f(g_project);
}
}  // namespace

bool begin() {
  Fs fs;
  if (!fs.begin()) return false;
  device::boot();      // first: every read below is of this box's state folder (core/device/device.h)
  fmt::sweep_boot();   // every atomic write's .prev / .tmp left by a power cut
  return true;
}

void sysex(const uint8_t* d, int len) {
  ++g_st.messages;
  uint8_t box[device::kMaxMessage];
  if (device::foreign() && !device::translate(device::g_link, d, len, box)) return;
  if (len < 6 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) {
    ++g_st.dropped;
    return;
  }
  uint8_t cmd = d[4];
  if (cmd == device::kCmdHello) { device::hello(device::g_link, d, len); return; }
  if (cmd == 0x10) { set_project(d + 5, len - 5); return; }
  if (cmd == 0x35) {
    ++g_st.hellos;
    link_reset("hello");
  }
  if (screen::sysex(d, len)) return;
  if (clock::sysex(d, len)) return;
  if (cmd >= kStoreFirst && cmd <= kStoreLast && g_store && g_store(d, len)) return;
  ++g_st.dropped;
  g_st.last_dropped = cmd;
}

void tick(uint32_t ms) { screen::tick(ms); }

bool idle_save() {
  if (fmt::post_step()) return true;
  return device::step(true);
}

const char* project() { return g_project; }
bool on_project(ProjectFn f) { return add(g_proj_fn, f); }
bool on_link(LinkFn f) { return add(g_link_fn, f); }
void set_store(StoreFn f) { g_store = f; }
Stats stats() { return g_st; }
}  // namespace gen

// Rate-limited as the engine's was (100 ms): an attach's several edges reset once
void link_reset(const char* why) {
  uint32_t ms = now_ms();
  if (gen::g_reset_any && ms - gen::g_reset_ms < 100) return;
  gen::g_reset_any = true, gen::g_reset_ms = ms;
  clock::reset();
  proc::link_reset();
  for (gen::LinkFn f : gen::g_link_fn) if (f) f(why);
}
}  // namespace core
