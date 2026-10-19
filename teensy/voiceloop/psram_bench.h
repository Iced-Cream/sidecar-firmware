// psram_bench.h - ':psram' and ':psram bench [ID]': the PSRAM code arena's status and the testers' self-test
// (docs/psram-testing.md). Included by voiceloop.ino after webui.h; all of it in flash (FLASHMEM), loop() only.
//
// ':psram'            PSRAM size, both arenas' use, the MPU regions 11/12 read back, the PSRAM limits, each
//                     loaded slot's placement.
// ':psram bench [ID]' the package ID (default rytm.sine_test, which must be installed) placed twice, in the
//                     free tail of each arena (the project's slots keep theirs: the bump marks are restored
//                     afterwards), then: the PSRAM copy read back past the D-cache (CRC), init, voice_init,
//                     params, note_on and kBenchBlocks renders warm, kBenchCold renders with the plugin's lines
//                     evicted first (D-cache clean+invalidate of the image, I-cache invalidated), both copies'
//                     output compared bit for bit; then the stress check: kBenchReloads PSRAM reloads (each CRC'd),
//                     8 voices x kBenchRounds blocks from the PSRAM copy with a 64 KB PSRAM sweep (twice the
//                     D-cache) between rounds, every call against the PSRAM budget and hard limit.
//                     Blocks loop() for about a second: the audio stops meanwhile (run it with the transport
//                     stopped). Needs PSRAM (the states and the sweep buffer are taken from it for the run).
#pragma once
#ifndef SCB_ICIALLU
#define SCB_ICIALLU (*(volatile uint32_t*)0xE000EF50)
#endif

const char kBenchDefault[] PROGMEM = "rytm.sine_test";
const int kBenchBlocks = 2000, kBenchCold = 200, kBenchReloads = 20, kBenchRounds = 500;
int pb_where = RMP_PLACE_INTERNAL;
FLASHMEM uint8_t* pb_alloc(uint32_t bytes) { return rmp_arena_alloc_in(pb_where, bytes); }
FLASHMEM void pb_noreset() {}
const rmp_platform pb_pf = {rmp_store_size, rmp_store_read_at, pb_alloc, pb_noreset, rmp_arena_sync, plug_slots_log, &plug_host_v2};

FLASHMEM void psram_status() {
  logf(PSTR("psram: %d MB fitted; code arena internal %lu of %lu B, PSRAM %lu of %lu B%s"), (int)external_psram_size,
       (unsigned long)rmp_arena_used(), (unsigned long)RMP_ARENA_BYTES, (unsigned long)rmp_arena_psram_used(),
       (unsigned long)rmp_arena_psram_size(), rmp_arena_psram_size() ? PSTR("") : PSTR(" (none: the stock behaviour)"));
  if (rmp_arena_psram_size()) {
    uint32_t rb, ra, rb2, ra2;
    rmp_arena_psram_mpu(&rb, &ra, &rb2, &ra2);
    logf(PSTR("psram: arena at %p; MPU region %d RBAR %08lx RASR %08lx, region %d RBAR %08lx RASR %08lx (XN bit 28 must be 0)"),
         rmp_arena_psram_base(), RMP_PSRAM_REGION, (unsigned long)rb, (unsigned long)ra, RMP_PSRAM_REGION2, (unsigned long)rb2,
         (unsigned long)ra2);
    logf(PSTR("psram: limits for PSRAM code: budget x%u, timeout x%u + %u cycles"), (unsigned)RMP_PSRAM_LIMIT_X,
         (unsigned)RMP_PSRAM_LIMIT_X, (unsigned)RMP_PSRAM_COLD_CYCLES);
  }
  for (int s = 0; s < RMP_SLOTS; ++s)
    if (plug[s].state == RMP_SLOT_READY || plug[s].state == RMP_SLOT_MUTED)
      logf(PSTR("psram: U%d %s %s, in %s"), s + 1, plug_map.slot[s].id, plug_state_name(plug[s].state), rmp_place_name(plug_place[s]));
}

