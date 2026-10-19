// voiceloop's send FX in the USB-host build (RYTM_USB_HOST 1): included into usb_host.h (voiceloop.ino's namespace).
// The core's SendFx (core/fx/send_fx.h) with the Teensy's small set: CHORUS, COMB, CRUSH, RINGMOD, HALO (RYTM_SENDFX_LARGE
// 0, 16-bit delay lines; RINGS / CLOUDSEED / STUTTER stay MISSING). As the Pi's (pi/engine/sendfx_pi.cpp):
//   - Rytm -> host (sfx_rx, the USB host interrupt after each Overbridge IN transfer): the 8 track stems (IN 2..9,
//     post the Rytm's analog chain) x each track's SEND FX 1 / 2 level -> both effects -> the PRE pair and the POST
//     pair, kept in sfx_ret (int16, 128 frames);
//   - host -> Rytm (sfx_tx, the same interrupt, once an OUT block): the pairs added to OUT 3-4 (MAIN FX: PRE, through
//     the analog distortion + compressor) and OUT 1-2 (MAIN: POST, clean), per FX slot's P1 = ROUTE (0..63 PRE,
//     64..127 POST; ':sendfxout=fx|main' forces one pair).
// The Rytm is the clock: an OUT block goes out for each IN block, so the ring holds ~kSfxPrime frames (1.2 ms, the
// stems batched by three transfers before each Process); an empty ring is silence and primes again (counted).
// Nothing runs while no FX slot plays (both OFF or MISSING).
// Memory: the SendFx in RAM1 (DTCM, ~17 KB: no PSRAM, nothing allocated), its code in program flash (FX_DSP).
// Console: ':sendfx' (the core's answer, then this side's stats since the last: frames, cycles a 12-frame block mean
// and max, the interrupt's load, underruns, drops, peak, faults), ':sendfx=K:TYPE', ':sendfxout=', ':sendlvl=',
// ':sendknob=' (the core's), ':sendfxbench' (both slots on synthetic stems, here in loop(): cycles a 12-frame block
// per type pair).

alignas(8) uint8_t sfx_mem[sizeof(core::fx::SendFx)];   // RAM1 (bss): placed by sfx_begin, not a static constructor
core::fx::SendFx* sfx = nullptr;
constexpr int kSfxRing = 128, kSfxChunk = 48, kSfxBatch = 42;   // frames: a Process call takes kSfxBatch (three transfers)
constexpr int kSfxPrime = kSfxBatch + 16;   // the return ring's level before the OUT side starts reading (1.2 ms)
int16_t sfx_ret[kSfxRing][4];   // PRE L R, POST L R
volatile uint32_t sfx_wr = 0, sfx_rd = 0;
bool sfx_primed = false;
volatile uint32_t sfx_frames = 0, sfx_cyc_max = 0, sfx_under = 0, sfx_over = 0, sfx_calls = 0, sfx_n_max = 0;
volatile uint64_t sfx_cyc_sum = 0;   // the interrupt's cycles in the effects since the last ':sendfx'
uint32_t sfx_stat_t0 = 0;

FLASHMEM void sfx_begin() {
  sfx = new (sfx_mem) core::fx::SendFx(48000.0f);
  sfx_stat_t0 = millis();
  logf(PSTR("sendfx: CHORUS COMB CRUSH RINGMOD HALO, %u bytes in RAM1 (RINGS CLOUDSEED STUTTER: not in this build)"),
       (unsigned)sizeof(core::fx::SendFx));
}

