// Pi-on-Linux stub engine: RT audio clock + jitter histogram, Overbridge host
// (libusb), Rytm USB-MIDI SysEx trig reports. Wire format: teensy/libraries/
// RytmHost/ob_block.h. Protocol background: Overwitch (GPL, read only).
//
//   engine [--period 7|64|128] [--seconds N] [--cpu N] [--prio N] [--no-usb]
#define _GNU_SOURCE
#include <errno.h>
#include <libusb.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define VID 0x1935
#define PID 0x0b48
#define OB_IN_EP 0x83
#define OB_OUT_EP 0x03
#define OB_IFACE_IN 1
#define OB_IFACE_OUT 2
#define OB_ALT 3
#define OB_FRAMES 7
#define OB_CH 12
#define OB_HDR 32
#define OB_BLOCK (OB_HDR + OB_FRAMES * OB_CH * 4) /* 368 */
#define MIDI_IN_EP 0x81

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ---------- RT audio thread ---------- */
static struct {
  int frames, cpu, prio;
  double seconds;
} cfg = {OB_FRAMES, 3, 80, 10};

#define HBINS 2000 /* 1 us bins, last = overflow */
static struct {
  int64_t min, max, sum, n, over_period;
  uint32_t hist[HBINS + 1];
  int sched_ok, mlock_ok, pin_ok;
} lat = {.min = INT64_MAX};
static atomic_uint_fast32_t g_phase_q; /* sine phase, shared with the USB OUT side */

static void render(float *out, int frames, double *ph) {
  for (int i = 0; i < frames; i++) {
    out[2 * i] = out[2 * i + 1] = 0.25f * (float)sin(*ph);
    *ph += 2 * M_PI * 440.0 / RATE;
    if (*ph > 2 * M_PI) *ph -= 2 * M_PI;
  }
}

static void *audio_thread(void *arg) {
  (void)arg;
  int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
  if (cfg.cpu >= 0 && cfg.cpu < ncpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cfg.cpu, &s);
    lat.pin_ok = pthread_setaffinity_np(pthread_self(), sizeof s, &s) == 0;
  }
  struct sched_param sp = {.sched_priority = cfg.prio};
  lat.sched_ok = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
  lat.mlock_ok = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;

  const int64_t period = (int64_t)cfg.frames * 1000000000LL / RATE;
  float buf[256 * 2];
  double ph = 0;
  int64_t next = now_ns() + period, end = next + (int64_t)(cfg.seconds * 1e9);
  while (!g_stop && next < end) {
    struct timespec ts = {next / 1000000000LL, next % 1000000000LL};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    int64_t l = now_ns() - next; /* wake-up latency */
    render(buf, cfg.frames, &ph); /* the "block of work" */
    atomic_store(&g_phase_q, (uint32_t)(ph * 1e6));
    if (l < 0) l = 0;
    if (l < lat.min) lat.min = l;
    if (l > lat.max) lat.max = l;
    lat.sum += l;
    lat.n++;
    if (l > period) lat.over_period++;
    int64_t b = l / 1000;
    lat.hist[b >= HBINS ? HBINS : b]++;
    next += period;
    int64_t t = now_ns();
    if (t > next + 10 * period) next = t + period; /* hopelessly late: resync */
  }
  g_stop = 1;
  return NULL;
}

static int64_t pct_us(double p) {
  int64_t target = (int64_t)(lat.n * p), c = 0;
  for (int i = 0; i <= HBINS; i++)
    if ((c += lat.hist[i]) >= target) return i;
  return HBINS;
}

