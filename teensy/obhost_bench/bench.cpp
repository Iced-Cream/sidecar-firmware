// obhost_bench: the CPU cost of the Overbridge host data path (RytmHost on
// USBHost_t36) on the Teensy 4.1, without a Rytm and without the USB host
// port (usb.begin() is never called: the EHCI controller and the host port
// stay off).
//
// What runs is the real code: RytmOverbridge's in_done / pump_out / fill_out /
// out_done (libraries/RytmHost, unchanged) and USBHost_t36's own
// queue_Data_Transfer / queue_Transfer / followup_Transfer / free_Transfer,
// called from an interrupt on the USB host's own vector (IRQ_USBHS) at its
// default priority. What is simulated:
//   - the EHCI controller: a PIT at 64 (above USBHS) plays the hardware. It
//     marks the oldest active IN qTD complete (2 blocks of 368 bytes, the
//     header counter +7 a block: the DMA's work, not counted) and, half a
//     period later, the oldest active OUT qTD, then pends IRQ_USBHS. The rate is
//     the Rytm's: 48000 / 7 blocks a second each way, 2 blocks a transfer
//     (RYTM_OB_BLOCKS_PER_XFER) = 3428.6 IN + 3428.6 OUT completions a second.
//   - USBHost::isr(): the handler reads and clears USBHS_USBSTS (a real
//     register access) and runs isr()'s periodic-followup loop line for line
//     (the real followup_Transfer and free_Transfer; remove_from_periodic_
//     followup_list is file-static in ehci.cpp, so it is repeated here on the
//     library's own list heads, found in RAM at start).
//   - the device: a Device_t with no bus behind it; the two interrupt pipes are
//     set up as new_Pipe() does, minus the periodic schedule (the controller is
//     off).
// Not covered: the controller's DMA and bus arbitration, NAKs, errors, the
// USB MIDI interface's bulk traffic, Task().
//
// Phases (about 3 s each), printed over USB serial; 'r' runs them again:
//   0 calibration: nothing but the headroom probe (busy units in loop()).
//   1 scaffold: the PIT and an empty IRQ_USBHS handler at the same rate - the
//     simulation's own cost, and an upper bound for the interrupt entries.
//   2 host, separate IN and OUT interrupts (6857 a second), loop() feeding the
//     FIFO with ready frames (pushVoices) as voiceloop's host_feed does.
//   3 host, IN and OUT completing in the same interrupt (3429 a second).
//   4 N Plaits voices, no host (a timed consumer drains 48000 frames a second).
//   5 host (as 2) + N Plaits voices rendered on demand into the FIFO.
//   6 as 5 with N - 1 voices.
// Headroom = busy units done in loop() / the calibration's units.
//
// Build and run: teensy/run_sketch.sh obhost_bench 50 (results: docs/HOST-BRINGUP.md)
#include <Arduino.h>
#include <new>

#include <Plaits.h>

// The bench reaches into RytmOverbridge and USBHost to drive their private
// paths (layout unchanged: only access).
#define private public
#define protected public
#include <USBHost_t36.h>
#include <RytmHost.h>
#undef private
#undef protected

#ifndef BENCH_VOICES
#define BENCH_VOICES 8
#endif
#ifndef BENCH_ENGINE
#define BENCH_ENGINE 20   // plaits_bench's numbering: 20 = MODAL, the costliest model
#endif
#ifndef BENCH_TARGET
#define BENCH_TARGET 256  // FIFO frames kept queued (voiceloop's RYTM_HOST_TARGET)
#endif
#ifndef BENCH_PHASE_MS
#define BENCH_PHASE_MS 3000
#endif

USBHost usb;   // never begun
RytmOverbridge rob(usb);
Device_t fake_dev;

// ---- the voices (Plaits, as plaits_bench / voiceloop) ----
const int kBlock = plaits::kBlockSize;   // 12
plaits::Voice voices[BENCH_VOICES];
DMAMEM alignas(32) char shared[BENCH_VOICES][16384];   // RAM2, as voiceloop
plaits::Patch patches[BENCH_VOICES];
plaits::Modulations mods[BENCH_VOICES];
plaits::Voice::Frame vframes[kBlock];
int16_t block_frames[kBlock][ob::kVoices];
uint32_t render_count = 0;

