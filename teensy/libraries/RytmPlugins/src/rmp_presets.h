// rmp_presets.h - presets of the user machines (the spec's 2.4). Portable: the Mac tests it.
//   Generic presets: /PRESETS/<id>/<name>.txt, one line per knob "P<n> <value>" (n 1..16, value
//   0..127); other lines and knobs not named are ignored.
//   Native presets: /PRESETS/<id>/<name>.<preset_ext> (the manifest's), handed to the plugin's
//   load_entry. Both in one list, in name order, at most 127; PRE picks entry min(PRE, n - 1).
//   rmp_pv: what a report means for one voice's preset (a PRE turn or the voice's first non-trig
//   report: the sound's preset, written back to the Rytm as 1C and held over the reports until they
//   carry it, 300 ms at most; a p-locked PRE on a trig: that preset played, nothing written back, until
//   a later trig or a knob that moves).
//
// Memory (no PSRAM needed): a preset list is split in two.
//   rmp_preset_names  ONE for all slots (DMAMEM on the Teensy, ~1.8 KB): the shown names of the slot
//                     scanned last, keyed by (slot, that slot's index generation). Only 1B 0x20 needs it.
//   rmp_preset_index  ONE PER SLOT (~390 B): each entry's folder index (SdFat's dirIndex) and kind, kept
//                     after a scan, so a PRE load or a p-lock play opens the file without a rescan.
//
// The glue (voiceloop, Tasks 10/11), all from loop() jobs, never from the audio path:
//   boot:  rmp_preset_names_init(&names); rmp_preset_index_init(&ix[s]) for each slot.
//   slot (re)assigned, package installed/removed, or the folder written: rmp_preset_invalidate(&ix[s]).
//   names asked (19 what 2) and !rmp_preset_names_ok(&names, &ix[s], s),
//   or a load for slot s and !rmp_preset_index_ok(&ix[s]):  scan:
//       rmp_preset_scan_begin(&names, &ix[s], s);
//       for each entry of /PRESETS/<id>/ (the caller skips directories and FAT-hidden/system entries):
//           rmp_preset_add(&names, &ix[s], <its name>, <its dirIndex>, desc.preset_ext);
//       rmp_preset_scan_end(&names, &ix[s]);   (moves ix[s].gen on when the list came out different)
//     on a card error midway: rmp_preset_invalidate(&ix[s]); rmp_preset_names_init(&names).
//   1B 0x20: names.name[0 .. names.n - 1] (once rmp_preset_names_ok).
//   load entry e of slot s: open dirIndex ix[s].dir[e]; ix[s].kind[e] says generic (read the file into
//     the 4 KB buffer, rmp_preset_parse with truncated = the buffer was filled) or native (load_entry).
//   per slot, whenever its description is (re)read: rmp_knobs_from_desc(&knobs[s], &desc[s]) - the
//     roles and ranges rmp_pv_load / rmp_pv_effective take (rmp_pv_loaded is only the 0..127 shorthand).
//   per voice: rmp_pv_reset(&pv) when it becomes a machine of a slot (first = true on its next report),
//     and when it leaves one.
//   rmp_pv_decide needs rmp_preset_index_ok(&ix[s]) first (its total is ix[s].n): with a stale index,
//     scan first (a job) and decide on a later report.
//   per report and voice: rmp_pv_check(&pv, ix[s].gen); e = rmp_pv_decide(...); e >= 0: a load job for
//     e (rmp_pv_load on success, sending its 1C pairs; rmp_pv_failed on failure); then rmp_pv_apply and
//     rmp_pv_effective on the report's values before params().
#pragma once
#include <stdint.h>
#define RMP_PRESETS_MAX 127
#define RMP_PRESET_NAME 14     // 13 characters and a NUL
#define RMP_PV_HOLD_MS 300
enum { RMP_PRESET_GENERIC = 0, RMP_PRESET_NATIVE = 1 };
struct rmp_desc;

// values[k]: P(k+1)'s value 0..127 from the file, -1 when the file does not name it. Lines end in LF,
// CRLF or a lone CR; a leading UTF-8 BOM is skipped. A line is: blanks, "P"/"p", 1..2 digits, blanks,
// 1..3 digits, blanks - nothing else (no signs, decimals or '='); a later line wins. truncated: txt is
// only the first part of a longer file (the read buffer was filled), so its last line, possibly cut, is
// dropped unless it ended in a line break. Without the flag the caller must drop a cut last line itself.
void rmp_preset_parse(const char *txt, uint32_t len, int16_t values[16], bool truncated = false);

