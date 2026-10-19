// rytm-teensy: Overbridge host bring-up (docs/HOST-BRINGUP.md). The Teensy 4.1
// as USB host of the Analog Rytm MKII on its 5-pin host header; the micro-USB
// to the Mac for power and this serial log (115200, or teensy/serial_read.py).
//
// Step by step, nothing automatic beyond enumeration: the Overbridge stream
// starts on 'g', the test tone on 't'.
//
// Serial commands (one character each):
//   d   the descriptors (parsed, then hex)        h   the first IN block's header
//   g   start the Overbridge stream               o   OUT stream on/off
//   t   test tone on/off (440 Hz, -12 dB) on voice V's input, and on MAIN L/R
//   v   next voice for the tone (0..7)            m   tone on MAIN L/R too on/off
//   b   a BD trig over USB MIDI (note 0, channel 14: the auto channel)
//   q   MIDI messages printed on/off (SysEx always)   s   stats now
//   R   reboot
#include <USBHost_t36.h>
#include <RytmHost.h>

USBHost usb;
USBHub hub1(usb);
MIDIDevice_BigBuffer midi(usb);
RytmOverbridge rytm(usb);

static const int kTarget = 128;   // FIFO frames kept queued (2.7 ms)
static int16_t sine[256];
static bool tone_on = false, tone_main = true, print_midi = true;
static int tone_voice = 0;
static uint32_t phase = 0;
static const uint32_t kStep = (uint32_t)(440.0 / 48000.0 * 4294967296.0);
static uint32_t clocks = 0, sysex_in = 0, last_stats = 0;
static RytmOverbridge::State said = RytmOverbridge::kIdle;
static bool midi_was = false;
static uint16_t said_cfg = 0;

static void log_line(const char* s) { Serial.println(s); }

static void print_descriptors() {
  uint16_t n;
  const uint8_t* p = rytm.config(&n);
  if (!n) {
    Serial.println("no Elektron device enumerated yet");
    return;
  }
  Serial.printf("device %04X:%04X, %s speed; configuration (%u bytes after its header):\n", rytm.vid(), rytm.pid(),
                rytm.speed() == 2 ? "high (480 Mbit)" : rytm.speed() == 0 ? "FULL (12 Mbit)" : "low", n);
  const uint8_t* end = p + n;
  for (const uint8_t* q = p; q + 2 <= end && q[0] >= 2 && q + q[0] <= end; q += q[0]) {
    if (q[1] == 4)
      Serial.printf("  interface %u alt %u: %u endpoints, class %02X/%02X/%02X\n", q[2], q[3], q[4], q[5], q[6], q[7]);
    else if (q[1] == 5)
      Serial.printf("    endpoint %02X %s, %u bytes, bInterval %u\n", q[2],
                    (q[3] & 3) == 3 ? "interrupt" : (q[3] & 3) == 2 ? "bulk" : (q[3] & 3) == 1 ? "isochronous" : "control",
                    q[4] | q[5] << 8, q[6]);
    else if (q[1] == 11)
      Serial.printf("  IAD: interfaces %u..%u, class %02X\n", q[2], q[2] + q[3] - 1, q[4]);
    else
      Serial.printf("    descriptor type %02X, %u bytes\n", q[1], q[0]);
  }
  for (uint16_t i = 0; i < n; ++i) Serial.printf("%02x%s", p[i], (i % 32 == 31 || i == n - 1) ? "\n" : " ");
  const ob::Layout& l = rytm.layout();
  Serial.printf("expected (rytm-emu, OS 1.73): 6 interfaces, 0x83/0x03 interrupt 368 B bInterval 1 at alt 3, MIDI interface 5\n");
  if (rytm.state() == RytmOverbridge::kRefused) Serial.printf("the Overbridge driver refused: %s\n", rytm.refused());
  else if (l.in.found) Serial.printf("found: in %u B / out %u B, %d / %d channels, MIDI interface %d\n", l.in.mps, l.out.mps,
                                     l.in_channels, l.out_channels, l.midi_iface);
}

