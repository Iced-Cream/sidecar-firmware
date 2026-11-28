// core/code/loader.cpp - see loader.h.
#include "loader.h"

#include <math.h>
#include <string.h>

namespace core {
namespace code {
namespace {

// ---- the allocation window (make() only, control core) ----
uint8_t* s_win;
size_t s_win_n, s_win_used;

void* win_alloc(size_t n) {
  if (!s_win) return nullptr;
  size_t at = (s_win_used + 15) & ~(size_t)15;
  if (n > s_win_n || at > s_win_n - n) return nullptr;
  s_win_used = at + n;
  memset(s_win + at, 0, n);
  return s_win + at;
}

// ---- what a blob may call (ABI 1, scx.h) ----
extern "C" {
void* x_alloc(size_t n) { return win_alloc(n); }
void x_free(void*) {}
void x_free2(void*, size_t) {}
void x_pure_virtual() { __builtin_trap(); }
int x_atexit(void (*)(void*), void*, void*) { return 0; }
}

constexpr uint32_t fnv(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}
struct Export { const char* name; uint32_t hash; const void* fn; };
#define X(n, f) {n, fnv(n), (const void*)(f)}
const Export kExports[] = {
    X("scx_alloc", x_alloc), X("scx_free", x_free),
    X("_Znwm", x_alloc), X("_Znam", x_alloc), X("_ZdlPv", x_free), X("_ZdaPv", x_free),
    X("_ZdlPvm", x_free2), X("_ZdaPvm", x_free2),
    X("memcpy", static_cast<void* (*)(void*, const void*, size_t)>(memcpy)),
    X("memmove", static_cast<void* (*)(void*, const void*, size_t)>(memmove)),
    X("memset", static_cast<void* (*)(void*, int, size_t)>(memset)),
    X("memcmp", static_cast<int (*)(const void*, const void*, size_t)>(memcmp)),
    X("strlen", static_cast<size_t (*)(const char*)>(strlen)),
    X("sinf", static_cast<float (*)(float)>(sinf)), X("cosf", static_cast<float (*)(float)>(cosf)),
    X("tanf", static_cast<float (*)(float)>(tanf)), X("atanf", static_cast<float (*)(float)>(atanf)),
    X("atan2f", static_cast<float (*)(float, float)>(atan2f)), X("expf", static_cast<float (*)(float)>(expf)),
    X("exp2f", static_cast<float (*)(float)>(exp2f)), X("logf", static_cast<float (*)(float)>(logf)),
    X("log2f", static_cast<float (*)(float)>(log2f)), X("log10f", static_cast<float (*)(float)>(log10f)),
    X("powf", static_cast<float (*)(float, float)>(powf)), X("sqrtf", static_cast<float (*)(float)>(sqrtf)),
    X("fabsf", static_cast<float (*)(float)>(fabsf)), X("floorf", static_cast<float (*)(float)>(floorf)),
    X("ceilf", static_cast<float (*)(float)>(ceilf)), X("fmodf", static_cast<float (*)(float, float)>(fmodf)),
    X("tanhf", static_cast<float (*)(float)>(tanhf)), X("roundf", static_cast<float (*)(float)>(roundf)),
    X("__cxa_pure_virtual", x_pure_virtual), X("__cxa_atexit", x_atexit),
};
#undef X
constexpr int kExportCount = (int)(sizeof kExports / sizeof kExports[0]);

uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
void wr64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }

bool name_ok(const char* n) {
  int k = 0;
  while (k < SCX_NAME_MAX && n[k]) {
    char c = n[k];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    ++k;
  }
  if (k == 0) return false;
  for (int i = k; i < SCX_NAME_MAX; ++i) if (n[i]) return false;   // NUL padded
  return true;
}
// [off, off + len) inside [0, size)
bool inside(uint64_t off, uint64_t len, uint64_t size) { return off <= size && len <= size - off; }
}  // namespace

void alloc_window(void* p, size_t n) {
  s_win = (uint8_t*)p;
  s_win_n = p ? n : 0;
  s_win_used = 0;
}
size_t alloc_used() { return s_win_used; }

const void* find_export(uint32_t hash) {
  for (const Export& e : kExports) if (e.hash == hash) return e.fn;
  return nullptr;
}
int export_count() { return kExportCount; }
const char* export_name(int i) { return i >= 0 && i < kExportCount ? kExports[i].name : nullptr; }

int check(const uint8_t* f, uint32_t n, scx_header* out) {
  if (!f || n < kHeader || n > kMaxBlob) return n > kMaxBlob ? kFull : kBad;
  scx_header h;
  memcpy(&h, f, sizeof h);
  if (h.magic != SCX_MAGIC) return kBad;
  if (h.abi != SCX_ABI) return kAbi;
  if ((h.flags & ~SCX_F_DATA) || !name_ok(h.name)) return kBad;
  const uint64_t img = h.image_size, mem = (uint64_t)h.image_size + h.bss_size;
  if (img == 0 || !inside(kHeader, img, n) || mem > kMaxBlob * 4ull) return kBad;
  // the writable part: from a 64 KB boundary inside the image, above the entry; none without the flag
  if (h.flags & SCX_F_DATA) {
    if (h.data_off == 0 || h.data_off % SCX_DATA_ALIGN || h.data_off > img || h.entry >= h.data_off) return kBad;
  } else if (h.data_off) {
    return kBad;
  }
  const uint64_t rel_n = (uint64_t)h.reloc_count * 4, imp_n = (uint64_t)h.import_count * sizeof(scx_import);
  // the tables follow the image, relocations first, and end the file
  if (h.reloc_off != kHeader + img || !inside(h.reloc_off, rel_n, n)) return kBad;
  if (h.import_off != h.reloc_off + rel_n || !inside(h.import_off, imp_n, n)) return kBad;
  if ((uint64_t)h.import_off + imp_n != n) return kBad;
  if (!inside(h.entry, 4, img) || (h.entry & 3)) return kBad;
  if (!inside(h.init_off, (uint64_t)h.init_count * 8, img)) return kBad;
  const uint8_t* image = f + kHeader;
  for (uint32_t i = 0; i < h.reloc_count; ++i) {
    uint32_t at = rd32(f + h.reloc_off + 4 * i);
    if (!inside(at, 8, img) || rd64(image + at) >= mem) return kBad;
  }
  for (uint32_t i = 0; i < h.import_count; ++i) {
    scx_import im;
    memcpy(&im, f + h.import_off + sizeof im * i, sizeof im);
    if (!inside(im.offset, 8, img)) return kBad;
    if (!find_export(im.hash)) return kImport;
  }
  // every constructor slot must be relocated (it is a pointer into the image)
  for (uint32_t k = 0; k < h.init_count; ++k) {
    uint32_t slot = h.init_off + 8 * k;
    bool rel = false;
    for (uint32_t i = 0; i < h.reloc_count && !rel; ++i) rel = rd32(f + h.reloc_off + 4 * i) == slot;
    if (!rel || rd64(image + slot) >= img) return kBad;
  }
  if (out) *out = h;
  return kOk;
}

int place(const uint8_t* f, uint32_t n, uint8_t* mem, uint32_t cap) {
  scx_header h;
  int c = check(f, n, &h);
  if (c != kOk) return c;
  if (mem_bytes(h) > cap) return kFull;
  memcpy(mem, f + kHeader, h.image_size);
  memset(mem + h.image_size, 0, h.bss_size);
  const uint64_t base = (uint64_t)(uintptr_t)mem;
  for (uint32_t i = 0; i < h.reloc_count; ++i) {
    uint32_t at = rd32(f + h.reloc_off + 4 * i);
    wr64(mem + at, base + rd64(mem + at));
  }
  for (uint32_t i = 0; i < h.import_count; ++i) {
    scx_import im;
    memcpy(&im, f + h.import_off + sizeof im * i, sizeof im);
    wr64(mem + im.offset, (uint64_t)(uintptr_t)find_export(im.hash) + rd64(mem + im.offset));
  }
  return kOk;
}

void run_init(uint8_t* mem, const scx_header& h) {
  for (uint32_t k = 0; k < h.init_count; ++k) {
    void (*fn)() = (void (*)())(uintptr_t)rd64(mem + h.init_off + 8 * k);
    fn();
  }
}

const scx_block* describe(uint8_t* mem, const scx_header& h, uint32_t max_state, int* code) {
  int dummy;
  int& c = code ? *code : dummy;
  c = kBad;
  const scx_block* (*entry)() = (const scx_block* (*)())(void*)(mem + h.entry);
  const scx_block* d = entry();
  const uint8_t* lo = mem;
  const uint8_t* hi = mem + mem_bytes(h);
  auto in = [&](const void* p) { return (const uint8_t*)p >= lo && (const uint8_t*)p < hi; };
  if (!d || !in(d)) return nullptr;
  if (d->abi != SCX_ABI) { c = kAbi; return nullptr; }
  int nl = 0;
  while (nl < SCX_NAME_MAX && h.name[nl]) ++nl;
  if (!in(d->name) || !in(d->name + nl) || strncmp(d->name, h.name, (size_t)nl) || d->name[nl]) return nullptr;
  if (d->nparams > SCX_PARAMS_MAX || (d->nparams && !in(d->params))) return nullptr;
  for (uint32_t i = 0; i < d->nparams; ++i)
    if (!in(d->params[i].name) || d->params[i].def > 16383) return nullptr;
  if (d->state_bytes > max_state) { c = kFull; return nullptr; }
  // the functions: in the read + execute part
  const uint8_t* xhi = mem + rx_bytes(h);
  auto rx = [&](const void* p) { return (const uint8_t*)p >= lo && (const uint8_t*)p < xhi; };
  if (!rx((const void*)d->make) || !rx((const void*)d->process) || !rx((const void*)d->set_param)) return nullptr;
  if ((d->midi && !rx((const void*)d->midi)) || (d->link_reset && !rx((const void*)d->link_reset))) return nullptr;
  c = kOk;
  return d;
}

}  // namespace code
}  // namespace core
