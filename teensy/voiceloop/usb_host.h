// voiceloop with the Teensy as the Rytm's USB host (RYTM_USB_HOST 1): included
// into voiceloop.ino's namespace, after send_frames(). Replaces the serial
// bridge (sim/teensy_bridge.py) with the Rytm itself:
//   - USB MIDI (MIDIDevice_BigBuffer, the Rytm's interface 5): the Rytm's
//     F0 7D 52 54 reports go to report(), as the bridge's 'S' frames did; its
//     note-on/off and CC to midi_channel() (voiceloop.ino: with ':notes=1' a
//     stock Rytm's channel N plays voice N-1, core/notes.h); what
//     the sketch sends the Rytm (to_rytm: 14/15/16/17, 0E, ...) goes back as
//     SysEx; clock and transport are counted for the log.
//   - Overbridge (RytmHost): the Rytm pulls 48000 / 7 blocks a second; each
//     block takes 7 frames of the 8 voices from RytmOverbridge's FIFO, which
//     loop() keeps at host_target frames by rendering on demand (the Rytm is
//     the clock). An empty FIFO sends silence and counts an xrun.
//   - The log goes out as plain text lines (no 'L' framing): any serial
//     monitor, or teensy/serial_read.py.
// With RMP_CORE=uac2 (USB_SERIAL_UAC2) the Teensy is also a 12 in / 12 out USB audio device for a computer: the
// block below host_setup's notes ("The computer over USB Audio Class 2"), console ':mac=on|off'.
// Serial: the monitor keys 's' the host stats now, '+' / '-' the FIFO target x2 / /2
// (32..RYTM_OB_FIFO - 32 frames), 'R' reboot; console lines ':fifo=N' (the target), ':cap=MS' (a latency
// capture, below); and the serial build's development frames
// (voiceloop.ino's header: 'S' 'N' 'I' 'J' 'K' 'U' 'D', 'P' ping), after which the log is framed
// until a reboot.

#ifndef RYTM_HOST_TARGET
#define RYTM_HOST_TARGET 96    // frames kept queued for the Rytm (2 ms): the sweep's smallest clean target (64) plus a
                               // step, docs/teensy-host-rytm.md section 4 (was 256: a card read <= 0.8 ms, a project
                               // change 3.8 ms, PLAN 3c - ':fifo=256' when that matters)
#endif
#ifndef RYTM_HOST_HUB
#define RYTM_HOST_HUB 0        // 1: a USB hub between the Teensy and the Rytm
#endif

USBHost usb;
#if RYTM_HOST_HUB
USBHub usb_hub(usb);   // only with a hub between them (bandwidth shared; 1.5 KB of ITCM)
#endif
MIDIDevice_BigBuffer rytm_midi(usb);
RytmOverbridge rytm_ob(usb);
int host_target = RYTM_HOST_TARGET;
uint32_t midi_clocks = 0, midi_sysex_in = 0, midi_sysex_out = 0, midi_sysex_dropped = 0, host_log_ms = 0;
bool midi_attached = false, host_log_now = false;
RytmOverbridge::Stats host_prev;

void host_to_rytm(const uint8_t* m, int n) {
  if (!rytm_midi) {
    ++midi_sysex_dropped;
    return;
  }
  if (n == 3 && m[0] >= 0x80 && m[0] < 0xF0) {   // a channel message (the LFOs' CCs, core/lfo.h)
    rytm_midi.send(m[0] & 0xF0, m[1], m[2], (m[0] & 0x0F) + 1, 0);
    return;
  }
  rytm_midi.sendSysEx(n, m, true);   // m is F0 .. F7
  ++midi_sysex_out;
}

// out=main (voiceloop.ino's out_main): the voices summed onto MAIN L/R in each frame's slot 0, the library told
// to send slot 0 alone (setMainOnly). The fade starts again on each new stream and on the switch.
MainOut main_out;
bool host_was_streaming = false;
__attribute__((noinline)) FLASHMEM void host_out(bool main) {
  if (main != out_main) main_out.Reset();
  out_main = main;
  rytm_ob.setMainOnly(main);   // drops the frames queued for the other layout
}
// n frames of the ring in place: MAIN in slot 0, the other slots 0. Flash: about 30 cycles a frame.
__attribute__((noinline)) FLASHMEM void main_mix(int16_t (*f)[kVoices], int n) {
  for (int i = 0; i < n; ++i) {
    int16_t m = main_out.Next16(f[i]);
    memset(f[i], 0, sizeof f[i]);
    f[i][0] = m;
  }
}

#if defined(USB_SERIAL_UAC2)
// The computer over USB Audio Class 2 (built with RMP_CORE=uac2, teensy/uac2/usb_uac2.c), the Rytm the clock:
//   - Rytm -> computer: the Overbridge IN frames (12 channels: MAIN L/R, BD SD RS BT LT MT CH CY, AUDIO IN L/R) go
//     to UAC2 IN channels 1..12 as they come (host_rx, the USB host interrupt) through uac_in[], read by the UAC2
//     interrupt (usb_uac2_in_fill). The UAC2 clock (usb_uac2_clock_q16) is the Rytm's IN frame count, smoothed
//     between transfers by the cycle counter: IN packet sizes and the OUT feedback follow the Rytm's clock.
//   - computer -> Rytm: UAC2 OUT channels 1..12 are added (saturating) to the Overbridge OUT block's channels
//     1..12 (MAIN L/R, MAIN FX L/R, the 8 tracks' inputs BD SD RS BT LT MT CH CY) on top of the voices, read
//     from the driver's OUT ring as each block is built (host_tx, the USB host interrupt). ':mac=off' stops it.
// Both interrupts have the same priority (no preemption between them); each ring has one writer per index.
#ifndef UAC_IN_FRAMES
#define UAC_IN_FRAMES 256                 // frames (power of 2): 5.3 ms
#endif
#ifndef UAC_IN_DELAY
#define UAC_IN_DELAY 40                   // frames the computer's IN runs behind the Rytm: a 14-frame transfer + a
#endif                                    // 13-frame packet + margin (0.83 ms)
// the Rytm's channels kept for the computer: all 12, or MAIN L/R only in a stereo build (UAC2_CH_IN=2, host_stereo.sh)
#define UAC_IN_CH (UAC2_CH_IN < 12 ? UAC2_CH_IN : 12)
#ifndef UAC_IN_ATTR
#define UAC_IN_ATTR                       // RAM1 (12 KB): RAM2 is full in the multi build; the stereo build: DMAMEM, 1 KB
#endif
#if REC_POOL   // in the pool with the recorder's ring (rec_mini.h): after the driver's OUT ring, both lent while it records
static_assert((UAC2_RING_FRAMES * UAC2_CH_OUT + UAC_IN_FRAMES * UAC_IN_CH) * 4 <= RYTM_REC_RING_KB * 1024,
              "USB audio's rings do not fit the recorder's pool (RYTM_REC_RING_KB)");