static void report_latency(void) {
  printf("\n== timing: %d-frame block = %.1f us @ %d Hz, %lld blocks ==\n", cfg.frames,
         cfg.frames * 1e6 / RATE, RATE, (long long)lat.n);
  printf("SCHED_FIFO %d: %s | mlockall: %s | pinned cpu %d: %s\n", cfg.prio, lat.sched_ok ? "ok" : "FAILED",
         lat.mlock_ok ? "ok" : "FAILED", cfg.cpu, lat.pin_ok ? "ok" : "no");
  if (!lat.n) return;
  printf("wake-up latency us: min %.1f avg %.1f max %.1f | p50 %lld p99 %lld p99.9 %lld p99.99 %lld (1 us bins)\n",
         lat.min / 1e3, (double)lat.sum / lat.n / 1e3, lat.max / 1e3, (long long)pct_us(.5),
         (long long)pct_us(.99), (long long)pct_us(.999), (long long)pct_us(.9999));
  printf("late by more than one period (xrun): %lld\n", (long long)lat.over_period);
  puts("histogram (us: count), non-empty bins up to 20 shown + tail:");
  int shown = 0;
  for (int i = 0; i <= HBINS && shown < 20; i++)
    if (lat.hist[i]) printf("  %4d%s: %u\n", i, i == HBINS ? "+" : "", lat.hist[i]), shown++;
}

/* ---------- Overbridge host (libusb) ---------- */
static struct {
  atomic_ullong in_blocks, out_blocks, gaps, gap_frames, bad_hdr, in_err, out_err, underruns;
  int have_dev, midi_iface;
} ob;
static libusb_context *g_ctx;
static libusb_device_handle *g_dev;
static uint16_t g_last_ctr;
static int g_have_ctr;
static uint16_t g_out_ctr;
static double g_out_ph;

#define NXFER 4
static struct libusb_transfer *g_in[NXFER], *g_out[NXFER];
static uint8_t g_inbuf[NXFER][OB_BLOCK], g_outbuf[NXFER][OB_BLOCK];
static atomic_int g_out_busy[NXFER]; /* 1 while the OUT transfer is queued */

static void fill_out(uint8_t *b) {
  memset(b, 0, OB_BLOCK);
  b[0] = 0x07, b[1] = 0xFF, b[2] = g_out_ctr >> 8, b[3] = g_out_ctr & 0xFF;
  g_out_ctr += OB_FRAMES;
  for (int f = 0; f < OB_FRAMES; f++) {
    int32_t s = (int32_t)(0.25 * sin(g_out_ph) * 2147483647.0);
    g_out_ph += 2 * M_PI * 440.0 / RATE;
    uint8_t *fr = b + OB_HDR + 4 * OB_CH * f; /* MAIN L/R = ch 0, 1 */
    for (int c = 0; c < 2; c++)
      fr[4 * c] = s >> 24, fr[4 * c + 1] = s >> 16, fr[4 * c + 2] = s >> 8, fr[4 * c + 3] = s;
  }
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *t) {
  if (t->status == LIBUSB_TRANSFER_COMPLETED) atomic_fetch_add(&ob.out_blocks, 1);
  else if (t->status != LIBUSB_TRANSFER_CANCELLED) atomic_fetch_add(&ob.out_err, 1);
  atomic_store(&g_out_busy[(intptr_t)t->user_data], 0);
}

static void LIBUSB_CALL in_cb(struct libusb_transfer *t) {
  if (t->status == LIBUSB_TRANSFER_CANCELLED || g_stop) return;
  if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length == OB_BLOCK) {
    const uint8_t *b = t->buffer;
    if (b[0] != 0x07 || b[1] != 0x00) atomic_fetch_add(&ob.bad_hdr, 1);
    uint16_t ctr = (uint16_t)(b[2] << 8 | b[3]);
    if (g_have_ctr && (uint16_t)(ctr - g_last_ctr) != OB_FRAMES) {
      atomic_fetch_add(&ob.gaps, 1);
      atomic_fetch_add(&ob.gap_frames, (uint16_t)(ctr - g_last_ctr - OB_FRAMES));
    }
    g_last_ctr = ctr, g_have_ctr = 1;
    atomic_fetch_add(&ob.in_blocks, 1);
    /* the device clocks us: one OUT block per IN block */
    int i = 0;
    while (i < NXFER && atomic_load(&g_out_busy[i])) i++;
    if (i < NXFER) {
      atomic_store(&g_out_busy[i], 1);
      fill_out(g_out[i]->buffer);
      if (libusb_submit_transfer(g_out[i])) atomic_store(&g_out_busy[i], 0), atomic_fetch_add(&ob.out_err, 1);
    } else atomic_fetch_add(&ob.underruns, 1);
  } else {
    atomic_fetch_add(&ob.in_err, 1);
  }
  if (libusb_submit_transfer(t)) atomic_fetch_add(&ob.in_err, 1);
}

