// core/browse/browse.h - the card browse service (2026-11-04, the engine strip-down): the Rytm asks for a folder of the
// Pi's card and gets its entries, as rytm-mods 0023 shows them (the TEENSY folder in the Rytm's own file browser). Only
// the listing: nothing is loaded and nothing on the card changes (0023's LOAD TO PROJ, 0F, has no Pi side here).
//
//   Rytm -> Pi  0D <folder hi lo>
//   Pi -> Rytm  0E <folder hi lo> <parent hi lo> <first hi lo> <total hi lo>, then per entry <kind> <id hi lo>
//               <size 4 x 7 bits, high first> <name length> <name>; six entries a message, one message for an empty
//               folder (total 0) so the Rytm, which waits for the answer, never waits for nothing
//
// Folder 0 is the root (/Samples unless set_root says otherwise). A folder's number is given the first time its parent
// is listed and kept until the reboot (the Rytm keeps numbers it was given); kMaxFolders at most, a folder past that,
// or one whose name is longer than kPathName, is left out (counted). Entries: the subfolders, then the files, each
// sorted by name (case ignored); kind 1 a folder (id: its number), kind 0 a file (id: its place among the files, size
// in bytes). Names starting with '.' are left out; a name is cut to kName characters, other bytes than ASCII shown as
// '?'. kMaxEntries a folder at most (the rest left out, counted).
//
// Threads: sysex() on the control side (the Pi's core 3); card_step() on the card core (core 0, the card queue's),
// which reads the folder kStepEntries at a time; host_step() on the control side again, which sends the answer. With
// no card core (cardq::worker() off: the Mac's tests) host_step runs card_step itself. A newer ask drops the answer
// still being read or sent. Portable.
#pragma once
#include <stdint.h>

namespace core {
namespace browse {

constexpr uint8_t kCmdList = 0x0D, kCmdListing = 0x0E;
constexpr int kMaxFolders = 1024;   // 0023's ids carry 10 bits of folder
constexpr int kMaxEntries = 512;    // 0023's cache
constexpr int kName = 23;
constexpr int kPathName = 96;       // a folder's own name, kept to find it again
constexpr int kPerMessage = 6;
constexpr int kStepEntries = 32;

void begin(bool card);                     // at boot: false answers every ask with an empty folder
void set_root(const char* path);           // before the first ask (default "/Samples")
bool sysex(const uint8_t* d, int len);     // 0D taken (true); anything else false
int card_step();                           // card core: entries read this call
int host_step(int max_msgs);               // control: messages sent

struct Stats { uint32_t asks, listings, entries, messages, superseded, errors, left_out; int folders; };
Stats stats();
}  // namespace browse
}  // namespace core
