// sim/rmp/rmp_native.cpp - load a plugin's native build (.dylib) and drive it through the same
// rmp_plugin table the Teensy uses. One harness object per voice; a plugin's init() runs once per
// loaded plugin, shared by all its voices. Plugin API 2 as on the Teensy, from the Teensy's own
// portable modules: an API-1 plugin gets an API-1 host table (api 1, no v2 functions); voice_enter
// after voice_init, voice_leave before close (the call log records the order); pool_info/pool_read on
// the voice's rmp_pool reader over samples registered from Python (rmp_native_pool_set; "psram" ones
// are read whole, as PSRAM-resident ones are), rmp_native_pool_job standing in for the Teensy's loop
// jobs; presets as voiceloop's glue does them (rmp_presets.h's recipe): a loaded plugin is one slot,
// with its package's description (rmp_loader + rmp_manifest: the knobs' roles and ranges, the PRE knob,
// preset_ext) and a preset folder registered from Python (scanned into the shared rmp_preset_names and
// the slot's rmp_preset_index), and each voice its rmp_pv; rmp_report runs a report through
// check/decide/load/apply/effective into the plugin's params.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <string>
#include <vector>
#include "rmp/rmp_api.h"
#include "rmp_loader.h"
#include "rmp_manifest.h"
#include "rmp_pool.h"
#include "rmp_presets.h"

struct preset_file { std::string name; std::vector<uint8_t> bytes; };
struct rmp_lib {
  rmp_lib *next; void *dl; const rmp_plugin *p; rmp_host host; int refs;
  int slot;                              // the slot it stands for (1..127): rmp_preset_names' key
  bool has_desc; rmp_desc desc; rmp_knobs knobs;
  std::vector<preset_file> folder;       // /PRESETS/<id>/ (dirIndex = the position here)
  rmp_preset_index ix;
};
struct rmp_h {
  rmp_h *next; rmp_lib *lib; void *state;
  rmp_pool_reader pool; uint8_t head[8192], ring[16384];   // the Teensy's StreamVoice sizes
  rmp_pv pv; int enters, leaves, loads;
};
struct sample { int slot; std::vector<uint8_t> bytes; rmp_pool_src src; bool psram; };
static rmp_lib *g_libs;
static rmp_h *g_voices;
static rmp_h *g_now;                    // the voice the host is calling (pool_info / pool_read)
// Heap-allocated: a voice's reader holds its sample as ctx, so a sample never moves while it lives.
static std::vector<std::unique_ptr<sample>> g_pool;
static rmp_preset_names g_names;        // ONE for all slots, as on the Teensy
static int g_next_slot;
static std::string g_log;               // the harness's calls into plugins: I init, E enter, L leave, C closed

