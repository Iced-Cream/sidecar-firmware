// core/host_link.cpp - see host_link.h. Per SysEx message, never per sample: cold.
#include "host_link.h"

#include <string.h>

#include "fx/send_fx.h"
#include "pool/pool.h"
#include "rec/rec_names.h"
#include "ui/remote.h"

namespace core {

// The send FX's TYPE list (0x21, 8 a message) and both FX states (0x22) to the Rytm.
static CORE_COLD void sendfx_names_out() {
  if (!hooks.to_rytm) return;
  uint8_t m[128];
  int total = fx::names_total();
  for (int first = 0; first < (total > 0 ? total : 1); first += 8) {
    int c = total - first < 8 ? total - first : 8;
    hooks.to_rytm(m, fx::sysex_names(m, first == 0, total, first, c));
  }
}
// Each playable TYPE's P2..P5 labels and zones (0x24), after the 0x21 list (whose replace clears the Rytm's labels).
static CORE_COLD void sendfx_labels_out() {
  if (!hooks.to_rytm) return;
  uint8_t m[160];
  for (int id = 1; id < 64; ++id)
    if (int n = fx::sysex_labels(m, id)) hooks.to_rytm(m, n);
}
static uint8_t fx_sent_id[fx::kSlots] = {0xFF, 0xFF}, fx_sent_st[fx::kSlots];
static bool s_sendfx_hello;   // names + labels + states sent since boot (asked or not)

static CORE_COLD void sendfx_state_out(bool all) {
  for (int s = 0; s < fx::kSlots; ++s) {
    uint8_t id = fx::control.knob[s][0], st = fx::state(s);
    if (!all && id == fx_sent_id[s] && st == fx_sent_st[s]) continue;
    fx_sent_id[s] = id, fx_sent_st[s] = st;
    uint8_t m[32];
    if (hooks.to_rytm) hooks.to_rytm(m, fx::sysex_state(m, s));
  }
}
static char fx_project[24] = "\x01";   // the project whose fxslots file is loaded (never a real name at boot)

CORE_COLD void sendfx_project() {
  if (strcmp(fx_project, project) == 0) return;
  strcpy(fx_project, project);
  fx::slots_load(project);
}

namespace fxin {
static const char kFile[] = "/.rytm/fxin.txt";
static bool s_want, s_dirty, s_hello, s_pending;   // setting, unsaved, first contact since boot, a 26 awaiting its 27
static int8_t s_rytm = -1;
static uint32_t s_ask_ms, s_sent_ms;   // when the oldest unanswered 26 went out (0: none); when the last went

CORE_COLD bool wanted() { return s_want; }
CORE_COLD int rytm() { return s_rytm; }
CORE_COLD bool active() { return s_rytm == 1; }
CORE_COLD const char* state() {
  if (s_rytm >= 0) return s_rytm ? "on" : "off";
  return s_ask_ms && now_ms() - s_ask_ms >= 3000 ? "unsupported" : "unknown";
}
static CORE_COLD void send26(uint8_t v) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x26, v, 0xF7};
  if (!hooks.to_rytm) return;
  hooks.to_rytm(m, (int)sizeof m);
  if (s_rytm < 0 && !s_ask_ms) s_ask_ms = now_ms() | 1;
  s_pending = true;
  s_sent_ms = now_ms();
}
CORE_COLD void ask() { send26(0x7F); }
CORE_COLD void load() {
  File f;
  s_want = false;
  if (!f.open(kFile, kRead)) return;
  char b[16] = {0};
  f.read(b, sizeof b - 1);
  f.close();
  s_want = strncmp(b, "src=fx", 6) == 0;
}
CORE_COLD bool save() {
  if (!s_dirty) return false;
  s_dirty = false;
  File f;
  if (!f.open(kFile, kWrite)) {
    log(CORE_STR("fxin: cannot write %s"), kFile);
    return true;
  }
  const char* t = s_want ? "src=fx\n" : "src=in\n";
  f.write(t, strlen(t));
  f.close();
  return true;
}
CORE_COLD void set(bool on) {
  if (on != s_want) s_dirty = true;
  s_want = on;
  send26(on ? 1 : 0);
}
// 27 <s> from the Rytm: a reply to a 26, or the unasked one at USB port-up (the cue to send the setting again)
static CORE_COLD void on_state(uint8_t s) {
  bool reply = s_pending && now_ms() - s_sent_ms < 1000;   // (a 26 sent into a closed port is no one's reply)
  s_pending = false;
  s_rytm = s ? 1 : 0;
  s_ask_ms = 0;
  rec::set_fxin(s != 0);
  if (!reply && s_want && s == 0) send26(1);   // a connect: the Rytm forgot; a 00 in reply to our 01 is not repeated
}
CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "fxin", 4) != 0 || (p[4] && p[4] != '=')) return false;
  if (p[4]) {
    const char* v = p + 5;
    if (v[0] == '1' && !v[1]) set(true);
    else if (v[0] == '0' && !v[1]) set(false);
    else if (v[0] == '?' && !v[1]) ask();
    else {
      sniprintf(out, cap, CORE_STR("fxin=1|0|? (IN L/R: FX return | inputs | ask), not \"%s\""), v);
      return true;
    }
  }
  sniprintf(out, cap, CORE_STR("fxin: IN L/R source %s, the Rytm %s%s"), s_want ? "FX return" : "inputs", state(),
            strcmp(state(), "unsupported") == 0 ? " (not supported by this Rytm build)" : "");
  return true;
}
CORE_COLD int json(char* out, int cap) {
  return sniprintf(out, cap, CORE_STR("\"fxin\":{\"want\":%d,\"state\":\"%s\",\"rytm\":%d}"), s_want, state(), s_rytm);
}
// the first message since boot is the host's first contact: tell the Rytm the setting (or ask)
static CORE_COLD void contact() {
  if (s_hello) return;
  s_hello = true;
  if (s_want) send26(1);
  else send26(0x7F);
}
}  // namespace fxin

