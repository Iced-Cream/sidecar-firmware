// core/fx/fx_guard.h - the measured-time guard of every effect (decided 2026-11-23: "we'll extend the methodology to
// all FX though, built-in or otherwise"; docs/fx-measured-mute.md). Plugin API 3's effect budget (core/user/um.cpp
// fx_run, docs/plugin-api-3-plan.md "The effect's budget") for the built-in effects too: each INSERT slot of every chain
// (the voices', the buses', the master's: AMP and the master's send TYPEs run inside it) and each send FX slot's built-in
// TYPE (STUTTER too) is timed on the core that runs it. CPU admission (core/device/admit.h) stays the first layer: a
// slot refused or shed plays dry and is never timed; this guard catches what the model did not foresee (a Pi without a
// fan throttled to 600 MHz, the L2 shared under load).
//
// The rules (the built-ins are measured on the bench, so a cool core never mutes one; decided 2026-11-23):
//   - one call of kHugeUs or more (the FIFO's 2 ms): muted at once, hot or not
//   - a slot over its budget (budget_us() a 12-frame block, scaled by the block's frames: API 3's 120 us) for kOverMax
//     blocks in a row, counted only while its core is hot (the core's block took 80 % or more of a block; core 3 70 %:
//     the cores report it, core_done)
//   - a core hot kOverMax blocks in a row: the dearest effect on it over those blocks (built-in or a user effect: API 3's
//     UFX1..UFX4 add their time with note_user) is muted, and the count starts again
// A muted slot fades out (the 5 ms TYPE fade), plays dry and shows NO CPU on the box (insert slots; ins::refused), its
// reason on the web page (/status "fxMuted", the user effects' beside them). It holds until the slot is set again (its
// TYPE changes, set_type) or its box attaches again (clear_box): no retry when the Pi cools, and the admission's
// regrant never brings it back (decided 2026-11-23). User effects keep um.cpp's own guard; a user effect picked as the
// dearest is muted through um's (user_mute).
//
// Cost on the render cores: two counter reads (core::cycles) per running slot per chunk; a slot OFF reads none.
// The bookkeeping is portable (the host tests drive core_done themselves: core/tests/test_fx_guard.cpp).
#pragma once
#include <stdint.h>

namespace core {
namespace fx {
namespace guard {

constexpr int kBoxes = 2;
constexpr int kInsChains = 13, kInsSlots = 2;            // ins::kChains, ins::kSlots (insert.cpp checks)
constexpr int kInsUnits = kInsChains * kInsSlots;        // 26 a box
constexpr int kSendUnits = 2;                            // FX 1, FX 2
constexpr int kPerBox = kInsUnits + kSendUnits;          // 28
constexpr int kBuiltin = kBoxes * kPerBox;               // 56 built-in units
constexpr int kUser = 8;                                 // um.cpp's kFx: each box's UFX1..UFX4
constexpr int kUnits = kBuiltin + kUser;                 // 64: one bit each in a u64
constexpr int kCores = 4;
constexpr int kOverMax = 16;                             // blocks in a row (as API 3's)
constexpr uint32_t kHugeUs = 2000;                       // one call this long mutes at once

constexpr int ins_unit(int box, int c, int s) { return box * kPerBox + c * kInsSlots + s; }
constexpr int send_unit(int box, int s) { return box * kPerBox + kInsUnits + s; }
constexpr int user_unit(int u) { return kBuiltin + u; }

// ---- the render cores
// Unit `unit` ran `ticks` (core::cycles) over `frames` frames on the calling core. Any core; each core its own row.
void note(int unit, uint32_t ticks, int frames);
uint32_t stamp();   // core::cycles() (the CPU-time counter: the Pi's CNTPCT), for note's ticks
// API 3's user effect u ran us microseconds (um.cpp fx_run, after its own checks): counts toward its core's dearest.
void note_user(int u, uint32_t us);
// A core after its block's work (voices and chains on 1 / 2; slot 0's master chain on 3): hot = it took the share
// that hurts (the Pi's cores.cpp). The per-slot and per-core rules run here, over what note() saw since the last call.
void core_done(int core, bool hot);
// The core the caller runs on (the Pi: rytm_render_core(); the host: the one set_core named, 0 by default)
int this_core();
void set_core(int c);   // host tests

// ---- what the render side reads (each block, relaxed)
bool muted(int unit);
inline bool ins_muted(int box, int c, int s) { return muted(ins_unit(box, c, s)); }
inline bool send_muted(int box, int s) { return muted(send_unit(box, s)); }
// test only (fxguard=slow:): extra busy time added inside the unit's call, us (0 none)
uint32_t slow_us(int unit);
void spin_us(uint32_t us);   // the busy wait slow_us asks for (render cores)

// ---- the control core
// Each idle pass: the mutes the render cores made, worded and logged once; a muted slot whose TYPE changed since (the
// kit, the page, the console) is released and fades in.
void poll();
void clear(int unit);                  // released (its slot set again: insert.cpp set_type)
void clear_box(int box);               // a box attached: every built-in mute of its slot released
int muted_count(int box);              // built-in units muted now, box slot box's
const char* why(int unit);             // "muted: ..." ("" not muted)
// /status: "\"fxMuted\":[...]": every muted effect, built-in and user, {"box","at","type","why"} ("at": V1 A .. M2 B,
// FX1, FX2, UFX1..UFX4). Returns the length written.
int json(char* out, int cap);
// The console: "fxguard" (the state), "fxguard=budget:<us>" (the per-slot budget, a 12-frame block), "fxguard=off|on",
// "fxguard=mute:<box>:<at>" / "fxguard=slow:<box>:<at>:<us>" (tests, the gates), "fxguard=clear". False: not ours.
bool command(const char* p, char* out, int cap);
void set_budget_us(uint32_t us);
uint32_t budget_us();
void set_enabled(bool on);   // off: nothing is muted (timing goes on; /status says so)
bool enabled();

// ---- the hooks (set at boot)
// um.cpp: mute user effect u, the dearest on core `core` (its us in the window); and its state for json (muted, why)
extern void (*user_mute)(int u, uint32_t us, int core);
extern bool (*user_state)(int u, const char** why, const char** id);
// insert.cpp / send_fx: the TYPE a unit plays as set now (the release check), and its name
int type_now(int unit);
const char* type_label(int unit, int type);

struct Stats { uint32_t mutes, huge, slow, dearest, released, user_mutes; };
const Stats& stats();
void reset();   // tests: every count, mute and setting back to its default

}  // namespace guard
}  // namespace fx
}  // namespace core
