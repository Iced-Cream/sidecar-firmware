// core/pool/pool.h - the card and the project's pool (moved from teensy/voiceloop/voiceloop.ino). The pool
// ("static" samples, as the Octatrack's), one per Rytm project: files added with LOAD TO PROJ (0F) or ASSIGN (12)
// in the Rytm's TEENSY folder while that project is the Rytm's (10 <name>). Kept on the card as text,
// /.rytm/pools/<PROJECT>.txt: slot N (1..127) is line N-1, the file's path under /Samples, an empty line an empty
// slot. DSP SAMPLE's SMP picks slot SMP (0 off); the Rytm gets the table as 14.
#pragma once
#include "../platform.h"
#include "../presets/dx7_lib.h"
#include "../presets/juno_lib.h"
#include "card_index.h"

namespace core {

extern card::Index cards;
extern bool card_ok;
extern jlib::Lib junolib;   // /JUNO's patches; folder j is TEENSY folder cards.folders() + j
extern dx7::Lib dxlib;      // /DX7's banks and the FM list; node k is TEENSY folder cards.folders() + junolib.folders() + k
inline int dx_base() { return cards.folders() + junolib.folders(); }

constexpr int kPool = 127;
constexpr int kPath = 160;
struct PoolEntry {
  int16_t folder;       // -1: empty, or not on the card
  uint16_t file;
  char name[card::kName];
  char path[kPath];     // under /Samples; "" empty
};
extern PoolEntry pool[kPool];          // RAM2 (26 KB)
// Each slot's file, opened the first time a voice plays it and kept open (a copy per voice that plays it),
// with its index record: a DSP SAMPLE voice switching samples then only reads the new head (RAM2, 19 KB).
#ifdef CORE_HEAP_BUFFERS
extern File* pool_files;   // heap_buffers_place (core/heap_buffers.h)
#else
extern File pool_files[kPool];
#endif
extern card::FileRec pool_recs[kPool];
extern bool pool_open[kPool];
extern int npool;                      // slots in use, empty ones included
extern char project[24];
void pool_forget(int line);

// The project (10 <name>), its pool file and the card (moved from voiceloop.ino). Not real time: cold.
extern bool project_dirty;   // /.rytm/project to be written (between requests: idle_save)
extern bool project_seen;
void pool_file(char* out, int max);   // "/.rytm/pools/<PROJECT>.txt", the name made safe for the card
void folder_path(int f, char* out, int max);
bool find_path(const char* path, int* folder, int* file);
void pool_save();
void pool_load();
void project_save();
void set_project(const uint8_t* d, int len);   // 10's name bytes (up to F7)
void project_load();
void pool_set(int line, int folder, int file, const card::FileRec& r);
void pool_add(int folder, int file);           // 0F LOAD TO PROJ (a sample, a JUNO patch, a DX7 voice)
void pool_assign(int slot, int folder, int file);   // 12 ASSIGN
void send_pool();                              // 14
void send_juno_names();                        // 16
void send_fm_names();                          // 17
void send_listing(int f);                      // 0D -> 0E

}  // namespace core
