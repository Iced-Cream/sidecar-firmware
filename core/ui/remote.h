// core/ui/remote.h - the Remote UI host: the Sidecar draws pages on the Rytm's screen (rytm-mods 0026-remote-ui, a dumb
// terminal: a 1 KB shadow, tiles diffed to the panel at its 20 Hz UI tick, keys and encoders reported while the page is
// open). Protocol 0x30..0x3F, every message F0 7D 52 54 <cmd> <ver 1> ... F7 (sim/protocol.py, core/ui/README.md):
//   Rytm -> host  30 <page> <on> <reason>   (rui1026: page 0 the default (SIDECAR), 1 the LFO page: TRK + LFO, also
//                 again while open; a close always page 0)
//                 31 <key >> 7> <key & 0x7f> <down>   32 <enc> <delta + 64> <func>
//   host -> Rytm  38 <seq> <col0> <page0> <ncols> <npages> <packed>   39 <seq>   3A <page> <on>   3C
//                 1021: 3D <seq> <slot> <x> <y> <font> <flags> <chars> (text the Rytm draws in its own font, committed by
//                 39), 3E <seq> (every text slot empty), 3F <enc> <slot> <value> <lo> <hi> <flags> <prefix> (local echo)
//                 1022: 37 <e0 e1 e2> <k0 ..> the catch mask: the keys and encoders the page takes (the rest work on the
//                 Rytm as on its stock screen; RECORD / PLAY / STOP always there), sent before a frame when it changed
//                 1023: 36 <ticks> the connect splash: the last committed frame shown with the page closed for ticks x
//                 50 ms, every key and knob staying the Rytm's (0: end it); the answer to the Rytm's hello
//   host -> Rytm  rui1026: 34 1 <c1..c16> the trig keys' LEDs while a page shows (palette 0..111: ramps of 16, red 0,
//                 green 16, blue 32, amber 48, cyan 64, magenta 80, white 96; a ramp's entry 0 off), 34 0 = stock again
//   Rytm -> host  rui1026: 3B <kit 0..127> the active kit, after each hello and when it changes (the LFOs
//                 are per kit: core/lfo.h kit_changed)
//   Rytm -> host  1027 (provisional): 29 <track> <on> the stock SRC page of track t shown / left (again after each
//                 hello); 2A <track> <first> <n> {<int> <frac>} x n its P<first+1>.. as the stock page has them (P1..P8
//                 SRC A..H, P9..P16 SMPL; all 16 after each 29 t 1). The host answers a 29 t 1 with a whole frame
//                 (the Rytm shows it from that 39 on, while 3C keeps coming) or nothing (no page: stock; 3A 0 0 when
//                 a page it drew is gone). The view only draws: no 37, no 3F, no 34; the TRK + YES page comes first.
//   Rytm -> host  1023: 35 <uptime >> 7> <uptime & 0x7F> <caps> hello when its USB port came up (uptime in 50 ms ticks):
//                 the splash (3 s: the product, CONNECTED, voices and FX, the address) when the Rytm was up >= 3 s
// Pages: SIDECAR (tracks, send FX), SYSTEM (U1..U5, recorder, Overbridge, cores, network), then one per voice whose
// machine has a page (page.h's machine table; not once the Rytm has sent a 29 / 2A / 2B, rui1027+: its SRC page shows
// them, the SRC view), then the pages drawn by code (extra.h: SCOPE, ...). Keys: LEFT / UP the previous page, RIGHT / DOWN / PAGE the next, NO back
// to SIDECAR, NO on SIDECAR leaves (3A 0 0, as TRK + YES; a page's own NO, the LFO design view's, comes first). Encoders A..H change the page's parameters through
// the voices' own report path (a 0x03 PARAMS into voice_report, as core/notes.cpp's CCs do); an edit holds over the
// Rytm's later reports of the same value and gives way when the Rytm's value moves (its knob, a p-lock, a new sound).
// Frames: the page drawn (only widgets whose inputs changed), diffed against what the Rytm has, the changed 8x8 tiles
// sent as 38 runs and the changed text slots as 3D, then one 39; when a turn changed more than its knob, the knob's
// cell and the bottom bar go first in a commit of their own. A frame at most every 25 ms, and at once (8 ms after the
// last) after a key or a turn; 3C every 250 ms otherwise (the Rytm leaves after 1 s without). Each numeric encoder
// of a voice page is bound (3F) to the bottom bar, so the Rytm shows a turn's value before the round trip.
// All of it runs on the control side (where engine_sysex runs; the Pi's core 3): remote_sysex from host_sysex,
// remote_tick from the platform's loop. Cold; nothing while the page is closed but one compare per tick.
#pragma once
#include <stdint.h>

#include "fx/send_fx.h"   // RYTM_SENDFX_LARGE: 0 on the Teensy build
#include "ui/page.h"