static sample *find_sample(int slot) {
  for (auto &s : g_pool) if (s->slot == slot) return s.get();
  return nullptr;
}
static bool mem_rd(void *ctx, uint32_t off, uint8_t *buf, uint32_t n) {
  sample *s = (sample *)ctx;
  if ((uint64_t)off + n > s->bytes.size()) return false;
  memcpy(buf, s->bytes.data() + off, n);
  return true;
}
static int32_t h_pool_info(int32_t slot, rmp_pool_info *info) {
  sample *s = find_sample(slot);
  if (!g_now || !s) return RMP_POOL_EMPTY;
  if (s->psram) {
    uint32_t bpf = s->src.channels * (s->src.bits / 8u);
    info->frames = s->src.data_len / bpf;
    info->channels = s->src.channels > 2 ? 2 : s->src.channels;
    info->rate = s->src.rate;
    info->where = RMP_POOL_PSRAM;
    info->head_frames = info->frames;
    return RMP_POOL_OK;
  }
  return rmp_pool_info_get(&g_now->pool, slot, info);
}
static int32_t h_pool_read(int32_t slot, uint32_t pos, float *buf, int32_t n) {
  sample *s = find_sample(slot);
  if (!g_now || !s) return 0;
  if (s->psram) {
    uint32_t bpf = s->src.channels * (s->src.bits / 8u);
    return rmp_pool_decode(s->bytes.data() + s->src.data_offset, s->src.data_len / bpf, s->src.channels, s->src.bits,
                           pos, buf, n);
  }
  return rmp_pool_read(&g_now->pool, slot, pos, buf, n);
}
static void h_log(const char *m) { fprintf(stderr, "plugin: %s\n", m); }
static uint32_t h_random(uint32_t *s) { uint32_t x = *s ? *s : 2463534242u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

static rmp_lib *lib_find(void *dl) {
  for (rmp_lib *l = g_libs; l; l = l->next) if (l->dl == dl) return l;
  return nullptr;
}
static void lib_release(rmp_lib *lib) {
  if (--lib->refs > 0) return;
  for (rmp_lib **pp = &g_libs; *pp; pp = &(*pp)->next)
    if (*pp == lib) { *pp = lib->next; break; }
  if (g_names.slot == lib->slot) rmp_preset_names_init(&g_names);
  dlclose(lib->dl);
  delete lib;
}

// The glue's scan of a slot's preset folder (rmp_presets.h): when the index is not current.
static void scan_if_needed(rmp_lib *lib) {
  if (rmp_preset_index_ok(&lib->ix) && rmp_preset_names_ok(&g_names, &lib->ix, lib->slot)) return;
  rmp_preset_scan_begin(&g_names, &lib->ix, lib->slot);
  for (size_t i = 0; i < lib->folder.size(); ++i)
    rmp_preset_add(&g_names, &lib->ix, lib->folder[i].name.c_str(), (uint16_t)i, lib->has_desc ? lib->desc.preset_ext : "");
  rmp_preset_scan_end(&g_names, &lib->ix);
}
static bool pkg_rd(void *ctx, uint32_t off, uint8_t *buf, uint32_t n) {
  std::vector<uint8_t> *b = (std::vector<uint8_t> *)ctx;
  if ((uint64_t)off + n > b->size()) return false;
  memcpy(buf, b->data() + off, n);
  return true;
}

extern "C" {
void *rmp_open(const char *path) {
  void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!dl) { fprintf(stderr, "%s\n", dlerror()); return nullptr; }
  rmp_lib *lib = lib_find(dl);
  if (lib) {
    dlclose(dl);   // dlopen counted this call; our table holds one reference per loaded plugin
  } else {
    const rmp_plugin *p = (const rmp_plugin *)dlsym(dl, "rmp_entry");
    if (!p || p->magic != RMP_PLUGIN_MAGIC || p->api < RMP_API_MIN || p->api > RMP_API) {
      fprintf(stderr, "%s: not an RMP plugin (rmp_entry missing or wrong magic/api)\n", path);
      dlclose(dl); return nullptr;
    }
    if (!p->init || !p->voice_init || !p->note_on || !p->note_off || !p->params || !p->render) {
      fprintf(stderr, "%s: plugin table has a NULL function pointer\n", path);
      dlclose(dl); return nullptr;
    }
    lib = new rmp_lib();
    lib->dl = dl; lib->p = p;
    lib->host.api = p->api;   // an API-1 plugin gets an API-1 table
    lib->host.sample_rate = 48000.0f; lib->host.max_block = 12;
    lib->host.log = h_log; lib->host.random = h_random;
    if (p->api >= 2) {
      lib->host.reserved[RMP_H_POOL_INFO] = RMP_FN(h_pool_info);
      lib->host.reserved[RMP_H_POOL_READ] = RMP_FN(h_pool_read);
    }
    if (p->init(&lib->host)) {
      fprintf(stderr, "%s: init refused\n", path);
      dlclose(dl); delete lib; return nullptr;
    }
    if (!g_next_slot) rmp_preset_names_init(&g_names);
    lib->slot = g_next_slot % 127 + 1; ++g_next_slot;
    rmp_knobs_from_desc(&lib->knobs, nullptr);
    rmp_preset_index_init(&lib->ix);
    lib->next = g_libs; g_libs = lib;
  }
  rmp_h *h = (rmp_h *)calloc(1, sizeof *h);
  void *st = nullptr;
  size_t sz = ((size_t)lib->p->state_bytes + 15) & ~(size_t)15;
  if (sz == 0) sz = 16;
  if (!h || posix_memalign(&st, 16, sz)) { free(h); lib->refs++; lib_release(lib); return nullptr; }
  memset(st, 0, sz);
  lib->refs++;
  h->lib = lib; h->state = st;
  rmp_pool_attach(&h->pool, h->head, sizeof h->head, h->ring, sizeof h->ring, 8192);
  rmp_pv_reset(&h->pv);
  h->next = g_voices; g_voices = h;
  g_now = h;
  lib->p->voice_init(st); g_log += 'I';
  if (rmp_voice_fn enter = rmp_voice_enter_of(lib->p)) { enter(st); h->enters++; g_log += 'E'; }
  g_now = nullptr;
  return h;
}
void rmp_note_on(void *v, float note, float vel) { rmp_h *h = (rmp_h *)v; g_now = h; h->lib->p->note_on(h->state, note, vel); g_now = nullptr; }
void rmp_note_off(void *v) { rmp_h *h = (rmp_h *)v; g_now = h; h->lib->p->note_off(h->state); g_now = nullptr; }
void rmp_params(void *v, const float *p) { rmp_h *h = (rmp_h *)v; g_now = h; h->lib->p->params(h->state, p); g_now = nullptr; }
void rmp_render(void *v, float *out, int n) {
  rmp_h *h = (rmp_h *)v;
  g_now = h;
  for (int i = 0; i < n; i += 12) h->lib->p->render(h->state, out + i, n - i < 12 ? n - i : 12);
  g_now = nullptr;
}
void rmp_close(void *v) {
  rmp_h *h = (rmp_h *)v;
  if (rmp_voice_fn leave = rmp_voice_leave_of(h->lib->p)) { g_now = h; leave(h->state); h->leaves++; g_log += 'L'; g_now = nullptr; }
  for (rmp_h **pp = &g_voices; *pp; pp = &(*pp)->next)
    if (*pp == h) { *pp = h->next; break; }
  free(h->state);
  g_log += 'C';
  lib_release(h->lib);
  free(h);
}
int rmp_api_of(void *v) { return (int)((rmp_h *)v)->lib->p->api; }
// which: 0 voice_enter calls, 1 voice_leave calls, 2 preset loads attempted by rmp_report.
int rmp_calls(void *v, int which) { rmp_h *h = (rmp_h *)v; return which == 2 ? h->loads : which ? h->leaves : h->enters; }
// The call log since the last call (into buf, NUL-terminated), then cleared: its length.
int rmp_call_log(char *buf, int cap) {
  int n = (int)g_log.size() < cap - 1 ? (int)g_log.size() : cap - 1;
  memcpy(buf, g_log.data(), (size_t)n); buf[n] = 0;
  g_log.clear();
  return n;
}
// The plugin's load_entry on this voice; values[16] -1 unless it fills them. -1000: it has none.
int rmp_load_entry(void *v, const uint8_t *bytes, int len, int16_t *values) {
  rmp_h *h = (rmp_h *)v;
  for (int k = 0; k < 16; ++k) values[k] = -1;
  rmp_load_entry_fn fn = rmp_load_entry_of(h->lib->p);
  if (!fn) return -1000;
  g_now = h;
  int r = fn(h->state, bytes, len, values);
  g_now = nullptr;
  return r;
}
// A pool slot's PCM (data_offset 0): bytes, channels 1+, bits 16/24/32 (integer), rate; psram: read whole,
// no jobs. Refused (-1) for another format. Re-registering a slot clears the reader of every voice that
// prepared, wanted or failed it (the Teensy's new-project path), before the old sample is freed.
int rmp_native_pool_set(int slot, const uint8_t *bytes, uint32_t len, int channels, int bits, float rate, int psram) {
  if (channels < 1 || channels > 255 || (bits != 16 && bits != 24 && bits != 32)) return -1;
  for (rmp_h *h = g_voices; h; h = h->next)
    if (h->pool.slot == slot || h->pool.want == slot || h->pool.bad == slot) rmp_pool_clear(&h->pool);
  for (size_t i = 0; i < g_pool.size(); ++i)
    if (g_pool[i]->slot == slot) { g_pool.erase(g_pool.begin() + (long)i); break; }
  std::unique_ptr<sample> s(new sample());
  s->slot = slot; s->bytes.assign(bytes, bytes + len);
  s->src = rmp_pool_src{0, len, rate, (uint8_t)channels, (uint8_t)bits};
  s->psram = psram != 0;
  g_pool.push_back(std::move(s));
  return 0;
}
void rmp_native_pool_clear(void) {
  for (rmp_h *h = g_voices; h; h = h->next) rmp_pool_clear(&h->pool);   // before the samples go
  g_pool.clear();
}
// The Teensy's loop jobs, once: each voice's wanted sample prepared (its head read), then one refill for
// each voice with less than 4096 frames ahead.
void rmp_native_pool_job(void) {
  for (rmp_h *h = g_voices; h; h = h->next) {
    if (h->pool.want) {
      int w = h->pool.want;
      sample *s = find_sample(w);
      if (s && !s->psram) rmp_pool_prepare(&h->pool, w, &s->src, mem_rd, s);
      else { h->pool.bad = w; h->pool.want = 0; }
    }
    if (rmp_pool_ahead(&h->pool) < 4096) rmp_pool_refill(&h->pool);
  }
}
void rmp_native_preset_parse(const char *txt, int len, int16_t *values) { rmp_preset_parse(txt, (uint32_t)len, values); }
int rmp_pv_decide_v(void *v, int pre, int total, int is_trig, int first, const float *p, int *sync) {
  bool s = false;
  int e = rmp_pv_decide(&((rmp_h *)v)->pv, pre, total, is_trig != 0, first != 0, p, &s);
  *sync = s;
  return e;
}
int rmp_pv_loaded_v(void *v, int entry, int sync, const int16_t *values, const uint8_t *roles, uint32_t now_ms,
                    const float *p, uint8_t *out_k, uint8_t *out_v) {
  return rmp_pv_loaded(&((rmp_h *)v)->pv, entry, sync != 0, values, roles, now_ms, p, out_k, out_v);
}
void rmp_pv_apply_v(void *v, float *p, int is_trig, uint32_t now_ms) { rmp_pv_apply(&((rmp_h *)v)->pv, p, is_trig != 0, now_ms); }

// The plugin's package (.rmp): its manifest gives the slot's description (knob roles and ranges, the PRE
// knob, preset_ext), as the Teensy reads it. 0 ok; -1 unreadable, -2 not a valid package or manifest.
int rmp_set_package(void *v, const char *rmp_path) {
  rmp_lib *lib = ((rmp_h *)v)->lib;
  FILE *f = fopen(rmp_path, "rb");
  if (!f) return -1;
  std::vector<uint8_t> b;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) b.insert(b.end(), chunk, chunk + n);
  fclose(f);
  rmp_info info;
  if (rmp_verify(b.data(), (uint32_t)b.size(), &info) != RMP_OK) return -2;
  rmp_desc d;
  if (!rmp_manifest_read(&info, pkg_rd, &b, &d)) return -2;
  lib->desc = d; lib->has_desc = true;
  rmp_knobs_from_desc(&lib->knobs, &lib->desc);
  rmp_preset_invalidate(&lib->ix);
  return 0;
}
// The slot's preset folder: a file added (name with its extension), or all removed (name null). The
// folder changed, so the index is invalidated (rescanned on next use, its generation moves on).
void rmp_preset_file(void *v, const char *name, const uint8_t *bytes, int len) {
  rmp_lib *lib = ((rmp_h *)v)->lib;
  if (!name) lib->folder.clear();
  else lib->folder.push_back(preset_file{name, std::vector<uint8_t>(bytes, bytes + len)});
  rmp_preset_invalidate(&lib->ix);
}
// The slot's list (1B 0x20): its count; name i into buf (14 bytes) when 0 <= i < count.
int rmp_preset_name(void *v, int i, char *buf) {
  rmp_lib *lib = ((rmp_h *)v)->lib;
  scan_if_needed(lib);
  if (i >= 0 && i < g_names.n) memcpy(buf, g_names.name[i], RMP_PRESET_NAME);
  return g_names.n;
}
// A report for the voice, as voiceloop's glue: rmp_pv_check, decide, the load (a generic file parsed from
// a 4 KB buffer, a native one through load_entry; rmp_pv_load with the description's knobs, or
// rmp_pv_failed), then apply and effective on p (in place) and the plugin's params. *entry: the entry
// loaded (-1 none, -2 failed). Returns the 1C pairs' count (out_k/out_v, 16 at most).
int rmp_report(void *v, int is_trig, int first, float *p, uint32_t now_ms, uint8_t *out_k, uint8_t *out_v, int *entry) {
  rmp_h *h = (rmp_h *)v;
  rmp_lib *lib = h->lib;
  int npairs = 0;
  *entry = -1;
  int pre = lib->has_desc && lib->desc.pre >= 0 ? lib->desc.knob[lib->desc.pre].p - 1 : -1;   // desc.pre is a knob[] index
  if (pre >= 0) {
    scan_if_needed(lib);
    rmp_pv_check(&h->pv, lib->ix.gen);
    bool sync = false;
    int e = rmp_pv_decide(&h->pv, pre, lib->ix.n, is_trig != 0, first != 0, p, &sync);
    if (e >= 0) {
      h->loads++;
      const preset_file &pf = lib->folder[lib->ix.dir[e]];
      int16_t values[16];
      int r = 0;
      if (lib->ix.kind[e] == RMP_PRESET_GENERIC) {
        uint32_t len = pf.bytes.size() < 4096 ? (uint32_t)pf.bytes.size() : 4096u;
        rmp_preset_parse((const char *)pf.bytes.data(), len, values, len == 4096u);
      } else {
        r = rmp_load_entry(h, pf.bytes.data(), (int)pf.bytes.size(), values);
      }
      if (r == 0) {
        npairs = rmp_pv_load(&h->pv, e, sync, values, &lib->knobs, now_ms, p, out_k, out_v);
        *entry = e;
      } else {
        rmp_pv_failed(&h->pv, e);
        *entry = -2;
      }
    }
  }
  rmp_pv_apply(&h->pv, p, is_trig != 0, now_ms);
  rmp_pv_effective(&h->pv, &lib->knobs, p);
  rmp_params(h, p);
  return npairs;
}
}