int32_t (*const uac_in)[UAC_IN_CH] = (int32_t (*)[UAC_IN_CH])(UAC2_RING_MEM + UAC2_RING_FRAMES * UAC2_CH_OUT);
#else
UAC_IN_ATTR int32_t uac_in[UAC_IN_FRAMES][UAC_IN_CH];
#endif
volatile uint32_t uac_in_wr = 0, uac_in_rd = 0, uac_in_over = 0, uac_rx_cyc = 0, uac_rx_n = 0;
volatile uint64_t uac_rx_frames = 0;      // Rytm IN frames seen (the clock)
volatile uint32_t uac_last_fill = 0;      // CYCCNT at the last IN packet filled (the computer reads IN)
volatile uint32_t uac_fill_ms = 0;        // millis() then: "is the computer reading IN" for loop() and host_rx (no wrap)
volatile int32_t uac_lvl16 = UAC_IN_DELAY * 16;   // uac_in[]'s level, smoothed, x16
volatile uint32_t uac_hist[3];            // IN packets of 11 / 12 / 13 frames
volatile bool uac_mac = true;             // ':mac=on|off': the computer's OUT to the Rytm
volatile uint32_t uac_gap_max = 0, uac_starts = 0;   // the longest time between IN packets filled (cycles), IN (re)starts
#if UAC2_CH_IN >= 20
// The 8 voices to the computer, IN 13..20 in track order (BD SD RS BT LT MT CH CY, as IN 3..10): each voice's own
// signal as rendered (after its level, before the MAIN mix and its limiter), pushed by host_feed (loop()) before
// out=main mixes the ring, read by the UAC2 interrupt. RAM1, 4 KB.
#define UAC_VOICES 1
#ifndef UAC_V_DELAY
#define UAC_V_DELAY 160                   // frames the reader keeps behind the newest: the Rytm IN ring's and loop()'s jitter (96 underran, 1.7 ms swings)
#endif
int16_t uac_voice[UAC_IN_FRAMES][kVoices];
volatile uint32_t uac_v_wr = 0, uac_v_rd = 0, uac_v_under = 0, uac_v_over = 0, uac_v_resync = 0;
volatile uint32_t uac_v_min = 0xFFFFFFFF, uac_v_max = 0;
// physical voice v -> its IN channel (12 + its track index): ob::kVoiceFromRytm is 2 + the track index
FLASHMEM void uac_voices_push(const int16_t (*f)[kVoices], int n) {
  // the computer is not reading IN (no packet for 1-2 ms): nothing kept. uac_fill_ms read first, then millis(): the UAC2
  // interrupt may fill a packet between the two reads (the other order, CYCCNT, wrapped to "long ago" and emptied
  // the ring now and then; CYCCNT's 7 s wrap also made a long idle look recent)
  const uint32_t lf = uac_fill_ms;
  if (millis() - lf > 1) {
    uac_v_wr = uac_v_rd;
    return;
  }
  uint32_t wr = uac_v_wr;
  for (int i = 0; i < n; ++i) {
    if (wr - uac_v_rd >= UAC_IN_FRAMES) {
      uac_v_over += n - i;
      break;
    }
    memcpy(uac_voice[wr & (UAC_IN_FRAMES - 1)], f[i], sizeof uac_voice[0]);
    ++wr;
  }
  __asm__ volatile("" ::: "memory");
  uac_v_wr = wr;
}
#else
#define UAC_VOICES 0
#endif
FLASHMEM void cap_rx(const int32_t* f, int n, int ch);
// The USB host interrupt, after each Overbridge IN transfer.
FLASHMEM void host_rx(const int32_t* f, int n, int ch) {
  if (ch == 12 && !usb_uac2_paused) {   // paused: uac_in's memory is the recorder's (rec_mini.h)
    uint32_t wr = uac_in_wr;
    for (int i = 0; i < n; ++i) {
      if (wr - uac_in_rd >= UAC_IN_FRAMES) {
        const uint32_t lf = uac_fill_ms;
        if (usb_uac2_stats.in_alt && millis() - lf <= 1)
          uac_in_over += n - i;   // the computer reads IN (selected, a packet within 2 ms): frames lost
        break;                                             // (idle: the reader realigns when it starts)
      }
      memcpy(uac_in[wr & (UAC_IN_FRAMES - 1)], f + i * 12, UAC_IN_CH * 4);
      ++wr;
    }
    __asm__ volatile("" ::: "memory");
    uac_in_wr = wr;
  }
  uac_rx_frames += n;
  uac_rx_n = n;
  uac_rx_cyc = ARM_DWT_CYCCNT;
  if (cap_on) cap_rx(f, n, ch);
}
// The UAC2 interrupt: the next IN packet's size from uac_in[]'s level (smoothed, x16), kept near UAC_IN_DELAY:
// 12 frames, 11 or 13 outside +-7 (a 14-frame transfer swings the level by that much). Its counts in uac_hist.
extern "C" FLASHMEM unsigned usb_uac2_in_frames(unsigned nom) {
  if (usb_uac2_paused) return 0;   // silence at the clock's rate (the driver's own sizes)
  int32_t lvl = (int32_t)(uac_in_wr - uac_in_rd);
  uac_lvl16 += lvl - (uac_lvl16 >> 4);
  int32_t d = (uac_lvl16 >> 4) - UAC_IN_DELAY;
  unsigned n = d > 7 ? nom + 1 : d < -7 ? nom - 1 : nom;
  ++uac_hist[n - (nom - 1)];
  return n;
}
// The UAC2 interrupt: n frames for the next IN packet, 24-bit right-aligned.
extern "C" FLASHMEM void usb_uac2_in_fill(int32_t* dst, unsigned n) {
  if (usb_uac2_paused) {   // the recorder holds the pool: silence (the first packet after it starts again realigns)
    memset(dst, 0, n * UAC2_CH_IN * sizeof(int32_t));
    return;
  }
  static uint32_t last_cyc = 0;
  uint32_t rd = uac_in_rd;
  const uint32_t wr = uac_in_wr;
  const bool live = rytm_ob.streaming();
  const uint32_t c = ARM_DWT_CYCCNT;
  // the first packet after 1-2 ms without (packets come every 250 us, at most 271 us apart measured; millis:
  // CYCCNT wraps every 7 s, a longer pause could look short)
  const bool start = millis() - uac_fill_ms > 1;
  if (start) ++uac_starts;
  else if (c - last_cyc > uac_gap_max) uac_gap_max = c - last_cyc;
  if (start) {   // start UAC_IN_DELAY frames back
    uint32_t lvl = wr - rd;
    rd = wr - (lvl < UAC_IN_DELAY ? lvl : UAC_IN_DELAY);
    uac_lvl16 = UAC_IN_DELAY * 16;
  }
  last_cyc = c;
  uac_last_fill = c;
  uac_fill_ms = millis();
#if UAC_VOICES
  {
    uint32_t vrd = uac_v_rd;
    const uint32_t vwr = uac_v_wr;
    uint32_t vl = vwr - vrd;
    // after a start (nothing kept while the computer did not read) or an underrun, silence until UAC_V_DELAY frames
    // are there, then read that far behind the newest: room for loop()'s bursts (uac_v_under counts the underruns)
    static bool priming = true;
    if (start) priming = true;
    if (!priming) {
      if (vl < uac_v_min) uac_v_min = vl;
      if (vl > uac_v_max) uac_v_max = vl;
    }
    if (priming && vl >= UAC_V_DELAY) {
      priming = false;
      vrd = vwr - UAC_V_DELAY;
    } else if (!priming && vl > UAC_IN_FRAMES - 32) {   // too far behind: realign
      ++uac_v_resync;
      vrd = vwr - UAC_V_DELAY;
    }
    for (unsigned i = 0; i < n; ++i) {
      int32_t* d = dst + i * UAC2_CH_IN + 12;
      if (!priming && vrd != vwr) {
        const int16_t* v = uac_voice[vrd & (UAC_IN_FRAMES - 1)];
        for (int k = 0; k < kVoices; ++k) d[ob::kVoiceFromRytm[k] - 2] = (int32_t)v[k] << 8;
        ++vrd;
      } else {
        memset(d, 0, kVoices * 4);
        if (live && !priming) {   // short: one gap, then the margin again
          ++uac_v_under;
          priming = true;
        }
      }
    }
    __asm__ volatile("" ::: "memory");
    uac_v_rd = vrd;
  }
#endif
  for (unsigned i = 0; i < n; ++i) {
    if (rd != wr) {
      const int32_t* s = uac_in[rd & (UAC_IN_FRAMES - 1)];
      for (int c = 0; c < UAC_IN_CH; ++c) dst[i * UAC2_CH_IN + c] = s[c] >> 8;
      ++rd;
    } else {
      memset(dst + i * UAC2_CH_IN, 0, UAC_IN_CH * 4);
      if (live) ++usb_uac2_in_underrun;
    }
  }
  __asm__ volatile("" ::: "memory");
  uac_in_rd = rd;
}
#if REC_POOL
// From loop() (rec_mini.h): USB audio paused, its rings' memory lent to the recorder (true), or back (false). The
// driver's pause resets its OUT ring; uac_in starts empty either way (the IN reader realigns on its next packet).
FLASHMEM void uac_pool_lend(bool lend) {
  if (lend) usb_uac2_pause(1);   // first: neither interrupt touches the rings after this
  __disable_irq();
  uac_in_wr = uac_in_rd = 0;
  uac_lvl16 = UAC_IN_DELAY * 16;
  __enable_irq();
  if (!lend) usb_uac2_pause(0);  // last: the rings are reset before the interrupts use them again
}
#endif
// The UAC2 clock (frames x 65536): the Rytm's IN frames while it streams, the transfer's frames spread over the
// time since it came (at most the transfer's own count: monotonic); the cycle counter (48 kHz) without it.
extern "C" FLASHMEM uint64_t usb_uac2_clock_q16(void) {
  static uint64_t acc = 0, last_r = 0;
  static uint32_t last_c = 0;
  static bool rmode = false;
  const uint32_t c = ARM_DWT_CYCCNT;
  const uint32_t cpf = F_CPU_ACTUAL / 48000;
  __disable_irq();
  const uint64_t fr = uac_rx_frames;
  const uint32_t rc = uac_rx_cyc, rn = uac_rx_n;
  __enable_irq();
  const uint32_t since = c - rc;
  if (rytm_ob.streaming() && fr && since < cpf * 240) {   // a transfer within 5 ms
    uint64_t interp = ((uint64_t)since << 16) / cpf;
    if (interp > ((uint64_t)rn << 16)) interp = (uint64_t)rn << 16;
    uint64_t r = ((fr - rn) << 16) + interp;
    if (!rmode) { last_r = r; rmode = true; }
    if (r > last_r) acc += r - last_r;
    last_r = r;
  } else {
    rmode = false;
    acc += ((uint64_t)(uint32_t)(c - last_c) << 16) / cpf;
  }
  last_c = c;
  return acc;
}
// The USB host interrupt, once an OUT block: the computer's 7 frames (zeros while it plays nothing).
FLASHMEM void host_tx(int32_t* f, int n, int ch) {
  int32_t m[7 * UAC2_CH_OUT];
  if (n > 7) n = 7;
  usb_uac2_out_read(m, n);
  if (!uac_mac) return;
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < ch && c < UAC2_CH_OUT; ++c) f[i * ch + c] = m[i * UAC2_CH_OUT + c] << 8;
}
#if defined(MIDI_INTERFACE)
// USB MIDI (the composite's MIDI interface, usbMIDI): the Rytm's MIDI to the computer and the computer's to the Rytm,
// from loop() (host_poll), merged with the voiceloop's own sends (all whole messages, one context).
//   - to the computer: everything the Rytm sends (notes, CC, PC, pitch bend, pressure, clock, transport, song
//     position, SysEx) except our host link's SysEx (F0 7D 52 54 ..), which stays between the Rytm and the Teensy;
//   - to the Rytm: channel messages, clock and transport; SysEx only when complete, not ours (7D 52 54), and not
//     Elektron's (F0 00 20 3C ..: OS updates and dumps never reach the Rytm through here).
uint32_t mm_to_mac = 0, mm_to_rytm = 0, mm_blocked = 0;
FLASHMEM bool mm_ours(const uint8_t* d, int n) { return n >= 4 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54; }
FLASHMEM void mm_from_rytm(uint8_t type) {
  if (type == MIDIDeviceBase::SystemExclusive) {
    const uint8_t* d = rytm_midi.getSysExArray();
    int n = rytm_midi.getSysExArrayLength();
    if (mm_ours(d, n)) return;
    usbMIDI.sendSysEx(n, d, true);
  } else {
    if (mm_to_mac < 16 && type != MIDIDeviceBase::Clock)
      logf(PSTR("usb midi: from the Rytm #%lu: %02x ch %u %u %u"), (unsigned long)mm_to_mac + 1, type,
           rytm_midi.getChannel(), rytm_midi.getData1(), rytm_midi.getData2());
    usbMIDI.send(type, rytm_midi.getData1(), rytm_midi.getData2(), rytm_midi.getChannel(), 0);
  }
  ++mm_to_mac;
}
FLASHMEM void mm_from_mac() {
  for (int k = 0; k < 64 && usbMIDI.read(); ++k) {
    uint8_t type = usbMIDI.getType();
    if (type == usbMIDI.SystemExclusive) {
      const uint8_t* d = usbMIDI.getSysExArray();
      int n = usbMIDI.getSysExArrayLength();
      bool whole = n >= 2 && d[0] == 0xF0 && d[n - 1] == 0xF7;
      if (!whole || mm_ours(d, n) || (n >= 4 && d[1] == 0x00 && d[2] == 0x20 && d[3] == 0x3C) || !rytm_midi) {
        if (mm_blocked < 16)
          logf(PSTR("usb midi: SysEx from the computer not passed (%d B, %02x %02x %02x %02x)"), n, n > 0 ? d[0] : 0,
               n > 1 ? d[1] : 0, n > 2 ? d[2] : 0, n > 3 ? d[3] : 0);
        ++mm_blocked;
        continue;
      }
      rytm_midi.sendSysEx(n, d, true);
    } else {
      if (!rytm_midi) continue;
      rytm_midi.send(type, usbMIDI.getData1(), usbMIDI.getData2(), usbMIDI.getChannel(), 0);
    }
    if (mm_to_rytm < 64 || mm_to_rytm % 1000 == 0)   // what the computer sends the Rytm, visibly
      logf(PSTR("usb midi: to the Rytm #%lu: %02x ch %u %u %u"), (unsigned long)mm_to_rytm + 1, type,
           usbMIDI.getChannel(), usbMIDI.getData1(), usbMIDI.getData2());
    ++mm_to_rytm;
  }
  usbMIDI.send_now();
}
#endif
// The USB mode (':usbmode=multi|stereo|off', ':usbmode'): what the computer sees on the micro-USB port.
//   multi  Serial + MIDI + audio UAC2_CH_IN in / UAC2_CH_OUT out (the Rytm's 12 channels, the voices on 13..20)
//   stereo Serial + MIDI + audio 2 in / 2 out (the Rytm's MAIN L/R both ways)
//   off    Serial + MIDI
// Kept in the emulated EEPROM (2 bytes at kUsbModeAddr, above the plugin store's flash, never in it), read before
// the USB starts (startup_middle_hook: the first enumeration has it). A change detaches the device (the computer
// sees it go), waits, and attaches it with the other descriptor; the Rytm's side (the USB host port) goes on.
#include <avr/eeprom.h>
constexpr uint32_t kUsbModeAddr = 0x0F00;   // magic 0x5C, then the mode
constexpr uint8_t kUsbModeMagic = 0x5C;
const char* const kUsbModeName[3] = {"multi", "stereo", "off"};
uint32_t usbmode_t0 = 0;
uint8_t usbmode_step = 0, usbmode_next = 0;   // 1: detach at t0 + 50 ms (the reply line goes out first), 2: attach
// A stereo build (UAC2_CH_IN=2, teensy/host_stereo.sh: the Sidecar Mini) has stereo and off only (multi kept from a
// multi build reads as stereo); its mode before any ':usbmode' is UAC_MODE_DEFAULT (stereo unless given).
#define UAC_STEREO_ONLY (UAC2_CH_IN <= 2)
#ifndef UAC_MODE_DEFAULT
#define UAC_MODE_DEFAULT (UAC_STEREO_ONLY ? UAC2_MODE_STEREO : UAC2_MODE_MULTI)
#endif
extern "C" FLASHMEM void startup_middle_hook(void) {
  usb_uac2_mode = UAC_MODE_DEFAULT;
  if (eeprom_read_byte((const uint8_t*)kUsbModeAddr) == kUsbModeMagic) {
    uint8_t m = eeprom_read_byte((const uint8_t*)(kUsbModeAddr + 1));
    if (m <= UAC2_MODE_OFF) usb_uac2_mode = m;
  }
  if (UAC_STEREO_ONLY && usb_uac2_mode == UAC2_MODE_MULTI) usb_uac2_mode = UAC2_MODE_STEREO;
}
// ':out' and ':notes' kept too (2 bytes after the mode: magic 0x5D, bit 0 out=main, bit 1 notes on), applied at boot
// (host_setup) over the build's defaults; written when a console line changes them.
constexpr uint32_t kSettingsAddr = kUsbModeAddr + 2;
constexpr uint8_t kSettingsMagic = 0x5D;
FLASHMEM void host_settings_load() {
  if (eeprom_read_byte((const uint8_t*)kSettingsAddr) != kSettingsMagic) return;
  uint8_t b = eeprom_read_byte((const uint8_t*)(kSettingsAddr + 1));
  out_main = b & 1;
  notes_on = (b >> 1) & 1;
}
FLASHMEM void host_settings_save() {
  uint8_t b = (out_main ? 1 : 0) | (notes_on ? 2 : 0);
  if (eeprom_read_byte((const uint8_t*)kSettingsAddr) != kSettingsMagic)
    eeprom_write_byte((uint8_t*)kSettingsAddr, kSettingsMagic);
  if (eeprom_read_byte((const uint8_t*)(kSettingsAddr + 1)) != b) eeprom_write_byte((uint8_t*)(kSettingsAddr + 1), b);
}
FLASHMEM void usbmode_show(const char* what) {
  uint8_t ci, co;
  usb_uac2_channels(&ci, &co);
#if defined(MIDI_INTERFACE)
  const char* const fn = "Serial + MIDI";
#else
  const char* const fn = "Serial";
#endif
  // the web page reads "multi not in this build" (its multi choice off) and the computer's state from the uac2 line
  const char* const only = UAC_STEREO_ONLY ? "; multi not in this build" : "";
  if (usb_uac2_mode == UAC2_MODE_OFF)
    logf(PSTR("usb: mode %s%s (%s, no audio%s)"), kUsbModeName[usb_uac2_mode], what, fn, only);
  else
    logf(PSTR("usb: mode %s%s (%s + audio %u in / %u out%s)"), kUsbModeName[usb_uac2_mode], what, fn, ci, co, only);
}
FLASHMEM void usbmode_cmd(const char* v) {
  if (!*v) {
    usbmode_show("");
    return;
  }
  int m = -1;
  for (int i = 0; i < 3; ++i)
    if (!strcmp(v, kUsbModeName[i])) m = i;
  if (m < 0 || (UAC_STEREO_ONLY && m == UAC2_MODE_MULTI)) {
    logf(PSTR("usb: no mode \"%s\" (%s)"), v, UAC_STEREO_ONLY ? "stereo, off" : "multi, stereo, off");
    return;
  }
  if (usbmode_step) {
    logf(PSTR("usb: a mode change is running"));
    return;
  }
  if (eeprom_read_byte((const uint8_t*)kUsbModeAddr) != kUsbModeMagic)
    eeprom_write_byte((uint8_t*)kUsbModeAddr, kUsbModeMagic);
  if (eeprom_read_byte((const uint8_t*)(kUsbModeAddr + 1)) != m) eeprom_write_byte((uint8_t*)(kUsbModeAddr + 1), m);
  if (m == usb_uac2_mode) {
    usbmode_show(" (unchanged)");
    return;
  }
  logf(PSTR("usb: mode %s, re-enumerating (the serial port goes away for about a second)"), kUsbModeName[m]);
  usbmode_next = m;
  usbmode_step = 1;
  usbmode_t0 = millis();
}
// From host_poll: the steps, no waiting in loop() (the Rytm's FIFO keeps being fed).
FLASHMEM void usbmode_poll() {
  if (!usbmode_step) return;
  uint32_t dt = millis() - usbmode_t0;
  if (usbmode_step == 1 && dt >= 50) {
    usb_uac2_detach();
    usb_uac2_mode = usbmode_next;
    usbmode_step = 2;
  } else if (usbmode_step == 2 && dt >= 400) {
    usb_uac2_attach();
    usbmode_step = 0;
  }
}
FLASHMEM void uac_mac_set(const char* v) {
  if (*v) uac_mac = v[0] == 'o' && v[1] == 'n';
  logf(PSTR("uac2: the computer's OUT 1..12 to the Rytm's Overbridge inputs %s (MAIN L/R, MAIN FX L/R, BD SD RS BT LT MT CH CY)"),
       uac_mac ? "on" : "off");
}
struct usb_uac2_stats uac_prev;
FLASHMEM void uac_log() {
  {
    RytmOverbridge::Stats t;   // totals since the Rytm attached (the per-line counts miss lines lost while detached)
    rytm_ob.stats(&t, false);
    logf(PSTR("usb host: since attach xruns %lu blocks %lu frames, unanswered %lu, bad %lu, gaps %lu; usb mode %s"),
         (unsigned long)t.xrun_blocks, (unsigned long)t.xrun_frames, (unsigned long)t.credit_drops,
         (unsigned long)t.bad_blocks, (unsigned long)t.counter_gaps, kUsbModeName[usb_uac2_mode]);
  }
  struct usb_uac2_stats s;
  __disable_irq();
  memcpy(&s, (const void*)&usb_uac2_stats, sizeof s);
  const uint32_t in_lvl = uac_in_wr - uac_in_rd, in_over = uac_in_over, in_under = usb_uac2_in_underrun;
  __enable_irq();
  usb_uac2_stats_reset_minmax();
  logf(PSTR("uac2: %s, alt in/out %u/%u, in %lu pk %lu fr (11/12/13 %lu/%lu/%lu, ring %lu, under %lu over %lu), "
       "fb %lu.%03lu Hz base %lu.%03lu Hz, sof %lu, mac %s%s"),
       s.configured ? (s.high_speed ? "480M" : "12M") : "not configured", s.in_alt, s.out_alt,
       (unsigned long)(s.in_pkts - uac_prev.in_pkts), (unsigned long)(s.in_frames - uac_prev.in_frames),
       (unsigned long)uac_hist[0], (unsigned long)uac_hist[1], (unsigned long)uac_hist[2],
       (unsigned long)in_lvl, (unsigned long)in_under, (unsigned long)in_over,
       (unsigned long)((uint64_t)s.fb * 8000 >> 16), (unsigned long)(((uint64_t)s.fb * 8000 & 0xFFFF) * 1000 >> 16),
       (unsigned long)((uint64_t)s.fb_base * 8000 >> 16), (unsigned long)(((uint64_t)s.fb_base * 8000 & 0xFFFF) * 1000 >> 16),
       (unsigned long)(s.sof - uac_prev.sof), uac_mac ? "on" : "off",
       usb_uac2_paused ? ", paused for the recorder (IN silent, OUT dropped)" : "");
#if UAC_VOICES
  logf(PSTR("uac2: voices on IN 13..20, ring %lu [%lu..%lu], under %lu over %lu resync %lu"), (unsigned long)(uac_v_wr - uac_v_rd),
       (unsigned long)(uac_v_min == 0xFFFFFFFF ? 0 : uac_v_min), (unsigned long)uac_v_max,
       (unsigned long)uac_v_under, (unsigned long)uac_v_over, (unsigned long)uac_v_resync);
  uac_v_min = 0xFFFFFFFF;
  uac_v_max = 0;
  logf(PSTR("uac2: IN fill gap max %lu us, starts %lu"), (unsigned long)(uac_gap_max / (F_CPU_ACTUAL / 1000000)),
       (unsigned long)uac_starts);
  uac_gap_max = 0;
#endif
#if defined(MIDI_INTERFACE)
  logf(PSTR("usb midi: to the computer %lu, to the Rytm %lu, blocked %lu"), (unsigned long)mm_to_mac,
       (unsigned long)mm_to_rytm, (unsigned long)mm_blocked);
#endif
  logf(PSTR("uac2: out %lu pk %lu fr (11/12/13 %lu/%lu/%lu, ring %lu [%lu..%lu], under %lu over %lu, bad %lu err %lu)"),
       (unsigned long)(s.out_pkts - uac_prev.out_pkts), (unsigned long)(s.out_frames - uac_prev.out_frames),
       (unsigned long)s.out_hist[0], (unsigned long)s.out_hist[1], (unsigned long)s.out_hist[2], (unsigned long)s.out_level,
       (unsigned long)(s.out_level_min == 0xFFFFFFFF ? 0 : s.out_level_min), (unsigned long)s.out_level_max,
       (unsigned long)s.out_underrun, (unsigned long)s.out_overrun, (unsigned long)s.out_badlen, (unsigned long)s.out_err);
  uac_prev = s;
}
#endif