FLASHMEM void voices_init() {
  for (int v = 0; v < BENCH_VOICES; ++v) {
    stmlib::BufferAllocator allocator(shared[v], sizeof shared[v]);
    voices[v].Init(&allocator);
    plaits::Patch& p = patches[v];
    p.engine = BENCH_ENGINE;
    p.note = 36.0f + 5.0f * v;
    p.harmonics = p.timbre = p.morph = 0.5f;
    p.frequency_modulation_amount = p.timbre_modulation_amount = p.morph_modulation_amount = 0.0f;
    p.decay = 0.5f;
    p.lpg_colour = 0.5f;
    mods[v].trigger_patched = true;
    mods[v].level_patched = false;
  }
}

// One 12-frame block of N voices into block_frames (int16, voice order).
void render_voices(int n) {
  for (int v = 0; v < n; ++v) {
    plaits::Patch& p = patches[v];
    mods[v].trigger = ((render_count + 125 * v) % 1000 == 0) ? 1.0f : 0.0f;   // each voice every 0.25 s
    p.timbre = 0.5f + 0.4f * sinf(render_count * 0.001f + v);
    p.morph = 0.5f + 0.4f * cosf(render_count * 0.0013f + v);
    voices[v].Render(p, mods[v], vframes, kBlock);
    for (int f = 0; f < kBlock; ++f) block_frames[f][v] = vframes[f].out;
  }
  for (int v = n; v < ob::kVoices; ++v)
    for (int f = 0; f < kBlock; ++f) block_frames[f][v] = 0;
  ++render_count;
}

// ---- the simulated controller ----
Transfer_t **pf_first = nullptr, **pf_last = nullptr;   // ehci.cpp's periodic_followup_first / _last
Transfer_t *in_cur = nullptr, *out_cur = nullptr;      // the qTD the "controller" works on, per pipe
volatile int sim_mode = 0;          // 0 off, 1 scaffold (empty handler), 2 separate, 3 combined
volatile uint8_t phase_half = 0;    // separate mode: IN on even ticks, OUT on odd ones
volatile uint8_t done_kind = 0;     // 1 IN, 2 OUT, 3 both, this interrupt
uint16_t dev_counter = 0;

struct IsrStat {
  uint32_t n, max;
  uint64_t sum;
};
volatile IsrStat st_in, st_out, st_both;
volatile uint32_t hw_in = 0, hw_out = 0, hw_nak_in = 0, hw_nak_out = 0;

// The oldest active qTD of a pipe complete, all bytes moved, no error (what the
// EHCI writes back). False: none active (the controller would NAK / idle).
bool complete_qtd(Pipe_t* pipe, Transfer_t*& cur, bool in) {
  if (!cur || !(cur->qtd.token & 0x80)) return false;   // the halt qTD (0x40): nothing queued
  if (in) {   // the DMA's work: the Rytm's header and counter, 2 blocks (samples left as they are)
    uint8_t* b = (uint8_t*)cur->buffer;
    for (int i = 0; i < RytmOverbridge::kBlocks; ++i, b += ob::block_bytes(ob::kChannels)) {
      b[0] = 0x07;
      b[1] = 0x00;
      b[2] = dev_counter >> 8;
      b[3] = dev_counter & 0xFF;
      dev_counter += ob::kFramesPerBlock;
    }
  }
  Transfer_t* next = (Transfer_t*)cur->qtd.next;
  cur->qtd.token = cur->qtd.token & 0x8000FF00 & ~0x80u;   // toggle, IOC, PID kept; 0 bytes left; status 0
  pipe->qh.current = (uint32_t)cur;   // the queue head's overlay moves on (queue_Transfer walks from qh.next)
  pipe->qh.next = (uint32_t)next;
  cur = next;
  return true;
}