// One copy of package id placed in arena `where`: its plugin table, the image (*img, *bytes); false with *why.
FLASHMEM bool pb_place(const char* id, int where, const rmp_plugin** pp, uint8_t** img, uint32_t* bytes, const char** why) {
  uint32_t len;
  uint8_t h[RMP_HEADER_BYTES];
  rmp_info info;
  if (!rmp_store_size(id, &len)) { *why = PSTR("not installed"); return false; }
  if (len < RMP_HEADER_BYTES || !rmp_store_read_at(id, 0, h, RMP_HEADER_BYTES) || rmp_verify_header(h, len, &info) != RMP_OK) {
    *why = PSTR("bad header");
    return false;
  }
  rmp_slot_map m;
  memset(&m, 0, sizeof m);
  strncpy(m.slot[0].id, id, 31);
  m.slot[0].version = info.version;
  rmp_loaded out[RMP_SLOTS];
  memset(out, 0, sizeof out);
  pb_where = where;
  rmp_slots_load_one(&pb_pf, &m, 0, out);
  if (out[0].state != RMP_SLOT_READY) { *why = out[0].state == RMP_SLOT_NO_ROOM ? PSTR("no room in this arena") : PSTR("did not load"); return false; }
  *pp = out[0].plugin;
  *img = (uint8_t*)out[0].plugin - info.entry_offset;
  *bytes = info.image_size + info.bss_size;
  return true;
}
// The image read back from memory, past the D-cache: the same CRC as through it (the PSRAM holds what was written).
FLASHMEM bool pb_readback(uint8_t* img, uint32_t bytes) {
  uint32_t c1 = rmp_crc32_update(0xFFFFFFFFu, img, bytes);
  arm_dcache_flush_delete(img, bytes);   // clean (a no-op: rmp_arena_sync cleaned it) and invalidate
  uint32_t c2 = rmp_crc32_update(0xFFFFFFFFu, img, bytes);
  return c1 == c2;
}