#include "sendfx_host.h"
FLASHMEM void cap_rx(const int32_t* f, int n, int ch);   // below
// The Overbridge callbacks (the USB host interrupt): the computer's UAC2 path and the capture, then the send FX.
FLASHMEM void ob_rx(const int32_t* f, int n, int ch) {
  if (core::ui::tap_on()) core::ui::tap_in(f, n, ch);   // the remote UI's SCOPE / meters: compares and stores only
  g_rec.in_block(f, n, ch);   // the recorder (rec_mini.h): copies only, nothing while idle
#if defined(USB_SERIAL_UAC2)
  host_rx(f, n, ch);   // the capture (':cap') runs from it too
#else
  if (cap_on) cap_rx(f, n, ch);
#endif
  sfx_rx(f, n, ch);
}
FLASHMEM void ob_tx(int32_t* f, int n, int ch) {
#if defined(USB_SERIAL_UAC2)
  host_tx(f, n, ch);   // writes its channels (the computer's OUT): first
#endif
  sfx_tx(f, n, ch);
}

FLASHMEM void host_setup() {
#if defined(USB_SERIAL_UAC2)
  host_settings_load();   // ':out' and ':notes' as last set (else the build's defaults: tracks, notes off)
#endif
  rytm_ob.setLog([](const char* s) { logf(PSTR("%s"), s); });
  rytm_ob.setMainOnly(out_main);
  sfx_begin();
  rytm_ob.setRxCallback(ob_rx);
  rytm_ob.setTxCallback(ob_tx);
#if defined(USB_SERIAL_UAC2)
  usb_uac2_in_delay = UAC_IN_DELAY;
  logf(PSTR("uac2: Serial + USB Audio 2, %d in / %d out at 48 kHz: IN 1..%d the Rytm's Overbridge channels"), UAC2_CH_IN, UAC2_CH_OUT, UAC_IN_CH);
  uac_mac_set("");
  usbmode_show("");
#endif
  usb.begin();
  logf(PSTR("output: %s"), out_main ? "main (the voices summed on MAIN L/R)" : "tracks (each voice on its track's channel)");
  logf(PSTR("usb host: waiting for the Rytm (OVERBRIDGE mode: audio and MIDI; USB MIDI mode: MIDI only)"));
}