void hw_tick() {   // PIT, priority 64
  const int m = sim_mode;
  if (m == 1) {
    NVIC_SET_PENDING(IRQ_USBHS);
    return;
  }
  uint8_t k = 0;
  if (m == 2) {
    phase_half ^= 1;
    if (phase_half) {
      if (complete_qtd(rob.in_pipe_, in_cur, true)) { k = 1; hw_in++; } else hw_nak_in++;
    } else {
      if (complete_qtd(rob.out_pipe_, out_cur, false)) { k = 2; hw_out++; } else hw_nak_out++;
    }
  } else if (m == 3) {
    if (complete_qtd(rob.in_pipe_, in_cur, true)) { k |= 1; hw_in++; } else hw_nak_in++;
    if (complete_qtd(rob.out_pipe_, out_cur, false)) { k |= 2; hw_out++; } else hw_nak_out++;
  }
  if (k) {
    done_kind = k;
    NVIC_SET_PENDING(IRQ_USBHS);
  }
}

// ehci.cpp's remove_from_periodic_followup_list, on its own list heads.
static void remove_periodic(Transfer_t* t) {
  Transfer_t *next = t->next_followup, *prev = t->prev_followup;
  if (prev) prev->next_followup = next;
  else *pf_first = next;
  if (next) next->prev_followup = prev;
  else *pf_last = prev;
}

// IRQ_USBHS: USBHost::isr()'s USBSTS handling and its UPI branch.
void sim_usb_isr() {
  if (sim_mode == 1) return;   // scaffold: the entry and exit only
  const uint32_t t0 = ARM_DWT_CYCCNT;
  uint32_t stat = USBHS_USBSTS;
  USBHS_USBSTS = stat;
  (void)stat;   // the controller would have set UPI
  Transfer_t* p = *pf_first;
  while (p) {
    if (USBHost::followup_Transfer(p)) {
      Transfer_t* next = p->next_followup;
      remove_periodic(p);
      USBHost::free_Transfer(p);
      p = next;
    } else {
      p = p->next_followup;
    }
  }
  const uint32_t dt = ARM_DWT_CYCCNT - t0;
  volatile IsrStat& s = done_kind == 1 ? st_in : done_kind == 2 ? st_out : st_both;
  s.n++;
  s.sum += dt;
  if (dt > s.max) s.max = dt;
}

IntervalTimer hw;

// A pipe as new_Pipe() sets one up, without the periodic schedule.
FLASHMEM Pipe_t* make_pipe(uint32_t ep, uint32_t dir, void (*cb)(const Transfer_t*)) {
  Pipe_t* pipe = USBHost::allocate_Pipe();
  Transfer_t* halt = USBHost::allocate_Transfer();
  if (!pipe || !halt) return nullptr;
  memset(pipe, 0, sizeof(Pipe_t));
  memset(halt, 0, sizeof(Transfer_t));
  halt->qtd.next = 1;
  halt->qtd.token = 0x40;
  pipe->device = &fake_dev;
  pipe->qh.next = (uint32_t)halt;
  pipe->qh.alt_next = 1;
  pipe->direction = dir;
  pipe->type = 3;
  pipe->periodic_interval = 1;
  pipe->callback_function = cb;
  (void)ep;
  return pipe;
}

extern unsigned long _sbss, _ebss;