namespace core {
namespace ui {

// The status page's title: "SIDECAR MINI" on the Teensy build (Sidecar Mini; 12 characters at 5 px = 60 px, clear of the 12
// page dots from x = 78), "SIDECAR" on the Pi and the Mac.
#if RYTM_SENDFX_LARGE
inline constexpr const char kStatusTitle[] = "SIDECAR";
#else
inline constexpr const char kStatusTitle[] = "SIDECAR MINI";
#endif

void remote_sysex(const uint8_t* d, int len);   // a whole 0x30..0x3F message from the Rytm
void remote_tick(uint32_t ms);                  // every pass of the platform's control loop (any ms clock)
void remote_request(bool on);                   // the host opens / closes the page (3A 0 on)
bool remote_open();

// The platform's numbers for the SYSTEM page (any time; plain words, a stale value at worst). The fields after cpu
// read 0 as "unknown", so an older initialiser ({ob, underruns, {c1, c2, c3}}) stays valid.
struct RemoteStatus {
  int8_t ob;            // the Overbridge stream runs: 1, stopped: 0, unknown: -1
  uint32_t underruns;   // the voices' output underruns since boot
  int8_t cpu[3];        // load percent of up to three cores (-1: not measured)
  uint8_t rec;          // the recorder: 0 unknown, 1 idle, 2 armed, 3 recording, 4 off (none)
  uint32_t rec_secs;    // recording: its length
  uint8_t eth, wifi;    // the network over each: 0 unknown / none, 1 down, 2 up (the Wi-Fi's name and key never)
  uint8_t ip[4];        // the network's address, 0.0.0.0 none
  char user[5][9];      // U1..U5 as the Rytm shows them ("---" empty), "" unknown
};
void remote_status(const RemoteStatus& s);
void remote_legacy(bool on);   // tests: the 1020 behaviour (50 ms frames, no input path, no echo, one commit)
void remote_src_log(bool on);  // 1027: log each 29 / 2A from the Rytm and each SRC view commit (core::log)

// voice_report's hook (voices.cpp, after the report's P1..Pn are decoded, before they apply): the page's held edits for
// voice vi on machine m (voice byte & 0x78).
void remote_params(int vi, int m, float* p, int np);

struct RemoteStats {
  uint32_t opens, closes, frames, tiles, rects, bytes, keepalives, keys, encs, edits, widgets, bad;
  uint32_t texts, echoes, split;       // 3D text slots sent, 3F echo bindings, frames sent as two commits
  uint32_t masks;                      // 1022: 37 catch masks sent
  uint32_t hellos, splashes;           // 1023: the Rytm's 35s, connect splashes shown
  uint32_t machines;                   // 1025: the Rytm's 33s (the tracks' machines)
  uint32_t leds;                       // 1026: 34s sent (the trig keys' LEDs)
  uint32_t src_views, src_vals, src_frames;   // 1027: the Rytm's 29s and 2As, SRC view frames committed
  uint32_t draw_us_max, draw_us_sum;   // the time a tick's draw + diff + send took (when it drew)
  uint8_t page, voice;                 // the current page: 0 SIDECAR, 1 a voice page (voice 0..7), 2 SYSTEM, 3 extra,
                                       // 4 the SRC view (voice = the track 0..11)
};
const RemoteStats& remote_stats();
const uint8_t* remote_frame();   // the last frame sent (what the Rytm's shadow holds), 1024 B in its layout
void preview_sent(uint8_t out[kBytes]);   // that frame with its text slots in the host's font (preview())
int sent_texts(uint8_t* out, int max);    // its text items, 36 B each (x, y, font, flags, 32 chars); how many
void remote_reset();             // tests: closed, statistics and held edits cleared
int remote_track_machine(int t);  // 1025: track t's voice byte from the Rytm's 33 (0x7F not a Sidecar machine), -1 none
void remote_forget_voices();     // no voice reported by the Rytm yet (after the platform's own boot bench): no voice pages
// The renderer and the tests (core/host/core_c.cpp): page `which` (0 SIDECAR, 4 SYSTEM from the live status, 1 voice
// `voice`'s machine page from its P1..P16, 2 / 3 the widget gallery from vals[16] with a synthetic sample, 5 the connect
// splash at vals[0] x 25 ms, 6 + k the page drawn by code k: LFO, REC, SCOPE) drawn whole into c;
// false when there is no such page. remote_bench: the mean us of a whole page drawn and diffed (full) and of one
// parameter change redrawn and diffed (one), over n rounds.
#if CORE_UI_TOOLS   // the Mac build (core/Makefile)
bool remote_render(int which, int voice, const uint8_t* vals, Canvas& c);
void remote_bench(int which, int voice, int n, float* full_us, float* one_us);
#endif

}  // namespace ui
}  // namespace core