FLASHMEM int16_t sfx_s16(float x) {
  float v = x * 32767.0f;
  return (int16_t)(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
}

// The IN interrupt: n frames of ch channels (int32, full scale 2^31), stems on 2..9. The stems wait in sfx_pend until
// kSfxBatch frames are there (three 14-frame transfers), then one Process call: its per-call costs (~3600 cycles:
// the targets, the ramps, the limit checks) shared by three times the frames (':sendfxbench=N' measures N-frame calls).
float sfx_pend[kSfxChunk][core::fx::kStems];
int sfx_np = 0;
FLASHMEM void sfx_run() {   // sfx_pend's sfx_np frames through the effects, onto the return ring
  float pre[kSfxChunk][2], post[kSfxChunk][2];
  const int k = sfx_np;
  sfx_np = 0;
  sfx->Process(sfx_pend, k, pre, core::fx::control, post);
  uint32_t wr = sfx_wr;
  for (int j = 0; j < k; ++j) {
    if (wr - sfx_rd >= (uint32_t)kSfxRing) {   // the OUT side stopped taking: dropped
      sfx_over += k - j;
      break;
    }
    int16_t* r = sfx_ret[wr & (kSfxRing - 1)];
    r[0] = sfx_s16(pre[j][0]), r[1] = sfx_s16(pre[j][1]), r[2] = sfx_s16(post[j][0]), r[3] = sfx_s16(post[j][1]);
    ++wr;
  }
  __asm__ volatile("" ::: "memory");
  sfx_wr = wr;
}
FLASHMEM void sfx_rx(const int32_t* f, int n, int ch) {
  if (!sfx || ch < 10) return;
  if (!core::fx::any_on()) {   // nothing plays: the rings emptied, primed again at the next start
    sfx_rd = sfx_wr;
    sfx_primed = false;
    sfx_np = 0;
    return;
  }
  const uint32_t t0 = ARM_DWT_CYCCNT;
  for (int i = 0; i < n; ++i) {
    float* d = sfx_pend[sfx_np++];
    for (int v = 0; v < core::fx::kStems; ++v) d[v] = (float)f[i * ch + 2 + v] * (1.0f / 2147483648.0f);
    if (sfx_np >= kSfxBatch) sfx_run();
  }
  const uint32_t dt = ARM_DWT_CYCCNT - t0;
  sfx_cyc_sum += dt;
  sfx_frames += n;
  ++sfx_calls;
  if ((uint32_t)n > sfx_n_max) sfx_n_max = n;
  if (dt > sfx_cyc_max) sfx_cyc_max = dt;   // a call's (one with a batch is the long one)
}

FLASHMEM int32_t sfx_add(int32_t a, int16_t r) {
  const int64_t s = (int64_t)a + ((int64_t)r << 16);
  return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
}
// The OUT interrupt: add n frames to f (int32, saturated by the library) on 0/1 (POST) and 2/3 (PRE).
FLASHMEM void sfx_tx(int32_t* f, int n, int ch) {
  if (ch < 4) return;
  uint32_t rd = sfx_rd;
  const uint32_t wr = sfx_wr;
  if (!sfx_primed) {
    if (wr - rd < (uint32_t)kSfxPrime) return;
    sfx_primed = true;
  }
  for (int i = 0; i < n; ++i) {
    if (rd == wr) {   // ran dry: silence, and prime again
      ++sfx_under;
      sfx_primed = false;
      break;
    }
    const int16_t* r = sfx_ret[rd & (kSfxRing - 1)];
    int32_t* o = f + i * ch;
    o[2] = sfx_add(o[2], r[0]), o[3] = sfx_add(o[3], r[1]);
    o[0] = sfx_add(o[0], r[2]), o[1] = sfx_add(o[1], r[3]);
    ++rd;
  }
  __asm__ volatile("" ::: "memory");
  sfx_rd = rd;
}

// ':sendfxbench': both slots on 8 synthetic stems (all sends full), 400 blocks of 12 frames each pair of types, here
// in loop() with interrupts off around each block (the interrupt's own cost: ':sendfx' while the Rytm streams). It
// runs on the one SendFx (no second 17 KB anywhere), so only while both FX are off (the interrupt leaves it alone
// then); reset after. A developer's tool (RYTM_DEBUG_TOOLS: not in the release build, host_stereo.sh; its Halo
// put 8.3 KB on loop()'s stack).
#if RYTM_DEBUG_TOOLS
FLASHMEM void sfx_bench(int bn) {   // bn frames a Process call (12, or the interrupt's chunk), reported per 12 frames
  bn = bn < 1 ? 12 : bn > kSfxChunk ? kSfxChunk : bn;
  static const uint8_t kPairs[][2] = {{core::fx::kChorus, core::fx::kComb}, {core::fx::kChorus, core::fx::kChorus},
                                      {core::fx::kComb, core::fx::kComb}, {core::fx::kCrush, core::fx::kRingMod},
                                      {core::fx::kHalo, core::fx::kChorus}, {core::fx::kHalo, core::fx::kHalo}};
  static const char* const kNames[] = {"chorus + comb", "chorus x2", "comb x2", "crush + ringmod", "halo + chorus", "halo x2"};
  core::fx::Control c;
  memset((void*)&c, 0, sizeof c);
  for (int t = 0; t < core::fx::kTracks; ++t) c.send[t][0] = c.send[t][1] = 127;
  if (!sfx || core::fx::any_on()) {
    logf(PSTR("sendfxbench: only while both FX are off (it borrows the live effects): ':sendfx=off' first"));
    return;
  }
  core::fx::SendFx* b = sfx;
  b->Reset();
  b->TakePeak();
  float st[kSfxChunk][core::fx::kStems], pre[kSfxChunk][2], post[kSfxChunk][2];
  uint32_t rng = 1;
  for (int lv = 0; lv < 2; ++lv)   // full-scale noise on all 8 stems (the worst case: the exact limit), then -20 dB
  for (unsigned p = 0; p < sizeof kPairs / sizeof kPairs[0]; ++p) {
    const float amp = lv ? 0.1f : 1.0f;
    for (int s = 0; s < 2; ++s) {
      c.knob[s][0] = kPairs[p][s];
      c.knob[s][1] = s ? 127 : 0;   // FX1 PRE, FX2 POST
      for (int j = 2; j < 6; ++j) c.knob[s][j] = 64;
    }
    uint32_t sum = 0, mx = 0;
    for (int blk = 0; blk < 440; ++blk) {
      for (int i = 0; i < bn; ++i)
        for (int v = 0; v < core::fx::kStems; ++v)
          st[i][v] = amp * ((float)((rng = rng * 1664525u + 1013904223u) >> 9) * (1.0f / 4194304.0f) - 1.0f);
      __disable_irq();
      const uint32_t t0 = ARM_DWT_CYCCNT;
      b->Process(st, bn, pre, c, post);
      const uint32_t dt = ARM_DWT_CYCCNT - t0;
      __enable_irq();
      if (blk < 40) continue;   // the fades in
      sum += dt;
      if (dt > mx) mx = dt;
    }
    const uint32_t mean = (uint32_t)((uint64_t)sum * 12 / (400u * bn)), budget = F_CPU_ACTUAL / 4000;   // 12 frames: 250 us
    mx = (uint32_t)((uint64_t)mx * 12 / bn);
    logf(PSTR("sendfxbench: %-16s %s %lu cycles a 12-frame block (max %lu; %d-frame calls): %lu.%02lu %% of the CPU"), kNames[p],
         lv ? "-20 dB" : "0 dB  ", (unsigned long)mean, (unsigned long)mx, bn, (unsigned long)(mean * 100 / budget),
         (unsigned long)(mean * 10000 / budget % 100));
  }
  b->Reset();
  b->TakePeak();
  // each effect alone (12 frames, knobs 64): what the effect costs without the send mix, fades and limits
  for (int s = 0; s < 2; ++s) c.knob[s][0] = core::fx::kOff;
  {
    uint32_t sum = 0, mx = 0;
    for (int blk = 0; blk < 440; ++blk) {
      __disable_irq();
      const uint32_t t0 = ARM_DWT_CYCCNT;
      b->Process(st, bn, pre, c, post);
      const uint32_t dt = ARM_DWT_CYCCNT - t0;
      __enable_irq();
      if (blk >= 40) sum += dt, mx = dt > mx ? dt : mx;
    }
    logf(PSTR("sendfxbench: both OFF (the frame)  %lu cycles a block (max %lu)"), (unsigned long)(sum / 400), (unsigned long)mx);
  }
  b->Reset();
  b->TakePeak();
  float x[12], y[12][2];
  for (int i = 0; i < 12; ++i) x[i] = 0.1f * (float)(i - 6);
  const uint8_t k[4] = {64, 64, 64, 64};
  auto alone = [&](const char* name, auto& fx) {
    fx.Clear();
    uint32_t sum = 0, mx = 0;
    for (int blk = 0; blk < 440; ++blk) {
      __disable_irq();
      const uint32_t t0 = ARM_DWT_CYCCNT;
      fx.Process(x, 12, y, k, 48000.0f, 0.002f);
      const uint32_t dt = ARM_DWT_CYCCNT - t0;
      __enable_irq();
      if (blk >= 40) sum += dt, mx = dt > mx ? dt : mx;
    }
    logf(PSTR("sendfxbench: %-8s alone        %lu cycles a block (max %lu)"), name, (unsigned long)(sum / 400), (unsigned long)mx);
  };
  {
    core::fx::Chorus ch;
    alone("chorus", ch);
  }
  {
    core::fx::Comb cb;
    alone("comb", cb);
  }
  core::fx::Crush cr;
  alone("crush", cr);
  core::fx::RingMod rm;
  alone("ringmod", rm);
  {
    core::fx::Halo hl;   // 8.3 KB on loop()'s stack, alone in its scope
    alone("halo", hl);
  }
}
#endif

FLASHMEM bool sfx_console(const char* p) {
  if (!strncmp(p, "sendfxbench", 11) && (!p[11] || p[11] == '=')) {
#if RYTM_DEBUG_TOOLS
    int bn = 0;   // no atoi: libc's strtol would land in ITCM
    for (const char* q = p[11] ? p + 12 : "12"; *q >= '0' && *q <= '9'; ++q) bn = bn * 10 + (*q - '0');
    sfx_bench(bn);
#else
    logf(PSTR("sendfxbench: a developer's tool, not in this build (RYTM_DEBUG_TOOLS=0)"));
#endif
    return true;
  }
  char r[200];
  if (!core::fx::command(p, r, sizeof r)) return false;
  logf(PSTR("%s"), r);
  if (!strcmp(p, "sendfx")) {
    __disable_irq();
    const uint64_t cyc = sfx_cyc_sum;
    const uint32_t fr = sfx_frames, mx = sfx_cyc_max, un = sfx_under, ov = sfx_over, calls = sfx_calls, nmax = sfx_n_max;
    sfx_cyc_sum = 0, sfx_frames = 0, sfx_cyc_max = 0, sfx_under = 0, sfx_over = 0, sfx_calls = 0, sfx_n_max = 0;
    __enable_irq();
    const uint32_t now = millis(), span_ms = now - sfx_stat_t0;   // ms: CYCCNT wraps every 7 s
    sfx_stat_t0 = now;
    const uint64_t span = (uint64_t)span_ms * (F_CPU_ACTUAL / 1000);
    const uint32_t per12 = fr ? (uint32_t)(cyc * 12 / fr) : 0, load = span ? (uint32_t)(cyc * 10000 / span) : 0;
    logf(PSTR("sendfx: %lu frames in %lu calls (<= %lu a call), %lu cycles a 12-frame block (a call at most %lu), interrupt load %lu.%02lu %%, underruns %lu, "
              "drops %lu, peak %lu/1000, faults %lu, stream %s"),
         (unsigned long)fr, (unsigned long)calls, (unsigned long)nmax, (unsigned long)per12, (unsigned long)mx, (unsigned long)(load / 100), (unsigned long)(load % 100),
         (unsigned long)un, (unsigned long)ov, (unsigned long)(sfx ? sfx->TakePeak() * 1000.0f : 0),
         (unsigned long)(sfx ? sfx->Faults() : 0), rytm_ob.streaming() ? "on" : "off (no Rytm in Overbridge mode)");
  }
  return true;
}
