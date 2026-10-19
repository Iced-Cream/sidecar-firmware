// uac2_probe (T1/T2 of docs/superpowers/specs/2026-10-16-overbridge-passthrough-spike.md): the Teensy 4.1
// enumerates as Serial + USB Audio Class 2, UAC2_CH_IN in / UAC2_CH_OUT out (2/2 default; T2: 12/12,
// 20/12 via RMP_EXTRA_FLAGS="-DUAC2_CH_IN=20 -DUAC2_CH_OUT=12"), 48 kHz, 24-bit, async with explicit feedback.
// Needs the patched core: RMP_CORE=uac2 teensy/run_sketch.sh uac2_probe (teensy/setup_uac2_core.sh).
//
// IN (to the computer), with N = UAC2_CH_IN:
//   2 channels: ch1 a 1 kHz sine at -6 dBFS, ch2 the loopback of OUT ch1 (the T1 layout).
//   more:       ch1..N-2 sines at -12 dBFS of distinct frequencies (tone_period[]: whole periods of 48 kHz, so
//               every sample can be checked on the computer), ch N-1 the loopback of OUT ch1, ch N of OUT ch M.
//   teensy/uac2/uac2_test.py checks all of it.
// OUT (from the computer), checked here per channel: positive zero crossings per second, the largest second
// difference, and discontinuities: a second difference above twice what a sine of last second's peak and
// frequency can make (+20000), with a 48-frame holdoff.
// Serial, once a second: SOFs, microframes, feedback, packet and frame counts, OUT ring level, underruns,
// overruns; then per OUT channel zero crossings, peak, largest second difference and discontinuities.
// Debug commands: "f<ppm>" adds ppm to the feedback, "c<ppm>" runs the device clock ppm fast; 0 clears.
#if !defined(USB_SERIAL_UAC2)
#error "build with the uac2 core: RMP_CORE=uac2 teensy/run_sketch.sh uac2_probe"
#endif
#include <math.h>

#define NI UAC2_CH_IN
#define NO UAC2_CH_OUT
#define NTONE (NI > 2 ? NI - 2 : 1)
// periods (frames) dividing 48000: 100, 120, 150, 160, 200, 240, 250, 300, 320, 375, 400, 480, 500, 600, 640,
// 750, 800, 960 Hz (uac2_test.py has the same list)
static const uint16_t tone_period[18] = {480, 400, 320, 300, 240, 200, 192, 160, 150, 128, 120, 100, 96, 80, 75, 64, 60, 50};
#if NTONE > 18
#error "uac2_probe: at most 18 tone channels (20 IN)"
#endif
#define TABLE_FRAMES 4096
DMAMEM static int32_t table[TABLE_FRAMES];
static uint16_t tab_off[NTONE], tab_len[NTONE], tab_pos[NTONE];

static volatile int32_t peak[NO];
static int32_t x1[NO], x2[NO];
static uint8_t have[NO];
static volatile uint32_t zc[NO], gl[NO], d2max[NO], last_gl[NO], thr[NO];
static uint32_t frames_seen;

// called from the USB interrupt for every IN packet
extern "C" FLASHMEM void usb_uac2_in_fill(int32_t *dst, unsigned frames)
{
  int32_t out[(UAC2_FRAMES_480 + 1) * NO];
  if (frames > UAC2_FRAMES_480 + 1) frames = UAC2_FRAMES_480 + 1;   // high speed only
  unsigned got = usb_uac2_out_read(out, frames);
  if (got < frames)
    for (int c = 0; c < NO; c++) have[c] = 0;
  for (unsigned f = 0; f < frames; f++) {
    const int32_t *o = out + f * NO;
    if (f < got) {
      frames_seen++;
      for (int c = 0; c < NO; c++) {
        int32_t x = o[c];
        int32_t a = x < 0 ? -x : x;
        if (a > peak[c]) peak[c] = a;
        if (have[c] >= 2) {
          int32_t d2 = x - 2 * x1[c] + x2[c];
          if (d2 < 0) d2 = -d2;
          if ((uint32_t)d2 > d2max[c]) d2max[c] = d2;
          if ((uint32_t)d2 > thr[c] && frames_seen - last_gl[c] > 48) { gl[c]++; last_gl[c] = frames_seen; }
          if (x1[c] < 0 && x >= 0) zc[c]++;
        } else have[c]++;
        x2[c] = x1[c]; x1[c] = x;
      }
    }
    int32_t *d = dst + f * NI;
#if NI == 2
    d[0] = table[tab_pos[0]];
    if (++tab_pos[0] == tab_len[0]) tab_pos[0] = 0;
    d[1] = o[0];
#else
    for (int t = 0; t < NTONE; t++) {
      d[t] = table[tab_off[t] + tab_pos[t]];
      if (++tab_pos[t] == tab_len[t]) tab_pos[t] = 0;
    }
    d[NI - 2] = o[0];
    d[NI - 1] = o[NO - 1];
#endif
  }
}

