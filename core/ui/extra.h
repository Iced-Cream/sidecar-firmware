// core/ui/extra.h - the remote UI's pages drawn by code rather than declared (page.h): SCOPE (core/ui/page_scope.cpp)
// and REC (page_rec.cpp), after the voice pages in the page order (remote.cpp: SIDECAR, SYSTEM, the voice pages, then
// these; LEFT on SIDECAR wraps to the last). Each draws its whole page every frame; the frame diff sends only the
// tiles that moved. And the Overbridge input tap they read: the Rytm's 12 IN channels as per-entry min / max of the
// selected source and per-channel peaks, written by the IN path (compares and stores only), reduced and drawn here on
// the control side (core/ui/README.md, "Live pages").
#pragma once
#include <stdint.h>

#include "fx/send_fx.h"   // RYTM_SENDFX_LARGE: 0 on the Teensy build
#include "ui/page.h"
#include "ui/ui.h"

namespace core {
namespace ui {

struct ExtraPage {
  const char* title;
  bool (*shown)();                                              // in the page list now (null: always)
  void (*draw)(Canvas& c, int page, int npages, uint32_t ms);  // the whole page (header, body, bottom bar)
  void (*enc)(int e, int delta, bool func);                    // a turn of one of its encoders
  bool (*key)(int code);                                       // a key pressed; true: the page's own, not navigation
  uint8_t encs;                                                // the encoders it takes (bit e: A = bit 0)
  bool tap;                                                    // it reads the input tap (tap_in runs while it shows)
  uint8_t (*encs_now)();                                       // the encoders it takes now, when they vary (null: encs)
  uint16_t (*trigs_now)();                                     // the trig keys T1..T16 it takes now (bit k: T(k+1); null none)
  // 1026: the trig keys' LEDs while it shows (host -> Rytm 34, rui1026): true and c[k] for T(k+1) (palette colours,
  // kLed*), or false: stock (null: never)
  bool (*leds_now)(uint8_t c[16]);
};
// 34's palette (rytm-mods 0026 rui1026): 0..111, ramps of 16 (red 0, green 16, blue 32, amber 48, cyan 64, magenta 80,
// white 96; each ramp's entry 0 is off). The REC page: recorded blue (47), not recorded orange (52), unused off.
enum : uint8_t { kLedOff = 0, kLedBlueDim = 43, kLedBlue = 47, kLedOrange = 52, kLedA = kLedBlue, kLedB = kLedOrange };
inline uint8_t extra_encs(const ExtraPage& x) { return x.encs_now ? x.encs_now() : x.encs; }
int extra_pages();                    // how many there are (shown or not)
const ExtraPage& extra_page(int i);   // in the page order

// ---- the Overbridge input tap. Channels as the Rytm sends them (ob_block.h): 0 / 1 MAIN L / R, 2 + t track t's voice
// (BD SD RS BT LT MT CH CY), 10 / 11 AUDIO IN L / R; int32 full scale.
constexpr int kTapCh = 12;
constexpr int kTapFrames = 8;   // frames an entry spans (6 kHz of min / max pairs)
#if RYTM_SENDFX_LARGE
constexpr int kTapRing = 2048;  // entries (8 KB: 341 ms)
#else
constexpr int kTapRing = 512;   // the Teensy: 2 KB of RAM2 (85 ms: more than a scope update's worth)
#endif
// The IN path (the Pi's IN completion on core 0, the Teensy's USB host interrupt): n frames, channel c of frame i at
// f[i * stride + c] (stride >= kTapCh). Nothing while no page reads it.
extern volatile bool tap_enabled;
inline bool tap_on() { return tap_enabled; }
void tap_in(const int32_t* f, int n, int stride);
void tap_enable(bool on);
// The scope's source: 0 MAIN (L + R) / 2, 1..8 Sidecar voice 1..8 (its track's channel), 9 / 10 AUDIO IN L / R.
constexpr int kTapSources = 11;
void tap_source(int src);
const char* tap_source_name(int src);   // "MAIN" "BD" "SD" "RS/CP" .. "CY/CB" (track order) "IN L" "IN R"
int tap_channel_of_voice(int vi);       // the IN channel of Sidecar voice vi (0..7)
// The peaks since the last call, |x| >> 16 (0..32767), channel order; the entries since *rd as (min, max) >> 16, at most
// max, oldest first (an overrun skips to the oldest still held); returns how many.
void tap_take_peaks(uint16_t out[kTapCh]);
int tap_read(uint32_t* rd, int16_t (*out)[2], int max);
uint32_t tap_write_pos();
void tap_reset();   // tests

// ---- meters (SCOPE and REC): 12 vertical bars (MAIN L R, voices 1..8, IN L R) in rows y..y+h-1, peak hold, labels
// under them in text slots from slot0 (4 used). Levels in dB x 2 below full scale (0 = 0 dBFS, kMeterFloor = none).
constexpr int kMeterFloor = 96;   // -48 dB
struct Meters {
  uint8_t level[kTapCh], hold[kTapCh];   // dB x 2 below full scale (smaller = louder)
  uint32_t hold_ms[kTapCh], last_ms;
  Meters() { reset(); }
  void reset();
  void update(const uint16_t peaks[kTapCh], uint32_t ms);   // a new peak set: rise at once, fall 24 dB/s; hold 1 s
  void draw(Canvas& c, int y, int h, int slot0) const;
  static int x(int i);   // meter i's left column (0..11: MAIN L R, voices 1..8, IN L R)
  bool main_right = false;   // 1026 (REC): MAIN drawn between voice 8 and IN, under T9
  int pos(int i) const;      // meter i's left column in this layout
};
int db2_below(uint32_t peak);   // |x| >> 16 -> dB x 2 below full scale, 0..kMeterFloor

// ---- the recorder (REC, page_rec.cpp): its state from the platform (the Pi's rec_pi.cpp, core 0, every 200 ms and
// after each request; any core), the page's requests to it (the platform takes them on the core that controls it)
enum RecState : uint8_t { kRecNone, kRecIdle, kRecArmed, kRecRecording, kRecFinishing };
enum RecRequest : int { kRecNoRequest, kRecStart, kRecStop, kRecArm, kRecDisarm };
struct RecInfo {
  uint8_t state;           // RecState (kRecNone: no recorder - the page is not shown)
  uint32_t secs;           // the take's length (this one, or the last)
  uint32_t free_mb, total_mb, need_kbps;   // the card; the selection's data rate
  uint16_t in_mask;        // the IN channels the take records (bit c: channel c, the tap's order)
  char take[24];           // the take's folder name ("" none yet)
  char error[32];          // the recorder's last error ("" none)
  bool usb_paused;         // the platform's USB audio paused for the take (the Mini's shared memory: "USB OFF")
  bool ch_select;          // 1026: the trig keys pick the take's channels (the Pi; the Mini not until stems with PSRAM)
};
void remote_rec(const RecInfo& i);
int remote_rec_take();     // the pending request (RecRequest), taken; kRecNoRequest none
// 1026: the trig keys pressed on the REC page since the last call (bit k: T(k+1); T1..T8 the tracks' channels (the
// tap's 2..9), T9 MAIN (the master L/R), T10 / T11 IN L / R (the FX return when it is on)); the platform toggles them
// in its channel selection (not while a take records: the page ignores them then). 0: none.
uint16_t remote_rec_toggles();
// Playback on the REC page (the Pi's rec_play.h): a take's channel into the Rytm's MAIN. The platform's state (as
// RecInfo's, any core), the page's input taken by the platform: B pushed (play / stop), B turned (the channel),
// C turned (the take, 0 = the newest), D turned (the level), summed since the last take.
enum PlayState : uint8_t { kPlayNone, kPlayStopped, kPlayPlaying, kPlayStopping };   // kPlayNone: no take to play
struct PlayInfo {
  uint8_t state;           // PlayState
  uint8_t level;           // 0..127
  uint8_t ch_i, channels, take_i, takes;
  uint32_t pos_ms, len_ms; // the playhead, the channel's length
  char take[24], channel[12];
  char error[32];          // "" none
};
struct PlayInput { int toggle, channel, take, level; };
void remote_play(const PlayInfo& i);   // the first call: a player exists (the page takes B / C / D from then on)
bool remote_play_take(PlayInput* in);   // false: nothing since the last call
extern const ExtraPage kRecPage;
// ---- the LFO page (page_lfo.cpp, core/lfo.h): first of the pages drawn by code (LFO, REC, SCOPE)
extern const ExtraPage kLfoPage;
const char* voice_label(int vi, int k);   // voice vi's P(k+1) as its machine page labels it (null: none)
#if CORE_UI_TOOLS
void lfo_page_set(int sel, bool design, int step, uint32_t ms);   // tests and the renderer: the view
#endif

// ---- cells drawn by code (page.h kDraw: arg one of these, in[] its parameters; page_wave.cpp dispatches)
enum DrawId : uint8_t {
  kDrawSample,   // DSP SAMPLE's waveform: in STA END LOP MOD
  kDrawGrain,    // GRAIN's waveform and grain window: in POS (STA) SIZE (END) DENS (LOP) MOD
  kDrawEnv,      // JUNO's envelope (page_gfx.cpp): in ATK DEC REL LEN, the sustain the patch's
  kDrawFm,       // 6-OP FM's algorithm: in PAT
  kDrawCloud,    // GRAIN's cloud: in SIZE DENS
  kDrawPatch,    // JUNO's patch name: in PAT
};
uint32_t draw_key(const Cell& c, const Source& s);   // what it shows besides its inputs (PageView redraws on a change)
void draw_custom(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot);   // slot: its text item
// The machine pages declared here (page_for_machine's fallback: DSP SAMPLE), and a voice's page: GRAIN for a DSP SAMPLE
// voice whose MOD is in GRAIN's zone (with the sample RAM), else page_for_machine (remote.cpp's page list).
const Page* code_page_for_machine(int machine);
const Page* gfx_page_for_machine(int machine);   // page_gfx.cpp: JUNO, 6-OP FM
uint32_t gfx_key(const Cell& c, const Source& s);
void gfx_draw(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot);
#if CORE_UI_TOOLS
void fm_algorithm_render(int alg, Canvas& c);                 // the renderer: algorithm alg (0..31) alone
int fm_algorithm_layout(int alg, int8_t* col, int8_t* lev);   // tests: its operators' columns and levels; columns
#endif
const Page* page_for_voice(int machine, int vi, int mod = -1);   // mod: MOD (P12) 0..127 as the page reads it, -1 the voice's
// The page the SRC view draws for a page_for_voice page: DSP SAMPLE / GRAIN (sample on P9..P16, the SMPL page) get the
// SRC page's own P1..P8 under the waveform, as the Rytm's encoders there turn those; any other page itself.
const Page* src_view_page(const Page* p);
// The sample overviews: one column of the file being read (remote_tick while the page is open); tests: cleared.
void wave_idle(uint32_t ms);
void wave_reset();
const void* wave_of_slot(int slot, int* state, int* progress);  // state 0 none, 1 loading (progress columns), 2 ready, 3 failed
const void* wave_of_voice(int vi, int* state, int* progress);   // the voice's SMP (P11)
void wave_column(const void* w, int c, int* lo, int* hi);      // column c (0..127) of an overview: min / max >> 16 bits

}  // namespace ui
}  // namespace core
