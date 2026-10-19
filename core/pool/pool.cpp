// core/pool/pool.cpp - see pool.h.
#include "pool.h"

#include <string.h>
#include <strings.h>

#include "../hooks.h"
#include "../sysex.h"
#include "../voices.h"
#include "../lfo.h"

namespace core {

card::Index cards;
bool card_ok = false;
jlib::Lib junolib;
dx7::Lib dxlib;
CORE_RAM2 PoolEntry pool[kPool];
#ifdef CORE_HEAP_BUFFERS
File* pool_files = nullptr;   // heap_buffers_place (core/heap_buffers.h)
#else
CORE_RAM2 File pool_files[kPool];
#endif
CORE_RAM2 card::FileRec pool_recs[kPool];
bool pool_open[kPool];
int npool = 0;
char project[24] = "";

CORE_COLD void pool_forget(int line) {
  if (pool_open[line]) pool_files[line].close();
  pool_open[line] = false;
}

// The project's pool file, its name made safe for the card.
CORE_COLD void pool_file(char* out, int max) {
  char n[24];
  int k = 0;
  for (const char* c = project; *c && k < 23; ++c) {
    char x = *c;
    bool ok = (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') ||
              x == '-' || x == '_' || x == ' ' || x == '.';
    n[k++] = ok ? x : '_';
  }
  while (k && (n[k - 1] == ' ' || n[k - 1] == '.')) --k;
  n[k] = 0;
  sniprintf(out, max, "/.rytm/pools/%s.txt", k ? n : "_default");
}

// Folder f's path under /Samples ("" for /Samples itself).
CORE_COLD void folder_path(int f, char* out, int max) {
  if (f <= 0) {
    out[0] = 0;
    return;
  }
  folder_path(cards.folder(f).parent, out, max);
  size_t l = strlen(out);
  sniprintf(out + l, max - l, "%s%s", l ? "/" : "", cards.folder(f).name);
}

// A path under /Samples -> folder and file (name order); false if not there.
CORE_COLD bool find_path(const char* path, int* folder, int* file) {
  int f = 0;
  const char* p = path;
  for (;;) {
    const char* slash = strchr(p, '/');
    if (!slash) break;
    int len = slash - p, next = -1;
    const card::Folder& d = cards.folder(f);
    for (int i = 0; i < d.nsub; ++i) {
      const card::Folder& s = cards.folder(d.first_sub + i);
      if ((int)strlen(s.name) == len && strncmp(s.name, p, len) == 0) {
        next = d.first_sub + i;
        break;
      }
    }
    if (next < 0) return false;
    f = next;
    p = slash + 1;
  }
  card::FileRec r;
  for (int i = 0; i < cards.folder(f).nfiles; ++i)
    if (cards.file(f, i, &r) && strncmp(r.name, p, card::kName) == 0) {
      *folder = f;
      *file = i;
      return true;
    }
  return false;
}

CORE_COLD void pool_save() {
  char fn[48];
  pool_file(fn, sizeof fn);
  cards.sd().mkdir("/.rytm/pools");
  File f;
  if (!f.open(fn, kWrite)) {
    log(CORE_STR("pool: cannot write %s"), fn);
    return;
  }
  for (int i = 0; i < npool; ++i) {
    f.write(pool[i].path, strlen(pool[i].path));
    f.write('\n');
  }
  f.close();
}

CORE_COLD void pool_load() {
  char fn[48];
  pool_file(fn, sizeof fn);
  for (int i = 0; i < kPool; ++i) pool_forget(i);
  npool = 0;
  File f;
  if (f.open(fn, kRead)) {
    char line[kPath];
    int n = 0, missing = 0, used = 0;
    for (;;) {
      int c = f.read();
      if (c < 0 && n == 0) break;
      if (c < 0 || c == '\n') {
        if (npool >= kPool) break;
        while (n && line[n - 1] == '\r') --n;
        line[n] = 0;
        PoolEntry& e = pool[npool++];
        strncpy(e.path, line, kPath);
        e.folder = -1;
        e.file = 0;
        const char* base = strrchr(line, '/');
        strncpy(e.name, base ? base + 1 : line, card::kName);
        int fo, fi;
        if (line[0] && find_path(line, &fo, &fi)) {
          e.folder = fo;
          e.file = fi;
          ++used;
        } else if (line[0]) {
          ++missing;
        }
        n = 0;
        if (c < 0) break;
      } else if (n < kPath - 1) {
        line[n++] = c;
      }
    }
    f.close();
    while (npool && !pool[npool - 1].path[0]) --npool;   // trailing empty slots
    log(CORE_STR("pool %s: %d slots, %d samples, %d not on the card"), fn, npool, used, missing);
  } else {
    log(CORE_STR("pool %s: empty"), fn);
  }
  for (int i = 0; i < 8; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(-1);   // plugin voices too
  if (psram) {   // the new project's samples into free PSRAM, in slot order
    for (int i = 0; i < 8; ++i)
      if (!from_ram[i]) ram_release(i);   // a sounding note keeps its sample
    psram->clear_prefetch();
    for (int i = 0; i < npool; ++i) psram->want(pool[i].folder, pool[i].file, true);
  }
}

bool project_dirty = false;

// The project's name to /.rytm/project, so the Teensy starts with its pool.
CORE_COLD void project_save() {
  project_dirty = false;
  File f;
  if (f.open("/.rytm/project", kWrite)) {
    f.write(project, strlen(project));
    f.close();
  }
}

// 10 <name>: the Rytm's project; its pool from now on. The table goes to the
// Rytm on the first 10 since start and when the project changes, not on the
// repeats the Rytm sends every few seconds.
bool project_seen = false;

CORE_COLD void set_project(const uint8_t* d, int len) {
  char n[24];
  int k = 0;
  for (int i = 0; i < len && k < 23 && d[i] != 0xf7 && d[i]; ++i) n[k++] = d[i] & 0x7f;
  n[k] = 0;
  bool first = !project_seen;
  project_seen = true;
  if (strcmp(n, project) != 0) {
    strcpy(project, n);
    log(CORE_STR("project: %s"), project);
    pool_load();
    junolib.load_list(project);
    juno_relist();
    dxlib.load_list(project);
    fm_relist();
    if (hooks.project_changed) hooks.project_changed();   // the project's machines (the Teensy's plugins)
    lfo::project_changed();   // the project's LFOs (/.rytm/lfo/<project>.txt)
    project_dirty = true;   // /.rytm/project written from loop() (a 2 ms card write)
  } else if (!first) {
    return;
  }
  send_pool();
  send_juno_names();
  send_fm_names();
}

CORE_COLD void project_load() {
  File f;
  project[0] = 0;
  if (f.open("/.rytm/project", kRead)) {
    int n = f.read(project, sizeof project - 1);
    project[n > 0 ? n : 0] = 0;
    f.close();
  }
  log(CORE_STR("project: %s"), project[0] ? project : "(none yet)");
  pool_load();
  junolib.load_list(project);
  dxlib.load_list(project);
}

// Pool line `line` (slot line + 1) = folder's file r.
CORE_COLD void pool_set(int line, int folder, int file, const card::FileRec& r) {
  for (; npool <= line; ++npool) {   // the pool grows: empty slots up to it
    pool[npool].path[0] = 0;
    pool[npool].folder = -1;
  }
  PoolEntry& e = pool[line];
  pool_forget(line);
  folder_path(folder, e.path, kPath);
  size_t l = strlen(e.path);
  sniprintf(e.path + l, kPath - l, "%s%s", l ? "/" : "", r.name);
  e.folder = folder;
  e.file = file;
  strncpy(e.name, r.name, card::kName);
  pool_save();
  for (int i = 0; i < 8; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(line);   // plugin voices reading this slot (or refused it) too
  log(CORE_STR("pool %s: slot %d = %s"), project[0] ? project : "_default", line + 1, e.path);
}

// 0F <folder> <file>: LOAD TO PROJ - into the first empty slot (not again if
// it is in the pool already); a JUNO patch into the project's JUNO list, a
// DX7 voice into its FM list.
CORE_COLD void pool_add(int folder, int file) {
  if (card_ok && folder >= dx_base()) {
    if (dxlib.add(folder - dx_base(), file) >= 0) {
      send_fm_names();
      fm_relist();   // a voice's PAT may point at the new slot, or the list replaced the ROM's
    }
    return;
  }
  if (card_ok && folder >= cards.folders()) {
    if (junolib.add(folder - cards.folders(), file) >= 0) {
      send_juno_names();
      juno_relist();   // a voice's PAT may point at the new slot
    }
    return;
  }
  card::FileRec r;
  if (card_ok && cards.file(folder, file, &r)) {
    bool there = false;
    for (int i = 0; i < npool && !there; ++i) there = pool[i].folder == folder && pool[i].file == file;
    int line = 0;
    while (line < npool && pool[line].path[0]) ++line;   // the first empty slot
    if (!there && line < kPool) pool_set(line, folder, file, r);
  }
  send_pool();
}

// 12 <slot> <folder> <file>: ASSIGN - pool slot 1..127 = that file, whatever
// it held (the file may be in other slots too).
CORE_COLD void pool_assign(int slot, int folder, int file) {
  card::FileRec r;
  if (card_ok && folder >= cards.folders())
    log(CORE_STR("pool: assign slot %d <- JUNO/DX7 entry %d/%d: not a sample, ignored"), slot, folder, file);
  else if (slot < 1 || slot > kPool || !card_ok || !cards.file(folder, file, &r))
    log(CORE_STR("pool: assign slot %d <- folder %d file %d: ignored"), slot, folder, file);
  else
    pool_set(slot - 1, folder, file, r);
  send_pool();
}

// 14 <replace> <count> { <slot> <folder hi lo> <file hi lo> <13 name bytes> }:
// the project's pool table, non-empty slots in order, 6 a message; the first
// replaces the Rytm's table. Name: the file's without extension, printable
// ASCII (else '_'), NUL padded; folder/file 0x3fff when it is not on the card.
CORE_COLD void send_pool() {
  int line = 0, first = 1;
  do {
    PoolRow rows[6];
    int count = 0;
    for (; line < npool && count < 6; ++line) {
      const PoolEntry& e = pool[line];
      if (!e.path[0]) continue;
      int len = strnlen(e.name, card::kName);   // a long name may fill it, no NUL
      for (int i = len - 1; i > 0; --i)
        if (e.name[i] == '.') { len = i; break; }
      rows[count++] = {(uint8_t)(line + 1), (uint16_t)(e.folder < 0 ? 0x3fff : e.folder),
                       (uint16_t)(e.folder < 0 ? 0x3fff : e.file), e.name, len};
    }
    while (line < npool && !pool[line].path[0]) ++line;   // so the last message is not empty
    uint8_t m[8 + 6 * 18];
    to_rytm(m, sysex_pool(m, first, rows, count));
    first = 0;
  } while (line < npool);
}

// 16 <replace> <total> { <slot> <13 name bytes> }: the project's JUNO list's
// names, every slot 0..total-1, 8 a message; the first replaces the Rytm's
// list; total 0 (no list): one message, no entries. An empty slot: 13 NULs.
CORE_COLD void send_juno_names() {
  int total = junolib.listed(), s = 0, first = 1;
  do {
    const char* names[8];
    int k = 0, s0 = s;
    for (; k < 8 && s < total; ++k, ++s) {
      const jlib::Patch& p = junolib.slot(s);
      names[k] = p.used ? p.name : nullptr;
    }
    uint8_t m[8 + 8 * 14];
    to_rytm(m, sysex_names(m, 0x16, first, total, s0, names, k));
    first = 0;
  } while (s < total);
}

// 17 <replace> <total> { <slot> <13 name bytes> }: the effective FM list's
// names as 16 - the project's FM list, or Plaits' 96 ROM voices when it is
// empty.
CORE_COLD void send_fm_names() {
  int nl = dxlib.listed(), total = nl ? nl : dx7::kRomVoices, s = 0, first = 1;
  do {
    char rom[8][11];
    const char* names[8];
    int k = 0, s0 = s;
    for (; k < 8 && s < total; ++k, ++s) {
      names[k] = "";
      if (nl) {
        if (dxlib.slot(s).used) names[k] = dxlib.slot(s).name;
      } else {
        dx7::voice_name(dx7::rom_bank(s / 32) + 128 * (s % 32) + 118, rom[k]);
        names[k] = rom[k];
      }
    }
    uint8_t m[8 + 8 * 14];
    to_rytm(m, sysex_names(m, 0x17, first, total, s0, names, k));
    first = 0;
  } while (s < total);
}

// 0D <folder>: the folder's entries for rytm-mods 0023's TEENSY folder in the
// Rytm's file browser: 0E <folder hi lo> <parent hi lo> <first hi lo> <total hi
// lo>, per entry <kind> <id hi lo> <size 4 x 7 bits> <name length> <name>, 6 a
// message. Subfolders (kind 1, id: the index's folder number), then files
// (kind 0, id: position in name order, size in bytes). Folder 0 is /Samples.
// Entry i of TEENSY folder f for send_listing: kind (1 a folder), id, size,
// name. Folders below cards.folders() are /Samples' (the card index); the rest
// /JUNO's (juno_lib.h), shown as a folder JUNO first in /Samples' root.
namespace {

struct Ent { uint8_t kind; uint32_t id, size; char name[24]; };

uint16_t lsub[512];

int lns = 0, lparent = 0;

CORE_COLD int list_folder(int f) {
  int base = cards.folders(), dbase = dx_base();
  if (f >= dbase) {   // /DX7: a folder's folders and banks, or a bank's 32 voices
    int k = f - dbase;
    lns = dxlib.children(k, lsub, 512);
    lparent = k ? dbase + dxlib.node(k).parent : 0;
    return lns + (dxlib.node(k).bank ? 32 : 0);
  }
  if (f >= base) {
    int j = f - base;
    int total = junolib.count(j, lsub, 512, &lns);
    lparent = j ? base + junolib.folder(j).parent : 0;
    return total;
  }
  bool juno = f == 0 && junolib.folders() > 0;
  bool dx = f == 0 && dxlib.nodes() > 0;
  lns = cards.subfolders(f, lsub + juno + dx, 510) + juno + dx;
  if (juno) lsub[0] = 0xffff;   // JUNO
  if (dx) lsub[juno] = 0xfffe;  // DX7
  int nf = (int)cards.folder(f).nfiles < 512 - lns ? (int)cards.folder(f).nfiles : 512 - lns;
  lparent = f ? cards.folder(f).parent : 0;
  return lns + nf;
}

CORE_COLD bool list_entry(int f, int i, Ent* e) {
  int base = cards.folders(), dbase = dx_base();
  const char* name;
  char buf[24];
  e->size = 0;
  if (i < lns) {
    e->kind = 1;
    if (f >= dbase) {
      e->id = dbase + lsub[i];
      name = dxlib.node(lsub[i]).name;
    } else if (f >= base) {
      e->id = base + lsub[i];
      name = junolib.folder(lsub[i]).name;
    } else if (lsub[i] == 0xffff) {
      e->id = base;
      name = "JUNO";
    } else if (lsub[i] == 0xfffe) {
      e->id = dbase;
      name = "DX7";
    } else {
      e->id = lsub[i];
      name = cards.folder(lsub[i]).name;
    }
  } else if (f >= dbase) {   // a bank's voice: its 10-character name
    char raw[11];
    if (!dxlib.name(f - dbase, i - lns, raw)) return false;
    dx7::voice_name((const uint8_t*)raw, buf);
    if (!buf[0]) sniprintf(buf, sizeof buf, "VOICE %d", i - lns + 1);
    e->kind = 0;
    e->id = i - lns;
    e->size = 128;
    name = buf;
  } else if (f >= base) {
    if (!junolib.patch(f - base, i - lns, buf, nullptr)) return false;
    e->kind = 0;
    e->id = i - lns;
    e->size = 24;
    name = buf;
  } else {
    card::FileRec r;
    if (!cards.file(f, i - lns, &r)) return false;
    e->kind = 0;
    e->id = i - lns;
    e->size = r.size;
    name = r.name;
  }
  int len = 0;
  for (const char* q = name; *q && len < 23; ++q) e->name[len++] = (*q >= 32 && *q < 127) ? *q : '?';
  if (!e->kind && f < base && len > 4 && strncasecmp(e->name + len - 4, ".wav", 4) == 0) len -= 4;
  e->name[len] = 0;
  return true;
}

}  // namespace

// 0D <folder>: the folder's entries for rytm-mods 0023's TEENSY folder in the
// Rytm's file browser: 0E <folder hi lo> <parent hi lo> <first hi lo> <total hi
// lo>, per entry <kind> <id hi lo> <size 4 x 7 bits> <name length> <name>, 6 a
// message. Subfolders (kind 1, id: the folder number), then files or patches
// (kind 0, id: position in name order, size in bytes). Folder 0 is /Samples.
//
// With no card (the Pi without a Samples folder, or no card index yet) the answer is an empty folder (total 0),
// so the Rytm's UI task, which waits for this answer (0023), is never left waiting for one that does not come.
CORE_COLD void send_listing(int f) {
  if (!card_ok && f >= 0) {
    uint8_t m[32];
    to_rytm(m, sysex_listing(m, f, 0, 0, 0, nullptr, 0));
    return;
  }
  if (!card_ok || f < 0 || f >= dx_base() + dxlib.nodes()) return;
  int total = list_folder(f);
  int parent = lparent;
  for (int first = 0; first < (total > 1 ? total : 1); first += 6) {
    ListEntry ents[6];
    Ent e[6];
    int count = 0;
    for (int i = first; i < total && i < first + 6; ++i) {
      if (!list_entry(f, i, &e[count])) continue;
      ents[count] = {e[count].kind, e[count].id, e[count].size, e[count].name};
      ++count;
    }
    uint8_t m[256];
    to_rytm(m, sysex_listing(m, f, parent, first, total, ents, count));
  }
}

}  // namespace core