FLASHMEM void setup()
{
  uint32_t off = 0;
  for (int t = 0; t < NTONE; t++) {
    uint32_t p = NI == 2 ? 48 : tone_period[t];
    double amp = NI == 2 ? 0.5 : 0.25;   // double: the same integers as Python's round(amp * FULL * sin(...))
    tab_off[t] = off; tab_len[t] = p;
    for (uint32_t i = 0; i < p; i++) table[off + i] = (int32_t)lround(amp * 8388607.0 * sin(2.0 * M_PI * i / p));
    off += p;
  }
  for (int c = 0; c < NO; c++) thr[c] = 0xFFFFFFFF;
  Serial.begin(115200);
}

static struct usb_uac2_stats prev;

FLASHMEM static int db(int32_t v) { return v > 0 ? (int)lrintf(20.0f * log10f(v / 8388607.0f)) : -999; }

FLASHMEM static void commands()
{
  static char cmd[16];
  static uint8_t cl;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      cmd[cl] = 0;
      if (cl && cmd[0] == 'f') {
        long ppm = atol(cmd + 1);
        usb_uac2_fb_offset = (int32_t)((int64_t)ppm * (6 << 16) / 1000000);
        Serial.printf("feedback offset %ld ppm = %ld (16.16)\n", ppm, (long)usb_uac2_fb_offset);
      } else if (cl && cmd[0] == 'c') {
        usb_uac2_clock_ppm = atol(cmd + 1);
        Serial.printf("device clock offset %ld ppm\n", (long)usb_uac2_clock_ppm);
      }
      cl = 0;
    } else if (cl < sizeof(cmd) - 1) cmd[cl++] = ch;
  }
}

FLASHMEM void loop()
{
  static uint32_t t0 = millis();
  static uint32_t n;
  commands();
  if (millis() - t0 < 1000) return;
  t0 += 1000;
  struct usb_uac2_stats s;
  uint32_t z[NO], g[NO], m[NO];
  int32_t pk[NO];
  __disable_irq();
  memcpy(&s, (const void *)&usb_uac2_stats, sizeof(s));
  for (int c = 0; c < NO; c++) {
    z[c] = zc[c]; g[c] = gl[c]; m[c] = d2max[c]; pk[c] = peak[c];
    zc[c] = 0; d2max[c] = 0; peak[c] = 0;
  }
  __enable_irq();
  // next second's discontinuity threshold from this second's sine: peak x (2 pi f / fs)^2, doubled, + 20000,
  // only from a steady second: the same zero-crossing count as the second before (a stream that starts or stops
  // mid-second gives a partial count, and a too-low threshold)
  static uint32_t zprev[NO];
  for (int c = 0; c < NO; c++) {
    uint32_t zp = zprev[c];
    zprev[c] = z[c];
    if (z[c] == 0 || pk[c] == 0 || zp == 0 || (z[c] > zp ? z[c] - zp : zp - z[c]) > z[c] / 100 + 1) { thr[c] = 0xFFFFFFFF; continue; }
    float w = 2.0f * (float)M_PI * z[c] / 48000.0f;
    thr[c] = (uint32_t)(2.0f * pk[c] * w * w) + 20000;
  }
  usb_uac2_stats_reset_minmax();
  Serial.printf("t=%lu hs=%u ch %u/%u alt in/out=%u/%u sof=%lu (+%lu) uf=+%lu fb=0x%08lx (%.3f Hz) fbtx=+%lu base=0x%08lx "
                "in pk=+%lu fr=+%lu clamp=%lu resync=%lu | out pk=+%lu fr=+%lu h=%lu/%lu/%lu bad=%lu err=%lu "
                "lvl=%lu [%lu..%lu] under=%lu over=%lu sink=%lu | req g%lu s%lu bad%lu\n",
                ++n, s.high_speed, NI, NO, s.in_alt, s.out_alt, s.sof, s.sof - prev.sof, s.uframes - prev.uframes,
                s.fb, s.fb * 8000.0 / 65536.0, s.fb_sent - prev.fb_sent, s.fb_base,
                s.in_pkts - prev.in_pkts, s.in_frames - prev.in_frames, s.in_clamp, s.in_resync,
                s.out_pkts - prev.out_pkts, s.out_frames - prev.out_frames, s.out_hist[0], s.out_hist[1], s.out_hist[2],
                s.out_badlen, s.out_err, s.out_level, s.out_level_min == 0xFFFFFFFF ? 0 : s.out_level_min,
                s.out_level_max, s.out_underrun, s.out_overrun, s.sink_frames, s.get_req, s.set_cur, s.bad_req);
  Serial.printf("  out zc");
  for (int c = 0; c < NO; c++) Serial.printf(" %lu", z[c]);
  Serial.printf(" | dB");
  for (int c = 0; c < NO; c++) Serial.printf(" %d", db(pk[c]));
  Serial.printf(" | d2max");
  for (int c = 0; c < NO; c++) Serial.printf(" %lu", m[c]);
  Serial.printf(" | gl");
  for (int c = 0; c < NO; c++) Serial.printf(" %lu", g[c]);
  Serial.printf("\n");
  prev = s;
}