// Latency capture (':cap=MS', off by default: nothing runs and nothing is allocated until asked). For MS ms of
// the IN clock each IN frame goes out over serial as 4 int16: IN MAIN L, IN MAIN R (the top 16 bits), OUT slot 0
// as sent (MAIN with out=main: the frame the FIFO hands the Rytm at that IN frame), and a mark: bit V (0..7) a
// MIDI note-on on channel V+1 arrived just before this IN frame, bit 8+V voice V's trigger takes effect in this
// OUT frame. As log lines "W <first frame, hex> <base64 of 18 frames>" (plain or 'L'-framed), drained from loop()
// as the serial port takes them; a full ring drops frames (counted in the closing line: a capture with any lost is void).
// tools/teensy_latency.py reads them into spike3/latency.py's 4-channel WAV. The buffers (11 KB, RAM2's heap)
// exist only during a capture.
FLASHMEM int cap_num(const char* v) {   // a decimal number (no atoi: libc code lands in ITCM)
  int n = 0;
  while (*v >= '0' && *v <= '9' && n < 1000000) n = n * 10 + (*v++ - '0');
  return n;
}
struct CapFrame { int16_t l, r, sent; uint16_t mark; };
constexpr int kCapRing = 1024, kCapMirror = 1024;   // powers of 2; the mirror > FIFO + the ring's rendered frames
CapFrame* cap_ring = nullptr;
int16_t* cap_sent = nullptr;     // OUT slot 0 by FIFO position (frames pushed)
uint8_t* cap_trig = nullptr;     // voices triggered at that OUT position
volatile uint32_t cap_wr = 0, cap_rd = 0, cap_left = 0, cap_lost = 0, cap_frame = 0;
volatile uint8_t cap_arrive = 0;
uint32_t cap_pushed = 0;         // frames pushed since the capture began (the FIFO's write side)
bool cap_done_pending = false;