CORE_COLD void link_reset(const char* why) {
  for (int vi = 0; vi < kVoices; ++vi) voice_silence(vi);
  if (fx::all_sound_off()) log(CORE_STR("link %s: voices silenced, send FX cleared"), why);
}

__attribute__((noinline)) CORE_COLD void host_sysex(const uint8_t* d, int len) {
  // F0 7D 52 54 cmd voice ...
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54 && d[4] == 0x27) {   // FXIN_STATE
    if (len >= 7 && d[5] <= 1) fxin::on_state(d[5]);
    return;
  }
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54) fxin::contact();
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54 && d[4] == 0x10) {   // the project
    if (hooks.project_seen) hooks.project_seen();   // (the Teensy's plugins: a 1D owed, rate-limited)
    else slots_1d(1000);
    set_project(d + 5, len - 5);
    if (strcmp(fx_project, project) != 0) {   // the user-FX slot map follows the project
      sendfx_project();
      sendfx_names_out();
      sendfx_labels_out();
      sendfx_state_out(true);
    }
    return;
  }
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54) {
    if (d[4] == 0x19) {   // ASK (user slots)
      if (hooks.slot_ask) hooks.slot_ask(d, len);
      else slots_ask(d, len);
      return;
    }
    if (d[4] == 0x1E) {   // ASSIGN (user slots)
      if (hooks.slot_assign) hooks.slot_assign(d, len);
      else slots_assign(d, len);
      return;
    }
    if (d[4] == 0x25) {   // SLOT PRINTS (user slots: which machines the project's sounds were made for)
      if (hooks.slot_prints) hooks.slot_prints(d, len);
      return;
    }
    if (d[4] == 0x20) {   // SENDFX (send levels, FX TYPE + params, tags): a TYPE or state change answered by 0x22
      // The Rytm asks (0x23) only until it has a name list, so after this engine (re)starts it may never ask again:
      // the first 0x20 since boot, with no ASK seen yet, sends the names, labels and states unasked, as an ASK would.
      if (!s_sendfx_hello) {
        s_sendfx_hello = true;
        sendfx_names_out();
        sendfx_labels_out();
        sendfx_state_out(true);
      }
      if (fx::take_sendfx(d, len) && d[6] >= 12) sendfx_state_out(false);
      return;
    }
    if (d[4] == 0x23) {   // SENDFX ASK: the TYPE list and both states
      s_sendfx_hello = true;
      sendfx_names_out();
      sendfx_labels_out();
      sendfx_state_out(true);
      return;
    }
    if (d[4] == 0x35) link_reset("hello");   // the Rytm's USB port came up: its transport and notes unknown
    if ((d[4] >= 0x30 && d[4] <= 0x3F) || d[4] == 0x29 || d[4] == 0x2A || d[4] == 0x2B) {   // the remote UI (core/ui/remote.h; 29 / 2A the SRC view)
      ui::remote_sysex(d, len);
      return;
    }
    if (d[4] == 0x09 || d[4] == 0x0B || d[4] == 0x0C) return;   // the retired folder walk
    if (d[4] == 0x13) { send_pool(); send_juno_names(); return; }
    if (d[4] == 0x18) { send_fm_names(); return; }
    if (d[4] == 0x0D && len >= 8) { send_listing(d[5] << 7 | d[6]); return; }
    if (d[4] == 0x0F && len >= 10) { pool_add(d[5] << 7 | d[6], d[7] << 7 | d[8]); return; }
    if (d[4] == 0x12 && len >= 11) { pool_assign(d[5], d[6] << 7 | d[7], d[8] << 7 | d[9]); return; }
  }
  if (len < 7 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return;
  int cmd = d[4], vi = d[5] & 7;
  if (cmd == 0x11) {   // NOTE_OFF: the voice's gate closed (plain voice, no flags)
    voice_note_off(vi);
    return;
  }
  voice_report(cmd, d, len);
}

}  // namespace core