FLASHMEM bool host_init() {
  // the Rytm's interfaces 1 and 2 at alt 3, as the real descriptor has them (ob_block.h)
  static const uint8_t desc[] = {
      9, 4, 1, 0, 0, 0xFF, 0, 0, 0,  9, 4, 1, 3, 1, 0xFF, 0, 0, 0,  7, 5, 0x83, 3, 0x70, 0x01, 1,
      9, 4, 2, 0, 0, 0xFF, 0, 0, 0,  9, 4, 2, 3, 1, 0xFF, 0, 0, 0,  7, 5, 0x03, 3, 0x70, 0x01, 1};
  if (!ob::find_layout(desc, sizeof desc, ob::kAlt, &rob.layout_)) return false;
  memset(&fake_dev, 0, sizeof fake_dev);
  fake_dev.speed = 2;
  fake_dev.address = 1;
  rob.in_block_ = ob::block_bytes(rob.layout_.in_channels);
  rob.out_block_ = ob::block_bytes(rob.layout_.out_channels);
  rob.in_pipe_ = make_pipe(3, 1, RytmOverbridge::in_cb);
  rob.out_pipe_ = make_pipe(3, 0, RytmOverbridge::out_cb);
  if (!rob.in_pipe_ || !rob.out_pipe_) return false;
  rob.device = &fake_dev;
  rob.st_.fifo_min = 1 << 30;
  in_cur = (Transfer_t*)rob.in_pipe_->qh.next;
  out_cur = (Transfer_t*)rob.out_pipe_->qh.next;
  Transfer_t* head = in_cur;
  // the IN samples: a ramp (the stream's data does not change the cost)
  for (int k = 0; k < RytmOverbridge::kXfers; ++k)
    for (int i = 0; i < (int)sizeof rob.in_buf_[k]; ++i) rob.in_buf_[k][i] = (uint8_t)(i * 7 + k);
  rob.start_stream();   // the real start: kXfers IN transfers queued
  if (rob.in_inflight_ != RytmOverbridge::kXfers) return false;
  Transfer_t* tail = head;
  while (tail->next_followup) tail = tail->next_followup;
  // ehci.cpp's list heads now hold head and tail: find them in .bss (outside our objects)
  const uint32_t lo = (uint32_t)&rob, hi = lo + sizeof rob;
  int found = 0;
  for (uint32_t* a = (uint32_t*)&_sbss; a + 1 < (uint32_t*)&_ebss; ++a) {
    if ((uint32_t)a >= lo && (uint32_t)a < hi) continue;
    if (*a != (uint32_t)head) continue;
    for (int d = -4; d <= 4; ++d)
      if (d && a[d] == (uint32_t)tail) {
        pf_first = (Transfer_t**)a;
        pf_last = (Transfer_t**)(a + d);
        ++found;
      }
  }
  Serial.printf("followup list heads: %d match%s at %08lx / %08lx\n", found, found == 1 ? "" : "es",
                (unsigned long)pf_first, (unsigned long)pf_last);
  return found == 1;
}

// ---- the headroom probe ----
volatile uint32_t sink;
__attribute__((noinline)) void busy_unit() {   // about 1 us, ITCM code, register work
  uint32_t x = sink;
  for (int i = 0; i < 150; ++i) x = x * 1664525u + 1013904223u;
  sink = x;
}

struct Result {
  uint32_t units, frames, xrun_frames, in_blocks, out_blocks, gaps, bad, fails;
  float secs;
  IsrStat in, out, both;
  uint32_t hw_in, hw_out;
};

void reset_isr_stats() {
  __disable_irq();
  st_in.n = st_in.max = 0; st_in.sum = 0;
  st_out.n = st_out.max = 0; st_out.sum = 0;
  st_both.n = st_both.max = 0; st_both.sum = 0;
  hw_in = hw_out = hw_nak_in = hw_nak_out = 0;
  __enable_irq();
}