FLASHMEM void cap_rx(const int32_t* f, int n, int ch) {   // the IN interrupt
  if (!cap_left || ch < 2) return;
  uint32_t rd = cap_pushed - (uint32_t)rytm_ob.fifoLevel();   // the next OUT frame the FIFO hands over
  for (int i = 0; i < n && cap_left; ++i, --cap_left, ++cap_frame) {
    uint32_t j = (rd + i) & (kCapMirror - 1);
    if (cap_wr - cap_rd >= (uint32_t)kCapRing) {
      ++cap_lost;
      cap_trig[j] = 0;
      continue;
    }
    CapFrame& c = cap_ring[cap_wr & (kCapRing - 1)];
    c.l = (int16_t)(f[i * ch] >> 16);
    c.r = (int16_t)(f[i * ch + 1] >> 16);
    c.sent = cap_sent[j];
    c.mark = (uint16_t)(cap_arrive | cap_trig[j] << 8);
    cap_arrive = 0;
    cap_trig[j] = 0;
    __asm__ volatile("" ::: "memory");
    ++cap_wr;
  }
}

// host_feed's push while capturing: slot 0 of each frame kept by its FIFO position, the count kept in step with
// the FIFO's (the interrupt reads both).
FLASHMEM int cap_push(const int16_t (*f)[kVoices], int n) {
  for (int i = 0; i < n; ++i) cap_sent[(cap_pushed + i) & (kCapMirror - 1)] = f[i][0];
  __disable_irq();
  int took = rytm_ob.pushVoices(f, n);
  cap_pushed += took;
  __enable_irq();
  return took;
}