/* USB-MIDI (class-compliant 4-byte event packets) -> SysEx -> F0 7D 52 54 cmd ver ... */
static uint8_t sx[1024];
static int sxn;
static atomic_ullong midi_sysex, midi_trig, midi_params;
static uint8_t midi_buf[512];
static struct libusb_transfer *g_midi;

static void sysex_done(void) {
  if (sxn < 6 || sx[0] != 0xF0 || sx[1] != 0x7D || sx[2] != 0x52 || sx[3] != 0x54) return;
  atomic_fetch_add(&midi_sysex, 1);
  uint8_t cmd = sx[4], ver = sx[5];
  if (cmd == 0x04 && sxn >= 9) { /* TRIG+PARAMS: voice note vel ... */
    unsigned long long n = atomic_fetch_add(&midi_trig, 1);
    if (n < 20) printf("[midi] TRIG   v%u voice %u note %u vel %u (%d bytes)\n", ver, sx[6] & 7, sx[7], sx[8], sxn);
  } else if (cmd == 0x03 && sxn >= 7) {
    unsigned long long n = atomic_fetch_add(&midi_params, 1);
    if (n < 20) printf("[midi] PARAMS v%u voice %u (%d bytes)\n", ver, sx[6] & 7, sxn);
  }
}

static void midi_feed(const uint8_t *p, int len) {
  for (int i = 0; i + 4 <= len; i += 4) {
    int cin = p[i] & 0xF, n;
    switch (cin) {
      case 0x4: n = 3; break;            /* sysex start/continue */
      case 0x5: n = 1; break;            /* end, 1 byte */
      case 0x6: n = 2; break;            /* end, 2 bytes */
      case 0x7: n = 3; break;            /* end, 3 bytes */
      default: continue;
    }
    for (int k = 0; k < n; k++) {
      uint8_t c = p[i + 1 + k];
      if (c == 0xF0) sxn = 0;
      if (sxn < (int)sizeof sx) sx[sxn++] = c;
      if (c == 0xF7) { sysex_done(); sxn = 0; }
    }
  }
}

static void LIBUSB_CALL midi_cb(struct libusb_transfer *t) {
  if (t->status == LIBUSB_TRANSFER_CANCELLED || g_stop) return;
  if (t->status == LIBUSB_TRANSFER_COMPLETED) midi_feed(t->buffer, t->actual_length);
  libusb_submit_transfer(t);
}

static int usb_setup(void) {
  if (libusb_init(&g_ctx)) return -1;
  g_dev = libusb_open_device_with_vid_pid(g_ctx, VID, PID);
  if (!g_dev) {
    printf("[usb] no Elektron Analog Rytm MKII (%04x:%04x) found: timing test only\n", VID, PID);
    return -1;
  }
  ob.have_dev = 1;
  libusb_set_auto_detach_kernel_driver(g_dev, 1);
  libusb_set_configuration(g_dev, 1);
  /* find the MIDIStreaming interface (class 1 / subclass 3) */
  struct libusb_config_descriptor *cd;
  ob.midi_iface = -1;
  if (!libusb_get_active_config_descriptor(libusb_get_device(g_dev), &cd)) {
    for (int i = 0; i < cd->bNumInterfaces; i++) {
      const struct libusb_interface_descriptor *d = &cd->interface[i].altsetting[0];
      if (d->bInterfaceClass == 1 && d->bInterfaceSubClass == 3) ob.midi_iface = d->bInterfaceNumber;
    }
    libusb_free_config_descriptor(cd);
  }
  int r;
  if ((r = libusb_claim_interface(g_dev, OB_IFACE_IN)) || (r = libusb_claim_interface(g_dev, OB_IFACE_OUT))) {
    printf("[usb] claim failed: %s (is it in OVERBRIDGE mode?)\n", libusb_error_name(r));
    return -1;
  }
  if ((r = libusb_set_interface_alt_setting(g_dev, OB_IFACE_IN, OB_ALT)) ||
      (r = libusb_set_interface_alt_setting(g_dev, OB_IFACE_OUT, OB_ALT))) {
    printf("[usb] alt setting %d failed: %s\n", OB_ALT, libusb_error_name(r));
    return -1;
  }
  libusb_clear_halt(g_dev, OB_IN_EP);
  libusb_clear_halt(g_dev, OB_OUT_EP);
  printf("[usb] Overbridge: interfaces %d/%d alt %d, EP 0x%02x in / 0x%02x out, %d-byte blocks\n", OB_IFACE_IN,
         OB_IFACE_OUT, OB_ALT, OB_IN_EP, OB_OUT_EP, OB_BLOCK);
  for (int i = 0; i < NXFER; i++) {
    g_in[i] = libusb_alloc_transfer(0);
    libusb_fill_interrupt_transfer(g_in[i], g_dev, OB_IN_EP, g_inbuf[i], OB_BLOCK, in_cb, NULL, 0);
    g_out[i] = libusb_alloc_transfer(0);
    libusb_fill_interrupt_transfer(g_out[i], g_dev, OB_OUT_EP, g_outbuf[i], OB_BLOCK, out_cb, (void *)(intptr_t)i, 1000);
  }
  for (int i = 0; i < NXFER; i++)
    if (libusb_submit_transfer(g_in[i])) printf("[usb] IN submit failed\n");
  if (ob.midi_iface >= 0 && !libusb_claim_interface(g_dev, ob.midi_iface)) {
    g_midi = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(g_midi, g_dev, MIDI_IN_EP, midi_buf, sizeof midi_buf, midi_cb, NULL, 0);
    libusb_submit_transfer(g_midi);
    printf("[usb] MIDI: interface %d, bulk 0x%02x, parsing F0 7D 52 54 reports\n", ob.midi_iface, MIDI_IN_EP);
  } else printf("[usb] MIDI interface not claimed\n");
  return 0;
}