struct PbRun {
  bool ok;
  const char* why;
  uint32_t init_cyc, first, avg, peak, cold_avg, cold_peak, hash;
};
FLASHMEM void pb_hash(uint32_t* h, const float* o) {
  const uint8_t* b = (const uint8_t*)o;
  for (int k = 0; k < kBlock * 4; ++k) *h = (*h ^ b[k]) * 16777619u;
}
// init, one voice from voice_init, warm and cold renders of the copy at img.
FLASHMEM PbRun pb_run(const rmp_plugin* p, uint8_t* img, uint32_t bytes, void* st, int where) {
  PbRun r = {false, PSTR(""), 0, 0, 0, 0, 0, 0, 2166136261u};
  float out[kBlock], prm[16];
  for (int k = 0; k < 16; ++k) prm[k] = 64.0f;
  PlugCall c = {p, st, out, prm, 60.0f, 1.0f, 0, nullptr, 0, nullptr};
  const uint32_t base = (uint32_t)(2ull * p->cpu_permille * (600000000u / 4000u) / 1000u) + 2000u;
  if (rmp_guard_call(pc_init, &c, rmp_limit_hard(kPlugInitCycles, where)) != RMP_GUARD_OK || c.ret) { r.why = PSTR("init failed"); return r; }
  r.init_cyc = rmp_guard_last_cycles();
  memset(st, 0, p->state_bytes);
  if (rmp_guard_call(pc_voice_init, &c, rmp_limit_hard(kPlugCallCycles, where)) != RMP_GUARD_OK ||
      rmp_guard_call(pc_params, &c, rmp_limit_hard(kPlugCallCycles, where)) != RMP_GUARD_OK ||
      rmp_guard_call(pc_note_on, &c, rmp_limit_hard(kPlugCallCycles, where)) != RMP_GUARD_OK) {
    r.why = PSTR("voice_init/params/note_on faulted or timed out");
    return r;
  }
  uint64_t sum = 0;
  for (int k = 0; k < kBenchBlocks; ++k) {
    if (rmp_guard_call(pc_render, &c, rmp_limit_hard(base, where)) != RMP_GUARD_OK) { r.why = PSTR("render faulted or timed out"); return r; }
    uint32_t cy = rmp_guard_last_cycles();
    if (!k) r.first = cy;
    sum += cy;
    if (cy > r.peak) r.peak = cy;
    pb_hash(&r.hash, out);
    if ((k & 255) == 0) rmp_guard_feed();
  }
  r.avg = (uint32_t)(sum / kBenchBlocks);
  sum = 0;
  for (int k = 0; k < kBenchCold; ++k) {   // the plugin's code and data out of both caches first
    arm_dcache_flush_delete(img, bytes);
    asm volatile("dsb; isb" ::: "memory");
    SCB_ICIALLU = 0;
    asm volatile("dsb; isb" ::: "memory");
    if (rmp_guard_call(pc_render, &c, rmp_limit_hard(base, where)) != RMP_GUARD_OK) { r.why = PSTR("cold render faulted or timed out"); return r; }
    uint32_t cy = rmp_guard_last_cycles();
    sum += cy;
    if (cy > r.cold_peak) r.cold_peak = cy;
    pb_hash(&r.hash, out);
  }
  r.cold_avg = (uint32_t)(sum / kBenchCold);
  r.ok = true;
  return r;
}
FLASHMEM void pb_say(const char* tag, const PbRun& r, uint32_t base) {
  if (!r.ok) { logf(PSTR("psram bench: %s: FAIL (%s)"), tag, r.why); return; }
  logf(PSTR("psram bench: %s: init %lu cyc; render first %lu, avg %lu (%lu permille), peak %lu, cold avg %lu, cold peak %lu cyc "
            "(budget %lu); output %08lx"),
       tag, (unsigned long)r.init_cyc, (unsigned long)r.first, (unsigned long)r.avg, (unsigned long)(r.avg / 150u),
       (unsigned long)r.peak, (unsigned long)r.cold_avg, (unsigned long)r.cold_peak, (unsigned long)base, (unsigned long)r.hash);
}

