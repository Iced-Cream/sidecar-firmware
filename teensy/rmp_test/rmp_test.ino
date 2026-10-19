// teensy/rmp_test/rmp_test.ino - load sine_test into the arena, play a note,
// check the pitch from zero crossings, print the cycles per 12-sample block.
// Then rmp_arena_reset() and load it again (the project-change path).
// sine_pkg.h / fault_pkg.h (git-ignored) are the packages as C arrays; regenerate them after a plugin rebuild:
//   xxd -i plugins/sine_test/sine_test.rmp | sed 's/sine_test_rmp/sine_pkg/g' > teensy/rmp_test/sine_pkg.h
//   xxd -i plugins/fault_test/fault_test.rmp | sed 's/fault_test_rmp/fault_pkg/g' > teensy/rmp_test/fault_pkg.h
#include <Arduino.h>
#include <rmp_arena.h>
#include <rmp_loader.h>
#include <rmp_state.h>
DMAMEM static uint8_t state_pool[24576] __attribute__((aligned(8)));   // the 24 KB fast pool
#include <rmp_api.h>
#include <rmp_store.h>
#include <rmp_guard.h>
#include "sine_pkg.h"
#include "fault_pkg.h"

extern "C" uint8_t external_psram_size;
static void h_log(const char *m) { Serial.printf("plugin: %s\n", m); }
static uint32_t h_random(uint32_t *s) { uint32_t x = *s ? *s : 2463534242u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static rmp_host host = {RMP_API, 48000.0f, 12, h_log, h_random, {0}};

static void run_once(const char *tag) {
  rmp_info info;
  rmp_status st = rmp_verify(sine_pkg, sine_pkg_len, &info);
  Serial.printf("[%s] verify %d (%s, image %lu, bss %lu)\n", tag, st, info.id, (unsigned long)info.image_size, (unsigned long)info.bss_size);
  uint8_t *img = rmp_arena_alloc(info.image_size + info.bss_size);
  if (!img) { Serial.printf("[%s] arena full\n", tag); return; }
  st = rmp_place(&info, img, (uint32_t)img);
  rmp_arena_sync(img, info.image_size);
  Serial.printf("[%s] place %d at %p (arena used %lu)\n", tag, st, img, (unsigned long)rmp_arena_used());
  const rmp_plugin *p = (const rmp_plugin *)(img + info.entry_offset);
  Serial.printf("[%s] init %ld\n", tag, (long)p->init(&host));
  rmp_state_set_pool(state_pool, sizeof state_pool);
  rmp_state_begin();
  uint8_t *state = (uint8_t *)rmp_state_alloc(p->state_bytes);
  Serial.printf("[%s] state %p, %lu bytes; OCRAM used %lu, PSRAM used %lu\n", tag, state, (unsigned long)p->state_bytes,
                (unsigned long)rmp_state_used_ocram(), (unsigned long)rmp_state_used_psram());
  p->voice_init(state);
  float prm[16]; for (float &x : prm) x = 64.0f; prm[1] = 127.0f; prm[2] = 127.0f;   // tune 0, level 1, decay ~4 s
  p->params(state, prm);
  p->note_on(state, 69.0f, 1.0f);
  static void (*volatile render)(void *, float *, int32_t);   // volatile: no hoisting out of the DWT window
  render = p->render;
  static float out[4800]; uint32_t cyc = 0;
  for (int i = 0; i < 4800; i += 12) {
    void (*r)(void *, float *, int32_t) = render;
    uint32_t t0 = ARM_DWT_CYCCNT;
    r(state, out + i, 12);
    asm volatile("" ::: "memory");
    cyc += ARM_DWT_CYCCNT - t0;
  }
  int zc = 0; for (int i = 1; i < 4800; ++i) zc += (out[i - 1] < 0) != (out[i] < 0);
  float peak = 0; for (int i = 0; i < 4800; ++i) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
  uint32_t per = cyc / 400;
  Serial.printf("[%s] pitch ~%.1f Hz (expect 440), peak %.3f, %lu cycles per block (%.2f %% of the Teensy)\n",
                tag, zc * 48000.0f / 2.0f / 4800.0f, peak, (unsigned long)per, 100.0f * per / (600e6f / 4000.0f));
}

static void list_cb(const char *i, uint32_t s, void *) { Serial.printf("installed: %s (%lu B)\n", i, (unsigned long)s); }
static int count_cb_n;
static void count_cb(const char *, uint32_t, void *) { ++count_cb_n; }

// Run 1 (empty store): install + read back + remove + re-install. Run 2 after a
// re-upload (store survives): the list shows the package BEFORE anything is installed.
static void store_test() {
  uint32_t t0 = micros();
  bool m = rmp_store_begin();
  Serial.printf("store mount %d (%lu us)\n", m, (unsigned long)(micros() - t0));
  Serial.println("boot listing (before any install):");
  rmp_store_list(list_cb, nullptr);
  count_cb_n = 0; rmp_store_list(count_cb, nullptr);
  Serial.printf("boot count %d\n", count_cb_n);
  bool had = count_cb_n > 0;
  static uint8_t back[32768]; uint32_t blen;
  for (int round = 0; round < (had ? 1 : 2); ++round) {
    t0 = micros();
    bool ok = rmp_store_install_begin(sine_pkg_len);
    for (uint32_t i = 0; ok && i < sine_pkg_len; i += 256) ok = rmp_store_install_write(sine_pkg + i, min(256u, sine_pkg_len - i));
    char id[32] = {0}; rmp_status cs = rmp_store_install_commit(id);
    uint32_t dt = micros() - t0;
    Serial.printf("install ok=%d status %d -> %s, %lu us (%lu B)\n", ok, cs, id, (unsigned long)dt, (unsigned long)sine_pkg_len);
    blen = sizeof back;
    Serial.printf("read back %d, same %d\n", rmp_store_read("rytm.sine_test", back, &blen), blen == sine_pkg_len && !memcmp(back, sine_pkg, blen));
    rmp_store_list(list_cb, nullptr);
    if (round == 0 && !had) {
      t0 = micros();
      bool rm = rmp_store_remove("rytm.sine_test");
      Serial.printf("remove %d (%lu us)\n", rm, (unsigned long)(micros() - t0));
      count_cb_n = 0; rmp_store_list(count_cb, nullptr);
      blen = sizeof back;
      Serial.printf("after remove: list count %d, read %d (expect 0, 0)\n", count_cb_n, rmp_store_read("rytm.sine_test", back, &blen));
      Serial.printf("remove missing %d (expect 0)\n", rmp_store_remove("rytm.sine_test"));
    }
  }
  // id safety
  blen = sizeof back;
  const char *bad[] = {"", "../x", "a/b", "a\\b", ".hidden", "a..b", "..", "/etc"};
  int refused = 0;
  for (const char *b : bad) refused += !rmp_store_read(b, back, &blen) && !rmp_store_remove(b);
  Serial.printf("unsafe ids refused %d of %d\n", refused, (int)(sizeof bad / sizeof *bad));
  // a corrupted package (one body byte flipped) is refused; the installed one stays intact
  {
    static uint8_t bad[sizeof sine_pkg]; memcpy(bad, sine_pkg, sine_pkg_len); bad[200] ^= 1;
    bool ok = rmp_store_install_begin(sine_pkg_len);
    for (uint32_t i = 0; ok && i < sine_pkg_len; i += 256) ok = rmp_store_install_write(bad + i, min(256u, sine_pkg_len - i));
    char id[32] = {0}; rmp_status cs = rmp_store_install_commit(id);
    blen = sizeof back;
    bool rd = rmp_store_read("rytm.sine_test", back, &blen);
    Serial.printf("corrupt install: written %d, status %d (expect %d RMP_ERR_CRC), pending %d (expect 0), old intact %d (expect 1)\n", ok, cs, (int)RMP_ERR_CRC,
                  rmp_store_pending(), rd && blen == sine_pkg_len && !memcmp(back, sine_pkg, blen));
    blen = 10;
    Serial.printf("read with a 10 B buffer: %d, *len = %lu (expect 0, %lu)\n", rmp_store_read("rytm.sine_test", back, &blen), (unsigned long)blen, (unsigned long)sine_pkg_len);
  }
  // a leftover install file is removed by rmp_store_begin
  {
    bool ok = rmp_store_install_begin(sine_pkg_len) && rmp_store_install_write(sine_pkg, 256);
    bool p1 = rmp_store_pending();
    bool m2 = rmp_store_begin();
    Serial.printf("leftover: started %d, pending %d, begin again %d, pending %d (expect 1 1 1 0), write after begin %d (expect 0)\n", ok, p1, m2, rmp_store_pending(),
                  rmp_store_install_write(sine_pkg + 256, 256));
  }
  // oversize install refused
  Serial.printf("oversize install_begin %d (expect 0)\n", rmp_store_install_begin(RMP_STORE_MAX_PKG + 1));
  Serial.printf("final: installed %s\n", "list follows");
  rmp_store_list(list_cb, nullptr);
}

// The guard: fault_test's four modes through rmp_guard_call, then the guard's own
// rules (a fault with interrupts masked, a nested call), then the plugin system
// is used again (mode 0, sine's pitch).
struct gcall { const rmp_plugin *p; void *st; float *out; };
static void g_render(void *a) { gcall *c = (gcall *)a; c->p->render(c->st, c->out, 12); }
static void g_masked_fault(void *) { __disable_irq(); *(volatile int *)0 = 1; }
static void g_nested(void *a) { *(rmp_guard_result *)a = rmp_guard_call(g_render, nullptr, 0); }
static void guard_test() {
  rmp_info info;
  rmp_status st = rmp_verify(fault_pkg, fault_pkg_len, &info);
  uint8_t *img = rmp_arena_alloc(info.image_size + info.bss_size);
  if (st != RMP_OK || !img) { Serial.printf("fault_test: verify %d, arena %p\n", st, img); return; }
  st = rmp_place(&info, img, (uint32_t)img);
  rmp_arena_sync(img, info.image_size);
  const rmp_plugin *fp = (const rmp_plugin *)(img + info.entry_offset);
  Serial.printf("fault_test: place %d at %p, init %ld\n", st, img, (long)fp->init(&host));
  void *fs_state = rmp_state_alloc(fp->state_bytes);
  fp->voice_init(fs_state);
  static float o[12];
  float prm[16] = {0};
  const uint32_t limit = 600000;   // 1 ms
  for (int mode : {0, 1, 2, 3, 0}) {
    prm[0] = mode * 32 + 1; fp->params(fs_state, prm);
    gcall c{fp, fs_state, o};
    uint32_t pm0; asm volatile("mrs %0, primask" : "=r"(pm0));
    rmp_guard_result r = rmp_guard_call(g_render, &c, limit);
    uint32_t pm1; asm volatile("mrs %0, primask" : "=r"(pm1));
    uint32_t cyc = rmp_guard_last_cycles();
    Serial.printf("mode %d -> %d (%lu cycles%s), primask %lu/%lu\n", mode, r, (unsigned long)cyc,
                  r == RMP_GUARD_TIMEOUT ? (cyc - limit <= 300000 ? ", within a tick of the limit" : ", MORE than a tick past") : "",
                  (unsigned long)pm0, (unsigned long)pm1);
  }
  uint32_t pm1;
  rmp_guard_result r = rmp_guard_call(g_masked_fault, nullptr, limit);
  asm volatile("mrs %0, primask" : "=r"(pm1));
  Serial.printf("fault with IRQs masked -> %d (expect 1), primask after %lu (expect 0)\n", r, (unsigned long)pm1);
  rmp_guard_result inner = RMP_GUARD_OK;
  r = rmp_guard_call(g_nested, &inner, limit);
  Serial.printf("nested call: outer %d, inner %d (expect 0, 3)\n", r, inner);
  Serial.println("still alive");
}

// The arena's MPU edges (8 KB subregions of 64 KB regions): a 'bx lr' at its
// first and last halfword runs; a jump to arena + RMP_ARENA_BYTES or to arena - 4
// (plain DTCM, no-execute) faults - IACCVIOL, returned by the guard as FAULT.
static void g_jump(void *a) { ((void (*)())((uint32_t)a | 1u))(); }
static void arena_bounds_test() {
  uint8_t *a = rmp_arena_base();
  uint16_t *last = (uint16_t *)(a + RMP_ARENA_BYTES - 2);
  uint16_t keep = *last;
  *last = 0x4770;   // bx lr (the arena's end is unused here: the images sit at its start)
  rmp_arena_sync(last, 2);
  rmp_guard_result r_last = rmp_guard_call(g_jump, last, 600000);
  *last = keep;
  rmp_guard_result r_end = rmp_guard_call(g_jump, a + RMP_ARENA_BYTES, 600000);
  rmp_guard_result r_before = rmp_guard_call(g_jump, a - 4, 600000);
  Serial.printf("arena edges: run its last halfword -> %d (expect 0), jump to arena+%lu -> %d (expect 1), to arena-4 -> %d (expect 1)\n",
                r_last, (unsigned long)RMP_ARENA_BYTES, r_end, r_before);
}

void setup() {
  while (!Serial && millis() < 3000) {}
  Serial.printf("rmp_test: DWT_CTRL %08lx (CYCCNTENA bit0), external_psram_size %u MB\n", (unsigned long)ARM_DWT_CTRL, external_psram_size);
  rmp_arena_begin();
  uint32_t cb, ca, rbar, rasr, rbar2, rasr2;
  rmp_arena_mpu_readback(&cb, &ca, &rbar, &rasr, &rbar2, &rasr2);
  Serial.printf("MPU_CTRL before %lu after %lu (%s); region %d RBAR %08lx RASR %08lx, region %d RBAR %08lx RASR %08lx; arena at %p\n",
                (unsigned long)cb, (unsigned long)ca, cb == ca ? "same" : "DIFFERENT", RMP_ARENA_REGION, (unsigned long)rbar,
                (unsigned long)rasr, RMP_ARENA_REGION2, (unsigned long)rbar2, (unsigned long)rasr2, rmp_arena_base());
  run_once("load 1");
  rmp_arena_reset(); rmp_state_reset();
  Serial.printf("reset: arena used %lu, OCRAM used %lu\n", (unsigned long)rmp_arena_used(), (unsigned long)rmp_state_used_ocram());
  run_once("load 2");
  store_test();
  rmp_guard_begin();                           // before CrashReport, whose print clears SRC_SRSR
  Serial.printf("safe mode %d (SRC_SRSR was %08lx)\n", rmp_guard_safe_mode(), (unsigned long)rmp_guard_reset_reason());
  if (CrashReport) Serial.print(CrashReport);
  { uint32_t c0 = WDOG3_CNT, t0 = micros(); delay(50); uint32_t c1 = WDOG3_CNT, t1 = micros();
    Serial.printf("RTWDOG: CS %08lx TOVAL %lu, %lu ticks in %lu us (%.0f Hz)\n", (unsigned long)WDOG3_CS, (unsigned long)WDOG3_TOVAL,
                  (unsigned long)(c1 - c0), (unsigned long)(t1 - t0), (c1 - c0) * 1e6f / (t1 - t0)); }
  rmp_guard_feed();
  guard_test();
  rmp_guard_feed();
  arena_bounds_test();
  run_once("after faults");
  Serial.println("done");
}
void loop() {
  rmp_guard_feed();
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'f') { Serial.println("fault outside a plugin"); Serial.flush(); delay(20); *(volatile int *)0 = 1; }   // core handler, then RTWDOG
    if (c == 'r') { Serial.println("software reset"); Serial.flush(); delay(20); SCB_AIRCR = 0x05FA0004; }
  }
}