static void *usb_thread(void *arg) {
  (void)arg;
  struct sched_param sp = {.sched_priority = cfg.prio - 10};
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); /* best effort */
  while (!g_stop) {
    struct timeval tv = {0, 100000};
    libusb_handle_events_timeout(g_ctx, &tv);
  }
  return NULL;
}

static void report_usb(void) {
  if (!ob.have_dev) return;
  printf("\n== Overbridge ==\nIN blocks %llu OUT blocks %llu | gaps %llu (%llu frames) | bad hdr %llu | IN err %llu OUT err %llu | OUT underruns %llu\n",
         (unsigned long long)ob.in_blocks, (unsigned long long)ob.out_blocks, (unsigned long long)ob.gaps,
         (unsigned long long)ob.gap_frames, (unsigned long long)ob.bad_hdr, (unsigned long long)ob.in_err,
         (unsigned long long)ob.out_err, (unsigned long long)ob.underruns);
  printf("MIDI: Mods SysEx %llu (TRIG 0x04: %llu, PARAMS 0x03: %llu)\n", (unsigned long long)midi_sysex,
         (unsigned long long)midi_trig, (unsigned long long)midi_params);
}

int main(int argc, char **argv) {
  int no_usb = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--period") && i + 1 < argc) cfg.frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) cfg.seconds = atof(argv[++i]);
    else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cfg.cpu = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--prio") && i + 1 < argc) cfg.prio = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--no-usb")) no_usb = 1;
    else { fprintf(stderr, "usage: %s [--period 7|64|128] [--seconds N] [--cpu N] [--prio N] [--no-usb]\n", argv[0]); return 2; }
  }
  if (cfg.frames < 1 || cfg.frames > 256) return 2;
  signal(SIGINT, on_sig), signal(SIGTERM, on_sig);
  pthread_t at, ut;
  int have_usb = !no_usb && usb_setup() == 0;
  if (have_usb) pthread_create(&ut, NULL, usb_thread, NULL);
  pthread_create(&at, NULL, audio_thread, NULL);
  pthread_join(at, NULL);
  if (have_usb) {
    pthread_join(ut, NULL);
    for (int i = 0; i < NXFER; i++) libusb_cancel_transfer(g_in[i]);
    libusb_release_interface(g_dev, OB_IFACE_IN);
    libusb_release_interface(g_dev, OB_IFACE_OUT);
    libusb_close(g_dev);
  }
  report_latency();
  report_usb();
  return 0;
}