FLASHMEM void psram_bench(const char* id) {
  if (!*id) id = kBenchDefault;
  if (!external_psram_size || !rmp_arena_psram_size()) { logf(PSTR("psram bench: no PSRAM arena (none fitted, or no room): nothing to compare")); return; }
  if (plug_job_on || plug_todo || plug_act_slot >= 0 || plug_installing || plug_reload_pending) {
    logf(PSTR("psram bench: a slot load or an install is under way - try again in a moment"));
    return;
  }
  uint32_t m_int, m_ps;
  rmp_arena_mark(&m_int, &m_ps);
  uint32_t t0 = millis();
  const rmp_plugin *pi = nullptr, *pp = nullptr;
  uint8_t *ii = nullptr, *ip = nullptr;
  uint32_t bi = 0, bp = 0;
  const char *wi = PSTR(""), *wp = PSTR("");
  bool have_i = pb_place(id, RMP_PLACE_INTERNAL, &pi, &ii, &bi, &wi);
  bool have_p = pb_place(id, RMP_PLACE_PSRAM, &pp, &ip, &bp, &wp);
  void* st = nullptr;
  uint8_t* sweep = nullptr;
  void* sts[RMP_VOICES] = {nullptr};
  if (!have_p) {
    logf(PSTR("psram bench: %s: PSRAM copy: %s"), id, wp);
    goto done;
  }
  {
    const uint32_t sb = pp->state_bytes ? pp->state_bytes : 8;
    const uint32_t base = (uint32_t)(2ull * pp->cpu_permille * (600000000u / 4000u) / 1000u) + 2000u;
    const uint32_t budget = rmp_limit_budget(base, RMP_PLACE_PSRAM), hard = rmp_limit_hard(base, RMP_PLACE_PSRAM);
    logf(PSTR("psram bench: %s, %lu B code+data, state %lu B, declares %lu permille; internal copy at %p%s%s, PSRAM copy at %p"),
         id, (unsigned long)bp, (unsigned long)pp->state_bytes, (unsigned long)pp->cpu_permille, have_i ? (void*)ii : nullptr,
         have_i ? PSTR("") : PSTR(" - "), have_i ? PSTR("") : wi, ip);
    bool rb = pb_readback(ip, bp);
    logf(PSTR("psram bench: PSRAM read-back past the D-cache: %s"), rb ? PSTR("same") : PSTR("DIFFERENT - FAIL"));
    st = extmem_malloc(sb);
    if (!st || ((uint32_t)st >> 28) != 7) { logf(PSTR("psram bench: no PSRAM for the state")); goto done; }
    PbRun ri = {false, PSTR("no internal copy"), 0, 0, 0, 0, 0, 0, 0};
    if (have_i) ri = pb_run(pi, ii, bi, st, RMP_PLACE_INTERNAL);
    pb_say(PSTR("internal"), ri, base);
    rmp_guard_feed();
    PbRun rp = pb_run(pp, ip, bp, st, RMP_PLACE_PSRAM);
    pb_say(PSTR("psram   "), rp, budget);
    rmp_guard_feed();
    if (ri.ok && rp.ok) {
      logf(PSTR("psram bench: PSRAM / internal: warm avg x%lu.%02lu, cold avg x%lu.%02lu; output %s"),
           (unsigned long)(rp.avg / (ri.avg ? ri.avg : 1)), (unsigned long)(rp.avg * 100u / (ri.avg ? ri.avg : 1) % 100u),
           (unsigned long)(rp.cold_avg / (ri.cold_avg ? ri.cold_avg : 1)),
           (unsigned long)(rp.cold_avg * 100u / (ri.cold_avg ? ri.cold_avg : 1) % 100u), ri.hash == rp.hash ? PSTR("identical") : PSTR("DIFFERENT - FAIL"));
    }
    if (!rp.ok) goto done;
    // ---- stress: reloads, then 8 voices with the D-cache swept between rounds ----
    uint32_t reload_bad = 0, reload_fail = 0;
    for (int k = 0; k < kBenchReloads; ++k) {
      rmp_arena_release(rmp_arena_used(), m_ps);   // the PSRAM copy's place again (the internal copy stays)
      const rmp_plugin* q;
      uint8_t* iq;
      uint32_t bq;
      const char* w;
      if (!pb_place(id, RMP_PLACE_PSRAM, &q, &iq, &bq, &w)) { ++reload_fail; continue; }
      if (!pb_readback(iq, bq)) ++reload_bad;
      pp = q, ip = iq, bp = bq;
      rmp_guard_feed();
    }
    logf(PSTR("psram bench: %d PSRAM reloads: %lu failed, %lu read back different"), kBenchReloads, (unsigned long)reload_fail,
         (unsigned long)reload_bad);
    PlugCall c0 = {pp, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr};
    if (rmp_guard_call(pc_init, &c0, rmp_limit_hard(kPlugInitCycles, RMP_PLACE_PSRAM)) != RMP_GUARD_OK || c0.ret) {
      logf(PSTR("psram bench: stress: init failed"));
      goto done;
    }
    sweep = (uint8_t*)extmem_malloc(65536);
    float out[kBlock], prm[16];
    for (int k = 0; k < 16; ++k) prm[k] = 64.0f;
    int nv = 0;
    for (; nv < RMP_VOICES; ++nv) {
      sts[nv] = extmem_malloc(sb);
      if (!sts[nv] || ((uint32_t)sts[nv] >> 28) != 7) break;
      memset(sts[nv], 0, sb);
      PlugCall c = {pp, sts[nv], out, prm, 48.0f + 3.0f * nv, 1.0f, 0, nullptr, 0, nullptr};
      if (rmp_guard_call(pc_voice_init, &c, rmp_limit_hard(kPlugCallCycles, RMP_PLACE_PSRAM)) != RMP_GUARD_OK ||
          rmp_guard_call(pc_params, &c, rmp_limit_hard(kPlugCallCycles, RMP_PLACE_PSRAM)) != RMP_GUARD_OK ||
          rmp_guard_call(pc_note_on, &c, rmp_limit_hard(kPlugCallCycles, RMP_PLACE_PSRAM)) != RMP_GUARD_OK)
        break;
    }
    uint32_t over_budget = 0, over_hard = 0, guard_bad = 0, peak = 0, round_peak = 0, mute3 = 0;
    uint32_t ov[RMP_VOICES] = {0};
    uint64_t sum = 0, n = 0;
    volatile uint32_t sink = 0;
    for (int r = 0; r < kBenchRounds && nv; ++r) {
      if (sweep)
        for (uint32_t k = 0; k < 65536; k += 32) sink += sweep[k];   // every line of 64 KB: the D-cache turned over
      uint32_t round = 0;
      for (int vi = 0; vi < nv; ++vi) {
        PlugCall c = {pp, sts[vi], out, prm, 0, 0, 0, nullptr, 0, nullptr};
        if (rmp_guard_call(pc_render, &c, hard) != RMP_GUARD_OK) { ++guard_bad; continue; }
        uint32_t cy = rmp_guard_last_cycles();
        round += cy;
        sum += cy, ++n;
        if (cy > peak) peak = cy;
        if (cy > hard) ++over_hard;
        if (cy > budget) {
          ++over_budget;
          if (++ov[vi] >= 3) ++mute3;   // what plug_render would have muted ("over budget", 3 in a row)
        } else {
          ov[vi] = 0;
        }
      }
      if (round > round_peak) round_peak = round;
      if ((r & 63) == 0) rmp_guard_feed();
    }
    uint32_t avg = n ? (uint32_t)(sum / n) : 0;
    bool pass = nv == RMP_VOICES && !guard_bad && !over_hard && !mute3 && !reload_fail && !reload_bad && rb && ri.hash == rp.hash;
    logf(PSTR("psram bench: stress %d voices x %d blocks, 64 KB PSRAM sweep a round: avg %lu cyc, peak %lu (budget %lu, timeout %lu); "
              "over budget %lu, would mute %lu, over timeout %lu, guard faults/timeouts %lu; 8-voice block peak %lu cyc = %lu permille of a block"),
         nv, kBenchRounds, (unsigned long)avg, (unsigned long)peak, (unsigned long)budget, (unsigned long)hard,
         (unsigned long)over_budget, (unsigned long)mute3, (unsigned long)over_hard, (unsigned long)guard_bad,
         (unsigned long)round_peak, (unsigned long)(round_peak / 150u));
    logf(PSTR("psram bench: %s (%lu ms)"), pass ? PSTR("PASS") : (ri.ok ? PSTR("FAIL") : PSTR("FAIL (or no internal copy to compare: unload slots)")),
         (unsigned long)(millis() - t0));
  }
done:
  for (int vi = 0; vi < RMP_VOICES; ++vi)
    if (sts[vi]) extmem_free(sts[vi]);
  if (sweep) extmem_free(sweep);
  if (st) extmem_free(st);
  rmp_arena_release(m_int, m_ps);   // the project's slots keep their images; the bench's copies are free space again
  rmp_store_release();
}

FLASHMEM void psram_console(const char* a) {
  if (!strncmp(a, PSTR("bench"), 5) && (!a[5] || a[5] == ' ')) psram_bench(a[5] ? a + 6 : PSTR(""));
  else psram_status();
}