struct rmp_preset_names {
  char name[RMP_PRESETS_MAX][RMP_PRESET_NAME];   // shown names: the file's without its extension, 13 at most
  int8_t slot;                                   // the slot they are of, -1 none
  uint8_t gen;                                   // that slot's index generation when scanned
  uint8_t n;
};
struct rmp_preset_index {
  uint16_t dir[RMP_PRESETS_MAX];                 // the file's index in its folder (SdFat's dirIndex)
  uint8_t kind[RMP_PRESETS_MAX];                 // RMP_PRESET_GENERIC / NATIVE
  uint8_t n;                                     // entries (the list's total)
  uint8_t gen;                                   // 1..255: moves on when the list changes (rmp_pv follows it)
  uint8_t ok;                                    // 1: scanned since the last invalidate
  uint32_t print;                                // fingerprint of the last scan's list
};
void rmp_preset_names_init(rmp_preset_names *nm);
void rmp_preset_index_init(rmp_preset_index *ix);
// The slot's folder or package may have changed: the next use rescans, and the generation moves on
// (the voices playing the slot forget their entries).
void rmp_preset_invalidate(rmp_preset_index *ix);
void rmp_preset_scan_begin(rmp_preset_names *nm, rmp_preset_index *ix, int slot);
// One file of the preset folder: "<name>.txt" (any case) is generic, "<name>.<ext>" native (ext the
// manifest's preset_ext, any case; "" or null: none). Not added (false): anything else; a name that is
// empty, all blanks, or starts with '.' (._Bass.txt, .DS_Store.txt); a name already listed of the same
// kind (the first stays: names cut to 13 characters can meet); a generic one where a native one of that
// name is. A native one replaces a generic one of its name (native wins, whatever the folder order).
// Names: bytes outside printable ASCII become '_'; ordered by bytes, so case-sensitive ("Abc" < "Bass" <
// "abc"). The list keeps the first 127 names in that order.
bool rmp_preset_add(rmp_preset_names *nm, rmp_preset_index *ix, const char *file, uint16_t dir, const char *ext);
void rmp_preset_scan_end(rmp_preset_names *nm, rmp_preset_index *ix);
// names holds slot's current list (no scan needed for 1B 0x20).
bool rmp_preset_names_ok(const rmp_preset_names *nm, const rmp_preset_index *ix, int slot);
// ix->dir / kind / n are current (no scan needed for a load).
bool rmp_preset_index_ok(const rmp_preset_index *ix);

// A slot's knobs as the presets use them, by P index (P1 = 0): role (RMP_ROLE_*) and the manifest's min..max.
struct rmp_knobs {
  uint8_t role[16], min[16], max[16];
};
void rmp_knobs_from_desc(rmp_knobs *r, const rmp_desc *d);           // knobs not in d: free, 0..127
void rmp_knobs_from_roles(rmp_knobs *r, const uint8_t roles[16]);    // ranges 0..127

struct rmp_pv {
  int16_t synced;      // the entry last loaded for the sound itself (written back), -1 none
  int16_t playing;     // the entry the voice plays, -1 none
  int16_t failed;      // an entry whose load failed: not asked again until another loads or the list changes
  bool plock;          // playing is a p-locked entry (no write-back)
  bool snd_ok;         // snd holds a non-trig report
  uint8_t gen;         // the slot index generation the entries are of (0: none yet)
  uint16_t pmask;      // a p-lock's preset-role knobs (all of them; mask is what still overrides)
  uint16_t mask;       // knobs the playing p-locked entry overrides
  uint8_t val[16];     // the playing entry's values for its preset-role knobs (clamped to min..max)
  uint16_t hold;       // after a write-back: knobs replaced in the reports until a report carries them all
  uint8_t hval[16];    // the written-back values (a p-lock played during the hold does not change them)
  uint32_t hold_t;     // when that began (ms)
  uint8_t base[16];    // the trig's values when a p-lock played: a later report that moves a knob ends its override
  uint8_t snd[16];     // the last non-trig report's values (the sound's): a trig value that differs is p-locked
  uint16_t omask;      // offset-role knobs the playing entry has values for
  uint8_t oval[16];    // those values
};
// Where these run: rmp_pv_* are called from report() in loop() context (not an ISR). Like the parsing
// and the scan they live in flash (RMP_PRE_COLD): that stalls only while flash is written, which happens
// with the transport stopped.
void rmp_pv_reset(rmp_pv *v);        // the voice starts the slot afresh
void rmp_pv_invalidate(rmp_pv *v);   // the list changed: forget the entries (the next report reloads)
// Before decide: the slot's index generation (ix.gen); a different one invalidates.
void rmp_pv_check(rmp_pv *v, uint8_t gen);
// A report for the voice (is_trig: a 0x04; first: the voice just became this machine). pre: the preset
// knob's P index (0..15), -1 none; total: entries in the slot's list. Returns the entry to load and
// *sync (write it back), or -1 when nothing is to be loaded. A trig never syncs (its PRE may be
// p-locked): a first report that is a trig plays its entry without write-back, and the next non-trig
// report syncs.
int rmp_pv_decide(rmp_pv *v, int pre, int total, bool is_trig, bool first, const float *p, bool *sync);
// The entry could not be loaded (read error, a native load_entry refusing): not asked again until another
// entry loads or the list changes.
void rmp_pv_failed(rmp_pv *v, int entry);
// The entry was loaded: values[k] 0..127 or -1 (a generic file's, or a native load_entry's). Preset-role
// knobs take the values, clamped to the knob's min..max; offset-role knobs keep theirs for
// rmp_pv_effective; free knobs are never touched. With sync: out_k/out_v get the 1C pairs (16 at most)
// and their count is returned. p: the report the load was decided on.
int rmp_pv_load(rmp_pv *v, int entry, bool sync, const int16_t values[16], const rmp_knobs *r, uint32_t now_ms,
                const float *p, uint8_t *out_k, uint8_t *out_v);
// rmp_pv_load with roles only (ranges 0..127).
int rmp_pv_loaded(rmp_pv *v, int entry, bool sync, const int16_t values[16], const uint8_t roles[16], uint32_t now_ms,
                  const float *p, uint8_t *out_k, uint8_t *out_v);
// Before params(): the hold and the p-lock override applied to the report's values in place. On a trig a
// knob whose value differs from the sound's (it is p-locked on that step) keeps its own p-lock value.
// The reports carry no lock flags: a p-lock equal to the sound's own value cannot be told from "not
// locked", so on that trig the preset's value wins.
void rmp_pv_apply(rmp_pv *v, float *p, bool is_trig, uint32_t now_ms);
// After apply, what the plugin gets: an offset knob with a value in the playing entry plays
// clamp(entry value + (knob - 64), min..max) (64, as stored, plays the entry's value); preset-role knobs
// keep apply's value (the entry's, written back); free knobs are untouched.
void rmp_pv_effective(const rmp_pv *v, const rmp_knobs *r, float *p);