static void print_stats(uint32_t dt_ms) {
  static RytmOverbridge::Stats prev;
  RytmOverbridge::Stats s;
  rytm.stats(&s, true);
  if (s.fresh) memset(&prev, 0, sizeof prev);   // a new attach: counters from 0
  uint32_t din = s.in_frames - prev.in_frames, dout = s.out_frames - prev.out_frames;
  Serial.printf("ob %s: in %lu Hz (%lu blocks), out %lu Hz; fifo %d (min %d max %d, target %d); xruns %lu blocks "
                "(%lu frames); bad %lu, counter gaps %lu, unanswered %lu, rearms %lu/%lu, setup timeouts %lu, queue fails %lu\n",
                rytm.streaming() ? "streaming" : rytm.stateName(), (unsigned long)(din * 1000 / dt_ms),
                (unsigned long)s.in_blocks, (unsigned long)(dout * 1000 / dt_ms), s.fifo_now, s.fifo_min, s.fifo_max,
                kTarget, (unsigned long)s.xrun_blocks, (unsigned long)s.xrun_frames, (unsigned long)s.bad_blocks,
                (unsigned long)s.counter_gaps, (unsigned long)s.credit_drops, (unsigned long)s.in_rearms,
                (unsigned long)s.out_rearms, (unsigned long)s.setup_timeouts,
                (unsigned long)s.queue_fails);
  // peaks in 1/1000 of full scale: MAIN L R | voices in track order | AUDIO IN L R
  Serial.print("  peaks (x1000): main");
  for (int c = 0; c < rytm.channelsIn() && c < ob::kChannels; ++c)
    Serial.printf("%s %ld", c == 2 ? " | BD SD RS BT LT MT CH CY" : c == 10 ? " | in" : "",
                  (long)((int64_t)s.peak[c] * 1000 / 2147483647));
  Serial.printf("\n  midi: %lu clocks (%lu BPM), %lu SysEx in\n", (unsigned long)clocks,
                (unsigned long)(clocks * 60000UL / 24 / dt_ms), (unsigned long)sysex_in);
  clocks = sysex_in = 0;
  prev = s;
}

static void poll_midi() {
  bool now = midi;
  if (now != midi_was) {
    midi_was = now;
    if (now) {
      const uint8_t *m = midi.manufacturer(), *p = midi.product();
      Serial.printf("MIDI attached: %04X:%04X '%s' '%s'\n", midi.idVendor(), midi.idProduct(), m ? (const char*)m : "",
                    p ? (const char*)p : "");
    } else {
      Serial.println("MIDI gone");
    }
  }
  // read() is false for SysEx continuation packets too: stop only after a run
  // of them, the rest comes next pass
  for (int misses = 0, k = 0; misses < 64 && k < 1024; ++k) {
    if (!midi.read()) {
      ++misses;
      continue;
    }
    misses = 0;
    uint8_t type = midi.getType();
    if (type == MIDIDeviceBase::SystemExclusive) {
      ++sysex_in;
      const uint8_t* d = midi.getSysExArray();
      uint16_t n = midi.getSysExArrayLength();
      Serial.printf("SysEx %u B:", n);
      for (int i = 0; i < n && i < 12; ++i) Serial.printf(" %02X", d[i]);
      Serial.println(n > 12 ? " ..." : "");
    } else if (type == MIDIDeviceBase::Clock) {
      ++clocks;
    } else if (type == MIDIDeviceBase::Start || type == MIDIDeviceBase::Stop || type == MIDIDeviceBase::Continue) {
      Serial.printf("transport %s\n", type == MIDIDeviceBase::Start ? "START" : type == MIDIDeviceBase::Stop ? "STOP" : "CONTINUE");
    } else if (print_midi && type != MIDIDeviceBase::ActiveSensing) {
      Serial.printf("MIDI %02X ch %u %u %u\n", type, midi.getChannel(), midi.getData1(), midi.getData2());
    }
  }
}