// mode: the simulated controller; nv: voices rendered; host: FIFO drained by the host path
// (else, with voices, a timed consumer).
FLASHMEM Result run_phase(int mode, int nv, bool host) {
  Result r;
  memset(&r, 0, sizeof r);
  RytmOverbridge::Stats s0, s1;
  rob.stats(&s0, true);
  reset_isr_stats();
  sim_mode = mode;
  phase_half = 0;
  const uint32_t ms0 = millis();
  const uint32_t c0 = ARM_DWT_CYCCNT;
  // timed consumer (no host): a 12-frame block due every 150000 cycles (48 kHz), a 32-bit compare a pass
  const uint32_t kBlockCycles = (uint32_t)((uint64_t)F_CPU_ACTUAL * kBlock / 48000);
  uint32_t next_due = c0 + kBlockCycles;
  int level = BENCH_TARGET / kBlock;   // blocks queued
  while (millis() - ms0 < BENCH_PHASE_MS) {
    if (host) {
      int need = BENCH_TARGET - rob.fifoLevel();
      if (need >= kBlock) {   // host_feed: whole 12-frame blocks
        if (nv) render_voices(nv);
        int took = rob.pushVoices(block_frames, kBlock);
        r.frames += took;
        continue;
      }
    } else if (nv) {
      while ((int32_t)(ARM_DWT_CYCCNT - next_due) >= 0) {
        next_due += kBlockCycles;
        if (level > 0) --level;
        else r.xrun_frames += kBlock;
      }
      if (level < BENCH_TARGET / kBlock) {
        render_voices(nv);
        ++level;
        r.frames += kBlock;
        continue;
      }
    }
    busy_unit();
    r.units++;
  }
  const uint32_t cyc = ARM_DWT_CYCCNT - c0;
  sim_mode = 0;
  delayMicroseconds(500);   // let the last completions run
  r.secs = (float)cyc / F_CPU_ACTUAL;
  rob.stats(&s1, true);
  __disable_irq();
  r.in.n = st_in.n; r.in.max = st_in.max; r.in.sum = st_in.sum;
  r.out.n = st_out.n; r.out.max = st_out.max; r.out.sum = st_out.sum;
  r.both.n = st_both.n; r.both.max = st_both.max; r.both.sum = st_both.sum;
  r.hw_in = hw_in; r.hw_out = hw_out;
  __enable_irq();
  r.in_blocks = s1.in_blocks - s0.in_blocks;
  r.out_blocks = s1.out_blocks - s0.out_blocks;
  if (host) r.xrun_frames = s1.xrun_frames - s0.xrun_frames;
  r.gaps = s1.counter_gaps - s0.counter_gaps;
  r.bad = s1.bad_blocks - s0.bad_blocks;
  r.fails = s1.queue_fails - s0.queue_fails;
  return r;
}

uint32_t units0 = 0;   // calibration, units a second

FLASHMEM void print_isr(const char* name, const IsrStat& s, float secs) {
  if (s.n < 10) return;   // a stray one at a mode switch
  const float avg = (float)s.sum / s.n;
  Serial.printf("    %-5s %6lu irq/s  avg %5.0f cyc (%.2f us)  max %5lu cyc (%.2f us)  = %.2f %% CPU\n", name,
                (unsigned long)(s.n / secs), avg, avg / 600.0f, (unsigned long)s.max, s.max / 600.0f,
                100.0f * (float)s.sum / (secs * F_CPU_ACTUAL));
}

FLASHMEM void report(const char* name, const Result& r) {
  const float ups = r.units / r.secs;
  Serial.printf("%s\n  headroom %.2f %% (%lu units/s of %lu), CPU used %.2f %%\n", name, 100.0f * ups / units0,
                (unsigned long)ups, (unsigned long)units0, 100.0f - 100.0f * ups / units0);
  if (r.in_blocks || r.out_blocks)
    Serial.printf("  blocks in %.1f/s out %.1f/s (%.1f Hz frames), xrun frames %lu, gaps %lu, bad %lu, queue fails %lu\n",
                  r.in_blocks / r.secs, r.out_blocks / r.secs, 7.0f * r.in_blocks / r.secs, (unsigned long)r.xrun_frames,
                  (unsigned long)r.gaps, (unsigned long)r.bad, (unsigned long)r.fails);
  else if (r.frames)
    Serial.printf("  frames %.1f/s, xrun frames %lu\n", r.frames / r.secs, (unsigned long)r.xrun_frames);
  print_isr("IN", r.in, r.secs);
  print_isr("OUT", r.out, r.secs);
  print_isr("IN+OUT", r.both, r.secs);
  const uint32_t nb = r.in_blocks + r.out_blocks;
  if (nb)
    Serial.printf("    per block (either way): %.0f cycles in the handler\n",
                  (float)(r.in.sum + r.out.sum + r.both.sum) / nb);
}