// midi_channel's: a note-on (velocity > 0) on channels 1..8 while capturing. Its trigger takes effect in the next
// frame rendered: after the ring's frames not yet pushed.
FLASHMEM void cap_note(uint8_t st, uint8_t d2) {
  if (!cap_on || (st & 0xF0) != 0x90 || !d2 || (st & 0x0F) > 7) return;
  uint8_t b = 1u << (st & 0x0F);
  __disable_irq();
  cap_arrive |= b;
  if (notes_on) cap_trig[(cap_pushed + ring_n) & (kCapMirror - 1)] |= b;
  __enable_irq();
}

FLASHMEM void cap_start(int ms) {
#if !RYTM_DEBUG_TOOLS
  (void)ms;
  logf(PSTR("cap: a developer's tool, not in this build (RYTM_DEBUG_TOOLS=0)"));
  return;
#endif
  if (cap_on) {
    logf(PSTR("cap: one is running"));
    return;
  }
  if (!rytm_ob.streaming() || ms <= 0) {
    logf(PSTR("cap: no Overbridge stream (or no length)"));
    return;
  }
  cap_ring = (CapFrame*)malloc(kCapRing * sizeof(CapFrame));
  cap_sent = (int16_t*)malloc(kCapMirror * sizeof(int16_t));
  cap_trig = (uint8_t*)malloc(kCapMirror);
  if (!cap_ring || !cap_sent || !cap_trig) {
    free(cap_ring); free(cap_sent); free(cap_trig);
    cap_ring = nullptr; cap_sent = nullptr; cap_trig = nullptr;
    logf(PSTR("cap: no memory for the buffers"));
    return;
  }
  memset(cap_sent, 0, kCapMirror * sizeof(int16_t));
  memset(cap_trig, 0, kCapMirror);
  cap_wr = cap_rd = cap_lost = cap_frame = 0;
  cap_arrive = 0;
  cap_pushed = 0;
  logf(PSTR("cap: %d ms, %d frames, fifo target %d"), ms, ms * 48, host_target);
  __disable_irq();
  cap_left = (uint32_t)ms * 48;
  cap_on = true;
  __enable_irq();
}

