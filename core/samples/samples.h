// core/samples/samples.h - the sample service (2026-11-10, Connor's pick for DSP SAMPLE: "a generic sample service +
// block"): the project's sample pool on the Pi's card, held in RAM for the DSP library's SAMPLE block (core/dsp/sample.h)
// and any later reader (MULTISAMPLE, CHOP, GRANULAR, the looper's takes). Nothing here knows what a Rytm mod does with
// a sample; a mod names files and plays pool entries by their index.
//
// The pool: up to kEntries card files per project (the Rytm's project ID, kv::link's NAME), each an index 0..127 that
// stays its own until it is dropped. Kept on the card as /.rytm/samples/<project, 8 hex>.lst (one line a entry:
// "<index> <path>"), read back when the project comes, so a project's sounds find their samples again by index.
//
// RAM: every listed entry is loaded whole (WAV: PCM 8/16/24/32-bit or 32-bit float, mono or stereo, any rate; kept as
// 16-bit, its channels and rate as the file's), at most kMaxFrames frames (a longer file: its start), from one arena the
// board gives (begin). Entries take the arena in order; a dropped entry's room comes back only with the next project
// (or a full reload). A file that does not fit is FULL.
//
// Instruments (2026-11-10, MULTISAMPLE and CHOP's slice maps on the library; parsed by upstream's core/msamp/sfz):
// an entry whose path ends .sfz or .elmulti is an instrument, not a sample. Loading it reads the file, lists each
// sample it names as a pool entry of its own (found by path, as SADD does; they load as any entry and stay listed),
// and keeps up to kZones zones (SFZ regions: key and velocity ranges, key centre, tuning, level, offset / end, loops,
// round robin). A reader asks zone() for a note: the entry that plays it and how. Its SNAME frames = its zones.
//
// Host link (51 STORE, version 1; the card core's, as kv's ops):
//   Rytm -> Pi  3A SADD  <seq> <owner> <path: 7-bit ASCII, 1..kPathMax, from the card's root>  answer 76
//               3B SDROP <owner> <index>                                                    no answer
//               3C SINFO <seq> <owner> <index>                                              answer 77
//               3D SADDB <seq> <owner> <folder hi lo> <file hi lo>                          answer 76
//                    a file as core/browse's listing (0E) named it: the Pi finds its whole path (names in a listing
//                    are cut); code 9 if the folder or file is not there (any more), 5 if the path is past kPathMax
//   Pi -> Rytm  76 SINDEX <seq> <owner> <index | 7F> <code>   code: 0 added (or already listed: its index), 3 the pool
//                                                              is full, 5 bad path, 9 no such file
//               77 SNAME  <seq> <owner> <index> <state> <frames: 4 x 7 bits> <name: up to 16 chars, the file's name>
// owner is the asker's (for the answer); the pool is the project's, one for every mod.
//
// Threads: card_op / card_poll / on_project on the card core (core 0's loop; the Mac and tests: the caller); take_answer
// on the control core (host_step); get() on the render side, lock-free: an entry is published (state READY) only once
// its frames are all in RAM, and its memory is reused only kRetireMs after it was unpublished, long after any render
// call that read it has returned. Portable C++17 (core/platform.h's File).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core {
namespace samples {

constexpr int kEntries = 128;
constexpr int kPathMax = 96;
constexpr uint32_t kMaxFrames = 8u << 20;   // a sample's frames in RAM at most (175 s at 48 kHz)
constexpr uint32_t kRetireMs = 50;          // an unpublished entry's memory is reused no sooner than this
enum Op : uint8_t { kAdd = 0x3A, kDrop = 0x3B, kInfo = 0x3C, kAddRef = 0x3D, kIndex = 0x76, kName = 0x77 };
enum Code : uint8_t { kOk = 0, kFull = 3, kBad = 5, kMissing = 9 };
enum State : uint8_t { kEmpty = 0, kListed, kLoading, kReady, kFailed, kTooBig };

bool is_samples_op(uint8_t op);

// the board's RAM for the samples (any alignment; 2-byte aligned inside) and the folder for the pool lists
bool begin(void* arena, size_t bytes, const char* lists = "/.rytm/samples");
bool ready();

// card core: a whole 51 message with a samples op (the answer is queued); false: malformed
bool card_op(const uint8_t* m, int n);
// card core: one step of the work (a project's list read, a file opened, or one card read of kChunk): false if idle
constexpr uint32_t kChunk = 32768;
bool card_poll();
// any core: the project changed (kv::link's NAME); its list is read on the card core's next card_poll
void on_project(uint32_t project);
// control core: the next queued answer (76 / 77), whole F0..F7; 0: none
int take_answer(uint8_t* out, int cap);

// render side: entry i's sample, if READY. pcm: interleaved 16-bit, ch 1 or 2; rate the file's (Hz)
struct View {
  const int16_t* pcm;
  uint32_t frames;
  uint8_t ch;
  float rate;
  uint32_t clm;   // a 'clm ' chunk's frame size (a wavetable's, Serum's "<!>2048"), 0 none
};
bool get(int i, View* v);
// render side: entry i is a READY instrument and one of its zones plays (note, vel): which entry (a sample's: get()
// it), and how. Round robin (seq_length) moves on each call that matched. False: not an instrument, no zone.
constexpr int kInstruments = 16;
constexpr int kZones = 128;
struct Zone {
  uint8_t entry, keycenter, loop;   // loop: 1 loops loop_start..loop_end (SFZ loop_continuous / loop_sustain)
  float cents;                      // transpose x 100 + tune
  float keytrack;                   // semitones a key (pitch_keytrack / 100)
  float gain;                       // amplitude x volume
  float veltrack;                   // 0..1 (amp_veltrack / 100)
  uint32_t offset, end, loop_start, loop_end;   // frames; end / loops kNone: the sample's own
};
constexpr uint32_t kNone = 0xFFFFFFFFu;
bool zone(int i, int note, int vel, Zone* z);
bool instrument(int i);

// diagnostics (/status, tests)
struct Stats {
  uint32_t adds, drops, loads, fails, bytes_used, bytes_total, project;
  int listed, ready;
};
Stats stats();
State state(int i);
const char* path(int i);   // card core / tests

void reset_for_tests();

}  // namespace samples
}  // namespace core