FLASHMEM void run_all() {
  Serial.printf("\nobhost_bench @ %lu MHz: %d blocks/xfer, %d xfers, FIFO %d (target %d), %d voices of Plaits engine %d\n",
                (unsigned long)(F_CPU_ACTUAL / 1000000), RytmOverbridge::kBlocks, RytmOverbridge::kXfers,
                RytmOverbridge::kFifo, BENCH_TARGET, BENCH_VOICES, BENCH_ENGINE);
  {   // fill_out alone (an OUT transfer: 2 blocks from the FIFO, BE int32, header): direct calls
    static uint8_t scratch[RytmOverbridge::kBlocks * 368] __attribute__((aligned(32)));
    uint64_t sum = 0;
    uint32_t mx = 0;
    for (int i = 0; i < 200; ++i) {
      rob.pushVoices(block_frames, kBlock);
      rob.pushVoices(block_frames, 2);
      uint32_t t0 = ARM_DWT_CYCCNT;
      rob.fill_out(scratch);
      uint32_t dt = ARM_DWT_CYCCNT - t0;
      sum += dt;
      if (dt > mx) mx = dt;
    }
    rob.fifo_rd_ = rob.fifo_wr_;
    Serial.printf("fill_out (one OUT transfer, %d blocks): avg %lu max %lu cycles\n", RytmOverbridge::kBlocks,
                  (unsigned long)(sum / 200), (unsigned long)mx);
  }
  Result r0 = run_phase(0, 0, false);
  units0 = r0.units / r0.secs;
  Serial.printf("0 calibration: %lu busy units/s\n", (unsigned long)units0);
  Result r1;
  hw.begin(hw_tick, 1e6f * 7 / 48000);   // the separate-mode tick: 145.83 us (IN, then OUT, each every 291.67 us)
  r1 = run_phase(1, 0, false);
  report("1 scaffold: PIT 6857/s + empty IRQ_USBHS handler", r1);
  Result r2 = run_phase(2, 0, true);
  report("2 host path, IN and OUT in separate interrupts", r2);
  hw.update(1e6f * 14 / 48000);   // combined: 291.67 us
  delay(2);
  Result r1c = run_phase(1, 0, false);
  report("1c scaffold: PIT 3429/s + empty handler", r1c);
  Result r3 = run_phase(3, 0, true);
  report("3 host path, IN and OUT in the same interrupt", r3);
  hw.update(1e6f * 7 / 48000);
  delay(2);
  sim_mode = 0;
  Result r4 = run_phase(0, BENCH_VOICES, false);
  report("4 voices only (timed consumer, no host)", r4);
  Result r5 = run_phase(2, BENCH_VOICES, true);
  report("5 host path (separate) + voices", r5);
  Result r6 = run_phase(2, BENCH_VOICES - 1, true);
  Serial.printf("(%d voices) ", BENCH_VOICES - 1);
  report("6 host path (separate) + one voice fewer", r6);
  hw.end();
  const float h2 = 100.0f - 100.0f * (r2.units / r2.secs) / units0;
  const float h1 = 100.0f - 100.0f * (r1.units / r1.secs) / units0;
  const float h4 = 100.0f - 100.0f * (r4.units / r4.secs) / units0;
  const float h5 = 100.0f - 100.0f * (r5.units / r5.secs) / units0;
  Serial.printf("summary: host %.2f %% (%.2f %% less the scaffold), %d voices %.2f %%, both %.2f %%, headroom %.2f %%\n", h2,
                h2 - h1, BENCH_VOICES, h4, h5, 100.0f - h5);
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 8000) {}
  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
  if (CrashReport) Serial.print(CrashReport);
  voices_init();
  if (!host_init()) {
    Serial.println("host_init failed");
    return;
  }
  attachInterruptVector(IRQ_USBHS, sim_usb_isr);   // USBHost::begin()'s vector, default priority
  NVIC_ENABLE_IRQ(IRQ_USBHS);
  hw.priority(64);
  run_all();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') run_all();
}