// From host_poll: the frames out as the serial port takes them; the end once all are out.
FLASHMEM void cap_drain() {
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  constexpr int kLine = 18;   // frames a line: 144 bytes, 192 characters
  for (int k = 0; k < 32; ++k) {
    uint32_t avail = cap_wr - cap_rd;
    bool last = !cap_left;
    if (!avail || (avail < (uint32_t)kLine && !last) || Serial.availableForWrite() < 220) break;
    int n = avail < (uint32_t)kLine ? (int)avail : kLine;
    uint8_t raw[kLine * sizeof(CapFrame)];
    for (int i = 0; i < n; ++i) memcpy(raw + i * sizeof(CapFrame), &cap_ring[(cap_rd + i) & (kCapRing - 1)], sizeof(CapFrame));
    char line[220];
    int p = 0;   // "W <frames out before these, hex> " (no snprintf: libc's printf would land in ITCM)
    line[p++] = 'W';
    line[p++] = ' ';
    for (int sh = 28; sh >= 0; sh -= 4)
      if ((cap_rd >> sh) || !sh) line[p++] = "0123456789abcdef"[cap_rd >> sh & 15];
    line[p++] = ' ';
    int nb = n * (int)sizeof(CapFrame);
    for (int i = 0; i < nb; i += 3) {
      uint32_t v = raw[i] << 16 | (i + 1 < nb ? raw[i + 1] << 8 : 0) | (i + 2 < nb ? raw[i + 2] : 0);
      line[p++] = b64[v >> 18 & 63];
      line[p++] = b64[v >> 12 & 63];
      line[p++] = i + 1 < nb ? b64[v >> 6 & 63] : '=';
      line[p++] = i + 2 < nb ? b64[v & 63] : '=';
    }
    __asm__ volatile("" ::: "memory");
    cap_rd += n;
    log_line(line, p);
  }
  if (!cap_left && cap_wr == cap_rd) {
    __disable_irq();
    cap_on = false;
    __enable_irq();
    free(cap_ring); free(cap_sent); free(cap_trig);
    cap_ring = nullptr; cap_sent = nullptr; cap_trig = nullptr;
    logf(PSTR("cap: done, %lu frames, %lu lost"), (unsigned long)cap_frame, (unsigned long)cap_lost);
  }
}

// Keep the FIFO at host_target frames: render 12-frame blocks into the ring
// (as send_frames), hand them over.
void host_feed() {
  if (!rytm_ob.streaming()) return;
  int need = host_target - rytm_ob.fifoLevel();
  if (need <= 0) return;
  uint32_t t_in = micros();
  busy_frames += need;
  while (need > 0) {
    while (ring_n < need && ring_n <= kRing - kBlock) {
#if RYTM_PLUGINS
      plug_pool_refill((uint32_t)(need - ring_n) + 4 * kBlock, true);   // plugin pool readers too, the neediest first
#else
      refill_neediest((uint32_t)(need - ring_n) + 4 * kBlock);
#endif
      render_block();
    }
    int run = min(min(ring_n, need), kRing - ring_rd);
#if defined(USB_SERIAL_UAC2) && UAC2_CH_IN >= 20
    uac_voices_push(&ring[ring_rd], run);   // each voice's own signal, before out=main mixes the ring
#endif
    if (out_main) main_mix(&ring[ring_rd], run);   // run <= need <= the FIFO's room: all taken, none mixed twice
    int took = cap_on ? cap_push(&ring[ring_rd], run) : rytm_ob.pushVoices(&ring[ring_rd], run);
    ring_rd = (ring_rd + took) % kRing;
    ring_n -= took;
    need -= took;
    if (took < run) break;   // full (the target is above the FIFO)
  }
  uint32_t took = micros() - t_in;
  busy_us += took;
  if (took > max_request_us) max_request_us = took;
}

// The FIFO at its target: time for card work (loop()'s refills, saves). Without Overbridge audio
// the serial link's 'N' requests are the clock, as in the serial build: card work only while no
// input waits (else a prepare could run between a trig and the request read right after it).
bool host_waiting() { return rytm_ob.streaming() ? rytm_ob.fifoLevel() >= host_target : !Serial.available(); }