// Keep kTarget frames queued for the Rytm: the tone, or silence.
static void feed() {
  if (!rytm.streaming()) return;
  int16_t f[16][ob::kVoices];
  while (rytm.fifoLevel() < kTarget) {
    memset(f, 0, sizeof f);
    if (tone_on)
      for (int i = 0; i < 16; ++i, phase += kStep) f[i][tone_voice] = sine[phase >> 24];
    if (rytm.pushVoices(f, 16) < 16) break;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) {}
  for (int i = 0; i < 256; ++i) sine[i] = (int16_t)(8192 * sinf(i * 6.2831853f / 256));   // -12 dBFS
  rytm.setLog(log_line);
  rytm.setAutoStart(false);
  rytm.setMainMirror(tone_voice);
  Serial.println("rytm-teensy obhost_bringup: plug the Rytm into the host header; 'd' descriptors, 'g' start audio");
  usb.begin();
}

void loop() {
  usb.Task();
  poll_midi();
  feed();
  uint16_t n;
  rytm.config(&n);
  if (n && n != said_cfg) {
    said_cfg = n;
    Serial.printf("Elektron device %04X:%04X enumerated at %s\n", rytm.vid(), rytm.pid(),
                  rytm.speed() == 2 ? "480 Mbit (high speed)" : "12 or 1.5 Mbit - Overbridge 2 will not run");
  }
  if (rytm.state() != said) {
    said = rytm.state();
    if (said == RytmOverbridge::kRefused) Serial.printf("Overbridge driver: refused - %s\n", rytm.refused());
    else if (said == RytmOverbridge::kIdle) Serial.println("Overbridge driver: idle (device gone?)");
  }
  uint32_t now = millis();
  if (rytm.streaming() && now - last_stats >= 1000) {
    print_stats(now - last_stats);
    last_stats = now;
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd') print_descriptors();
    else if (c == 'g') { rytm.start(); last_stats = millis(); Serial.println("starting the Overbridge stream"); }
    else if (c == 'o') {
      static bool on = true;
      on = !on;
      rytm.setOutEnabled(on);
      Serial.printf("OUT stream %s\n", on ? "on" : "off (IN only)");
    } else if (c == 't') {
      tone_on = !tone_on;
      Serial.printf("tone %s, voice %d%s\n", tone_on ? "on" : "off", tone_voice, tone_main ? " and MAIN L/R" : "");
    } else if (c == 'v') {
      tone_voice = (tone_voice + 1) % 8;
      if (tone_main) rytm.setMainMirror(tone_voice);
      Serial.printf("tone voice %d (Overbridge input channel %d)\n", tone_voice, ob::kVoiceToRytm[tone_voice]);
    } else if (c == 'm') {
      tone_main = !tone_main;
      rytm.setMainMirror(tone_main ? tone_voice : -1);
      Serial.printf("tone on MAIN L/R %s\n", tone_main ? "on" : "off");
    } else if (c == 'b') {
      midi.sendNoteOn(0, 127, 14);
      for (uint32_t t0 = millis(); millis() - t0 < 50;) feed();   // the FIFO kept full meanwhile
      midi.sendNoteOff(0, 0, 14);
      Serial.println("sent BD trig (channel 14, note 0)");
    } else if (c == 'q') {
      print_midi = !print_midi;
    } else if (c == 's') {
      print_stats(now - last_stats ? now - last_stats : 1);
      last_stats = now;
    } else if (c == 'h') {
      uint8_t b[368];
      if (!rytm.firstBlock(b, sizeof b)) Serial.println("no IN block yet");
      else {
        Serial.print("first IN block header:");
        for (int i = 0; i < 32; ++i) Serial.printf(" %02x", b[i]);
        Serial.println();
      }
    } else if (c == 'R') {
      Serial.flush();
      delay(20);
      SCB_AIRCR = 0x05FA0004;
    }
  }
}
