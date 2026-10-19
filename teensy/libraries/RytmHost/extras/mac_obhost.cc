// Mac-side Overbridge host test (libusb): the RytmHost driver's sequence against
// a real Rytm in OVERBRIDGE mode (Elektron's Overbridge app closed).
// build: c++ -std=c++17 -O2 -I/opt/homebrew/include -I.. mac_obhost.cc -L/opt/homebrew/lib -lusb-1.0 -o mac_obhost
// usage: mac_obhost SECS [TONE_VOICE FREQ AMP_DB]   TONE_VOICE 0..7 one voice, 8 all
//        (200 250 315 400 500 630 800 1000 Hz by physical voice); OB_REC=file.raw
//        records all 12 return channels (int32, host order); OB_INFLIGHT=N reads in flight
#include <libusb-1.0/libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <initializer_list>
#include "ob_block.h"
static libusb_device_handle *h;
static int tone_voice = -1; static double tone_hz = 220, tone_amp = 0.0, phase = 0, phases[8];
static const double freqs[8] = {200, 250, 315, 400, 500, 630, 800, 1000};
static long in_blocks, out_blocks, in_err, out_err, gaps, bad, short_pk;
static int have_last; static uint16_t last_ctr, out_ctr; static int64_t peak[ob::kChannels];
static uint8_t first_hdr[32]; static int got_first; static FILE *rec;
static volatile int running = 1; static int pending_out;
static void out_cb(libusb_transfer *t) {
  if (t->status != LIBUSB_TRANSFER_COMPLETED) ++out_err; else ++out_blocks;
  --pending_out; free(t->buffer); libusb_free_transfer(t);
}
static void send_block() {
  uint8_t *b = (uint8_t *)malloc(368);
  ob::write_header(b, out_ctr); out_ctr += 7;
  for (int f = 0; f < 7; ++f) {
    int16_t v[8] = {0};
    if (tone_voice == 8) for (int i = 0; i < 8; ++i) { v[i] = (int16_t)(32767 * tone_amp * sin(phases[i])); phases[i] += 2 * M_PI * freqs[i] / 48000.0; }
    else if (tone_voice >= 0) { v[tone_voice] = (int16_t)(32767 * tone_amp * sin(phase)); phase += 2 * M_PI * tone_hz / 48000.0; }
    ob::put_voice_frame(b, 12, f, tone_voice >= 0 ? v : nullptr, ob::kVoiceToRytm, -1);
  }
  libusb_transfer *t = libusb_alloc_transfer(0);
  libusb_fill_interrupt_transfer(t, h, ob::kOutEp, b, 368, out_cb, nullptr, 100);
  if (libusb_submit_transfer(t) == 0) ++pending_out; else { ++out_err; free(b); libusb_free_transfer(t); }
}
static void in_cb(libusb_transfer *t) {
  if (t->status == LIBUSB_TRANSFER_COMPLETED) {
    if (t->actual_length != 368) ++short_pk;
    else {
      const uint8_t *b = t->buffer;
      if (!ob::block_from_device(b)) ++bad;
      else {
        if (!got_first) { memcpy(first_hdr, b, 32); got_first = 1; }
        uint16_t c = ob::block_counter(b);
        if (have_last && (uint16_t)(c - last_ctr) != 7) ++gaps;
        last_ctr = c; have_last = 1;
        for (int f = 0; f < 7; ++f) for (int ch = 0; ch < 12; ++ch) { int64_t s = llabs((int64_t)ob::block_sample(b, 12, f, ch)); if (s > peak[ch]) peak[ch] = s; }
        if (rec) for (int f = 0; f < 7; ++f) for (int ch = 0; ch < 12; ++ch) { int32_t v = ob::block_sample(b, 12, f, ch); fwrite(&v, 4, 1, rec); }
        ++in_blocks;
        if (pending_out < 16) send_block();
      }
    }
  } else ++in_err;
  if (running) libusb_submit_transfer(t); else { free(t->buffer); libusb_free_transfer(t); }
}
int main(int argc, char **argv) {
  double secs = argc > 1 ? atof(argv[1]) : 5;
  if (argc > 4) { tone_voice = atoi(argv[2]); tone_hz = atof(argv[3]); tone_amp = pow(10, atof(argv[4]) / 20); }
  if (getenv("OB_REC")) rec = fopen(getenv("OB_REC"), "wb");
  libusb_context *ctx; libusb_init(&ctx);
  h = libusb_open_device_with_vid_pid(ctx, ob::kVid, ob::kPidRytmMk2);
  if (!h) { printf("open failed\n"); return 1; }
  int r;
  for (int i : {1, 2}) { r = libusb_claim_interface(h, i); printf("claim %d: %s\n", i, libusb_error_name(r)); if (r) return 1; }
  for (int i : {1, 2}) { r = libusb_set_interface_alt_setting(h, i, 3); printf("alt3 %d: %s\n", i, libusb_error_name(r)); if (r) return 1; }
  printf("clear 0x83: %s\n", libusb_error_name(libusb_clear_halt(h, 0x83)));
  printf("clear 0x03: %s\n", libusb_error_name(libusb_clear_halt(h, 0x03)));
  for (int i = 0; i < (getenv("OB_INFLIGHT") ? atoi(getenv("OB_INFLIGHT")) : 4); ++i) { libusb_transfer *t = libusb_alloc_transfer(0); uint8_t *b = (uint8_t *)malloc(368);
    libusb_fill_interrupt_transfer(t, h, ob::kInEp, b, 368, in_cb, nullptr, 0); r = libusb_submit_transfer(t); if (r) printf("submit in: %s\n", libusb_error_name(r)); }
  for (int i = 0; i < 4; ++i) send_block();
  struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); double el = 0; long last_in = 0;
  while (el < secs) {
    struct timeval tv = {0, 100000}; libusb_handle_events_timeout(ctx, &tv);
    clock_gettime(CLOCK_MONOTONIC, &t1); el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if ((long)el != last_in) { last_in = (long)el; printf("t=%2lds in %ld (%.1f Hz frames) out %ld gaps %ld bad %ld short %ld errs in %ld out %ld\n", last_in, in_blocks, in_blocks * 7 / el, out_blocks, gaps, bad, short_pk, in_err, out_err); }
  }
  running = 0;
  printf("header of the first block:"); for (int i = 0; i < 32; ++i) printf(" %02x", first_hdr[i]); printf("\n");
  printf("peaks (dBFS):"); const char *nm[12] = {"MAIN L", "MAIN R", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "IN L", "IN R"};
  for (int c = 0; c < 12; ++c) printf(" %s %.1f", nm[c], peak[c] ? 20 * log10(peak[c] / 2147483648.0) : -999.0); printf("\n");
  struct timeval tv = {0, 300000}; libusb_handle_events_timeout(ctx, &tv);
  for (int i : {1, 2}) { libusb_set_interface_alt_setting(h, i, 0); libusb_release_interface(h, i); }
  if (rec) fclose(rec);
  libusb_close(h); libusb_exit(ctx); return 0;
}