__attribute__((noinline)) FLASHMEM void host_poll() {
  usb.Task();
  if (rytm_ob.streaming() != host_was_streaming) {   // a new stream fades in again
    host_was_streaming = !host_was_streaming;
    if (host_was_streaming) main_out.Reset();
    core::link_reset(host_was_streaming ? "overbridge up" : "overbridge down");   // nothing hangs across the gap
#if defined(USB_SERIAL_UAC2)
    usb_uac2_ext_consumer = host_was_streaming;   // the Rytm's OUT drains the computer's ring; else the driver's sink
#endif
  }
  bool now = rytm_midi;
  if (now != midi_attached) {
    midi_attached = now;
    if (now) logf(PSTR("usb host: MIDI attached, %04x:%04x"), rytm_midi.idVendor(), rytm_midi.idProduct());
    else logf(PSTR("usb host: MIDI gone"));
    core::link_reset(now ? "midi attached" : "midi gone");   // a lost note-off / STOP: no voice left sounding
#if RYTM_PLUGINS
    if (!now) plug_transport = false;   // no Stop will come: a Rytm unplugged while playing must not block installs
#endif
  }
  // read() is false for SysEx continuation packets too: stop after a run of
  // them; the rest comes next pass (the parser keeps its place)
  for (int misses = 0, k = 0; misses < 64 && k < 1024; ++k) {
    if (!rytm_midi.read()) {
      ++misses;
      continue;
    }
    misses = 0;
    uint8_t type = rytm_midi.getType();
#if defined(USB_SERIAL_UAC2) && defined(MIDI_INTERFACE)
    mm_from_rytm(type);   // a copy to the computer (not our SysEx)
#endif
    if (type == MIDIDeviceBase::SystemExclusive) {
      const uint8_t* d = rytm_midi.getSysExArray();
      int len = rytm_midi.getSysExArrayLength();
      if (len < 6 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) continue;   // not ours
      ++midi_sysex_in;
      uint32_t tr = micros();
      report(d, len);
      tr = micros() - tr;
      if (tr > max_report_us) max_report_us = tr;
      if (tr > 2000) logf(PSTR("slow report %02x: %lu us"), d[4], (unsigned long)tr);
      host_feed();   // a slow report may have eaten into the FIFO
    } else if (type == MIDIDeviceBase::NoteOn || type == MIDIDeviceBase::NoteOff ||
               type == MIDIDeviceBase::ControlChange) {   // the stock Rytm's notes: voices with notes on
      midi_channel(type | ((rytm_midi.getChannel() - 1) & 0x0F), rytm_midi.getData1(), rytm_midi.getData2());
      if (notes_on) host_feed();
    } else if (type == MIDIDeviceBase::Clock) {
      ++midi_clocks;
      core::fx::clock_in(0xF8);   // the LFOs' sync (core/lfo.h)
    } else if (type == MIDIDeviceBase::SongPosition) {
      core::fx::clock_in(0xF2, (uint16_t)(rytm_midi.getData1() | rytm_midi.getData2() << 7));
    } else if (type == MIDIDeviceBase::Start) {
      core::fx::clock_in(0xFA);
      logf(PSTR("transport START"));
      rec_transport(0xFA);   // an armed recorder starts / ends a take
#if RYTM_PLUGINS
      plug_transport = true;   // no store installs while the Rytm plays
#endif
    } else if (type == MIDIDeviceBase::Continue) {
      core::fx::clock_in(0xFB);
      logf(PSTR("transport CONTINUE"));
      rec_transport(0xFB);   // an armed recorder starts / ends a take
#if RYTM_PLUGINS
      plug_transport = true;
#endif
    } else if (type == MIDIDeviceBase::Stop) {
      core::fx::clock_in(0xFC);
      logf(PSTR("transport STOP"));
      rec_transport(0xFC);   // an armed recorder starts / ends a take
#if RYTM_PLUGINS
      plug_transport = false;
#endif
    }
  }
#if defined(USB_SERIAL_UAC2) && defined(MIDI_INTERFACE)
  mm_from_mac();
#endif
#if defined(USB_SERIAL_UAC2)
  usbmode_poll();
#endif
  host_feed();
  if (cap_on) cap_drain();
}

#if defined(USB_SERIAL_UAC2)
FLASHMEM void host_mac(const char* v) { uac_mac_set(v); }
FLASHMEM void host_usbmode(const char* v) { usbmode_cmd(v); }
FLASHMEM void host_settings_changed() { host_settings_save(); }
#else
FLASHMEM void host_settings_changed() {}
FLASHMEM void host_usbmode(const char*) { logf(PSTR("usb: one mode in this build (Serial); the modes need RMP_CORE=uac2"));}
FLASHMEM void host_mac(const char*) { logf(PSTR("uac2: not in this build (RMP_CORE=uac2)")); }
#endif

// ':fifo=N' (console_line): the FIFO target, 24..RYTM_OB_FIFO - 32 frames; ':fifo' shows it.
FLASHMEM void host_fifo(const char* v) {
  if (*v) host_target = max(24, min(cap_num(v), RytmOverbridge::kFifo - 32));
  logf(PSTR("usb host: FIFO target %d frames"), host_target);
}

// The monitor keys (poll_input passes them): '+'/'-' the FIFO target x2 / /2, 's' the stats now.
// ('R', reboot, is poll_input's own.)
FLASHMEM void host_key(char c) {
  if (c == '+' || c == '-') {
    host_target = c == '+' ? host_target * 2 : host_target / 2;
    host_target = max(32, min(host_target, RytmOverbridge::kFifo - 32));
    logf(PSTR("usb host: FIFO target %d frames"), host_target);
  } else if (c == 's') {
    host_log_now = true;   // the next host_log() prints at once
  }
}

// Every 2 s with the cpu lines: the stream's rate (the Rytm's clock against
// the Teensy's), FIFO level and xruns, the MIDI side.
FLASHMEM void host_log() {
  uint32_t now = millis(), dt = now - host_log_ms;
  if (host_log_ms && dt < 2000 && !host_log_now) return;
  host_log_now = false;
  RytmOverbridge::Stats s;
  rytm_ob.stats(&s, true);
  if (s.fresh) memset(&host_prev, 0, sizeof host_prev);   // a new attach: counters from 0
  if (!dt) dt = 1;
  logf(PSTR("usb host: ob %s, in %lu Hz out %lu Hz, fifo %d (min %d max %d target %d), xruns %lu blocks %lu frames, "
       "bad %lu gaps %lu unanswered %lu rearms %lu/%lu queue fails %lu; midi %s, clock %lu BPM, sysex in %lu out %lu dropped %lu"),
       rytm_ob.streaming() ? "streaming" : rytm_ob.stateName(),
       (unsigned long)((s.in_frames - host_prev.in_frames) * 1000u / dt),
       (unsigned long)((s.out_frames - host_prev.out_frames) * 1000u / dt), s.fifo_now, s.fifo_min, s.fifo_max,
       host_target, (unsigned long)(s.xrun_blocks - host_prev.xrun_blocks),
       (unsigned long)(s.xrun_frames - host_prev.xrun_frames), (unsigned long)s.bad_blocks,
       (unsigned long)s.counter_gaps, (unsigned long)s.credit_drops, (unsigned long)s.in_rearms,
       (unsigned long)s.out_rearms, (unsigned long)s.queue_fails, midi_attached ? "on" : "off", (unsigned long)(midi_clocks * 60000u / 24 / dt),
       (unsigned long)midi_sysex_in, (unsigned long)midi_sysex_out, (unsigned long)midi_sysex_dropped);
  if (rytm_ob.state() == RytmOverbridge::kRefused) logf(PSTR("usb host: Overbridge refused: %s"), rytm_ob.refused());
  if (rytm_ob.streaming()) {   // the Rytm's IN peaks since the last line, x1000 of full scale (obhost_bringup's)
    int pk[12] = {0};
    for (int c = 0; c < 12 && c < rytm_ob.channelsIn() && c < ob::kChannels; ++c)
      pk[c] = (int)(s.peak[c] / 2147484);   // (32-bit: no 64-bit division helper in ITCM)
    logf(PSTR("usb host: peaks (x1000) main %d %d | BD SD RS BT LT MT CH CY %d %d %d %d %d %d %d %d | in %d %d"),
         pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], pk[6], pk[7], pk[8], pk[9], pk[10], pk[11]);
    if (out_main) logf(PSTR("usb host: out main, sent peak (x1000) %d"), (int)(main_out.TakePeak() * 1000.0f));
  }
#if defined(USB_SERIAL_UAC2)
  uac_log();
#endif
  midi_clocks = 0;
  host_prev = s;
  host_log_ms = now;
}
