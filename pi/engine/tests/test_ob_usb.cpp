// pi/engine/tests/test_ob_usb.cpp - the real ob_usb.cpp on a fake Circle (shim_ob/): the descriptors a Digitakt and a Rytm send, the
// sequence on the control endpoint, the URBs' sizes, OUT paced by IN whatever the two sizes are, the bytes of an OUT block. The
// descriptors are what the firmware sent an emulated host (rytm-emu dtusb 692c4cc, dtboot --usb-host: Digitakt OS 1.53 and 1.54,
// OVERBRIDGE; Rytm OS 1.73): the Digitakt's matches a physical capture byte for byte (docs/device-profiles.md, "Audio").
#include "check.h"
#include "device/digitakt.h"
#include "fx/out.h"
#include "ob_block.h"
#include "ob_usb.h"
#include "ob_follow.h"
#include "shared.h"

#include <circle/koptions.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

CLogger* CLogger::Get() { static CLogger l; return &l; }
void CLogger::Write(const char*, int, const char* f, ...) {
  char b[512];
  va_list a;
  va_start(a, f);
  vsnprintf(b, sizeof b, f, a);
  va_end(a);
  lines.push_back(b);
}
#include "device/a4.h"
namespace core { namespace device {
static bool g_a4_chosen = false;      // its hello chose it (device.cpp: audio_by_hello)
const Profile& active() {             // (only ob_follow.cpp asks)
  static Profile none;
  return g_a4_chosen ? kA4 : none;
}
const Profile* by_usb(uint16_t vid, uint16_t pid) {   // device.cpp's list, without the engine it links
  static Profile rytm;
  rytm.audio = &out::kRytmMk2;
  if (vid == 0x1935 && pid == 0x0b48) return &rytm;
  if (vid == kA4Vid && pid == kA4Pid) return g_a4_chosen ? &kA4 : nullptr;
  return vid == kDigitakt.usb_vid && pid == kDigitakt.usb_pid ? &kDigitakt : nullptr;
}
}}
static int g_rytm_usb = 0;
namespace core { namespace device { void rytm_on_usb(bool on) { g_rytm_usb += on ? 1 : -1; } }}
CUSBFunction* RytmObDeviceCreate(CUSBFunction* p);
extern "C" int core_snprintf(char* b, unsigned long n, const char* f, ...) {
  va_list a;
  va_start(a, f);
  int r = vsnprintf(b, n, f, a);
  va_end(a);
  return r;
}

// The Rytm's, the same as teensy/libraries/RytmHost/extras/ob_block_test.cpp's; the Digitakt's differs in OUT alt 3's size alone.
static uint8_t g_rytm[263] = {
    0x09, 0x02, 0x07, 0x01, 0x06, 0x01, 0x00, 0xc0, 0x00, 0x08, 0x0b, 0x00, 0x04, 0xff, 0x00, 0x00, 0x00, 0x09,
    0x04, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x09, 0x04, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x09,
    0x04, 0x01, 0x01, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x01, 0x09, 0x04, 0x01,
    0x02, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x58, 0x00, 0x01, 0x09, 0x04, 0x01, 0x03, 0x01,
    0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x70, 0x01, 0x01, 0x09, 0x04, 0x02, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x09, 0x04, 0x02, 0x01, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x01,
    0x09, 0x04, 0x02, 0x02, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x58, 0x00, 0x01, 0x09, 0x04,
    0x02, 0x03, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x70, 0x01, 0x01, 0x09, 0x04, 0x03, 0x00,
    0x02, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00, 0x07, 0x05, 0x82, 0x03, 0x40, 0x00,
    0x01, 0x08, 0x0b, 0x04, 0x02, 0x01, 0x01, 0x00, 0x00, 0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x05, 0x09, 0x04, 0x05, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00, 0x06, 0x24, 0x02, 0x01, 0x01, 0x00, 0x06, 0x24, 0x02, 0x02, 0x02,
    0x00, 0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00, 0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01,
    0x00, 0x09, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x01, 0x09, 0x05, 0x81,
    0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x03};
static uint8_t g_digitakt[263];
static const int kOutAlt3Mps = 137;   // 0x70 0x01 -> 0x58 0x00
// The A4 MKII's differs from the Rytm's in both alt 3 sizes (OS 1.55D's table and 1.56's over an emulated USB host, byte for byte)
static uint8_t g_a4[263];
static const int kInAlt3Mps = 80;     // 0x70 0x01 -> 0x00 0x01

struct Bus {
  CUSBDevice dev;
  CUSBHostController host;
  CUSBFunction parent{&dev, &host};
  CUSBFunction* ob = nullptr;
  Bus(const uint8_t* cfg, uint16_t pid) {
    dev.config = cfg;
    dev.desc = {0x1935, pid};
    ob = RytmObDeviceCreate(&parent);
  }
  ~Bus() { delete ob; }
  // n IN blocks arrive, one URB each, as the device sends them; the next counter is the caller's
  void in(int n, uint16_t& ctr) {
    for (int k = 0; k < n; ++k) {
      CUSBRequest* u = host.in.front();
      host.in.pop_front();
      memset(u->buf, 0, 368);
      u->buf[0] = 0x07, u->buf[2] = ctr >> 8, u->buf[3] = (uint8_t)ctr;
      ctr += 7;
      u->SetStatus(1), u->SetResultLen(368);
      u->fn(u, u->param, u->ctx);
    }
  }
  // the A4's: a block of 256 B, a tone on MAIN L and its negative on MAIN R, the other six channels at full scale so a wrong channel shows
  void in_a4(int n, uint16_t& ctr, long& at, double hz) {
    for (int k = 0; k < n; ++k) {
      CUSBRequest* u = host.in.front();
      host.in.pop_front();
      memset(u->buf, 0, 256);
      u->buf[0] = 0x07, u->buf[2] = ctr >> 8, u->buf[3] = (uint8_t)ctr;
      ctr += 7;
      for (int f = 0; f < 7; ++f, ++at) {
        int32_t x = (int32_t)(0.5 * 2147483647.0 * sin(2 * M_PI * hz * (double)at / 48000.0));
        for (int c = 0; c < 8; ++c) ob::put_be32(u->buf + 32 + 4 * (8 * f + c), c == 0 ? x : c == 1 ? -x : 0x7FFFFFFF);
      }
      u->SetStatus(1), u->SetResultLen(256);
      u->fn(u, u->param, u->ctx);
    }
  }
  void out_done() {
    CUSBRequest* u = host.out.front();
    host.out.pop_front();
    u->SetStatus(1), u->SetResultLen(u->len);
    u->fn(u, u->param, u->ctx);
  }
};

static void same_sequence(const CUSBHostController& h) {
  const u16 want[4][2] = {{0, 1}, {0, 2}, {3, 1}, {3, 2}};   // alt 0 on 1 and 2, then alt 3
  CHECK(h.ctl.size() == 4);
  for (int k = 0; k < 4; ++k)
    CHECK(h.ctl[k].req == SET_INTERFACE && h.ctl[k].value == want[k][0] && h.ctl[k].index == want[k][1]);
}

// the driver holds its URBs and endpoints for the life of the device; on the Pi nothing frees them
extern "C" const char *__asan_default_options () { return "detect_leaks=0"; }

int main() {
  memcpy(g_digitakt, g_rytm, sizeof g_rytm);
  CHECK(g_rytm[kOutAlt3Mps] == 0x70 && g_rytm[kOutAlt3Mps + 1] == 0x01);
  g_digitakt[kOutAlt3Mps] = 0x58, g_digitakt[kOutAlt3Mps + 1] = 0x00;

  {  // the parser of both
    ob::Layout r, d;
    CHECK(ob::find_layout(g_rytm, sizeof g_rytm, ob::kAlt, &r) && r.in_channels == 12 && r.out_channels == 12);
    CHECK(ob::find_layout(g_digitakt, sizeof g_digitakt, ob::kAlt, &d));
    CHECK(d.in.mps == 368 && d.out.mps == 88 && d.in_channels == 12 && d.out_channels == 2 && d.midi_iface == 5);
    CHECK(ob::block_bytes(core::device::kDigitakt.audio->channels) == 88);
    CHECK(core::device::kDigitakt.audio->in_channels == 12);
    CHECK(core::device::kDigitakt.bracket == core::device::kBracketMix && core::device::kDigitakt.audio->ret == core::out::kStereoMix);
  }

  {  // the Rytm: 368 both ways, 12 channels, the old log line, the table untouched
    Bus b(g_rytm, 0x0b48);
    CHECK(b.ob && b.ob->Configure());
    CHECK(g_rytm_usb == 1);
    {  // a second box while one streams: no second stream, and its id is still noted when it is a Rytm
      Bus s(g_digitakt, 0x0b2c);
      CHECK(s.ob == nullptr && g_rytm_usb == 1);
    }
    same_sequence(b.host);
    CHECK(b.host.in.size() == 8 && b.host.in.front()->len == 368);
    CHECK(&core::out::device() == &core::out::kRytmMk2);
    CHECK(CLogger::Get()->lines[0] == "Analog Rytm MKII (1935:0b48) claimed");
    g_Output.SetMode(Output::kTracks);
    int16_t fr[32][8] = {};
    for (auto& f : fr) f[2] = 8000;
    g_Output.Push(fr, 32);
    uint16_t c = 0;
    b.in(1, c);
    CHECK(b.host.out.size() == 1 && b.host.out[0]->len == 368);
    // a voice on its track's channel and nowhere else, as ever
    const uint8_t* o = b.host.out[0]->buf;
    for (int ch = 0; ch < 12; ++ch) CHECK((ob::block_sample(o, 12, 6, ch) != 0) == (ch == ob::kVoiceToRytm[2]));
    g_Output.SetMode(Output::kMain);
  }

  CHECK(g_rytm_usb == 0);   // the Rytm gone
  {  // a Digitakt's descriptor under the Rytm's id: refused, as a Rytm with a wrong descriptor always was
    Bus b(g_digitakt, 0x0b48);
    CHECK(b.ob && !b.ob->Configure());
  }
  {  // USB MIDI mode: not this driver's, Circle goes on to the interfaces
    Bus b(g_rytm, 0x000d);
    CHECK(b.ob == nullptr);
    b.ob = nullptr;
  }

  {  // the Digitakt
    Bus b(g_digitakt, 0x0b2c);
    CHECK(b.ob && b.ob->Configure());
    CHECK(g_rytm_usb == 0);   // a Digitakt's id asks for nothing: its hello does
    same_sequence(b.host);
    CHECK(b.host.in.size() == 8 && b.host.in.front()->len == 368);
    CHECK(&core::out::device() == core::device::kDigitakt.audio);
    for (int v = 0; v < 8; ++v) CHECK(core::out::control.route[v] == core::out::kPost);
    CHECK(g_Output.GetMode() == Output::kMain);   // whatever was set: no voice channels

    // voices in, one OUT block per IN block: 88 B, header, counter + 7, two channels
    int16_t fr[64][8] = {};
    for (auto& f : fr) f[0] = 8000, f[7] = 8000;
    g_Output.Push(fr, 64);
    g_Output.Push(fr, 64);
    uint16_t c = 0;
    b.in(1, c);
    CHECK(b.host.out.size() == 1 && b.host.out[0]->len == 88);
    const uint8_t* o = b.host.out[0]->buf;
    CHECK(o[0] == 0x07 && o[1] == 0xFF && o[2] == 0 && o[3] == 0);
    b.out_done();
    b.in(1, c);
    o = b.host.out[0]->buf;
    CHECK(o[3] == 7);

    // eight URBs in flight, the device stops taking them: credit is kept, none is lost, nothing is sent short
    b.out_done();
    b.in(8, c);
    CHECK(b.host.out.size() == 8);
    b.in(4, c);
    CHECK(b.host.out.size() == 8);   // four owed
    for (int k = 0; k < 4; ++k) b.out_done();
    CHECK(b.host.out.size() == 8);   // the four owed went at once
    for (int k = 0; k < 8; ++k) {
      CHECK(b.host.out[k]->len == 88);
      CHECK(((b.host.out[k]->buf[2] << 8 | b.host.out[k]->buf[3]) % 7) == 0);
    }
    uint16_t last = b.host.out[7]->buf[2] << 8 | b.host.out[7]->buf[3];
    CHECK(last == 7 * 13);   // blocks 0..13, one per IN, in order
    // the voices' sum on both channels, L = R (TRK-less: POST, centred), and the block ends at 88 B: frames 7 x 2 x 4 after 32
    const uint8_t* l = b.host.out[7]->buf;
    CHECK(ob::block_sample(l, 2, 6, 0) != 0 && ob::block_sample(l, 2, 6, 0) == ob::block_sample(l, 2, 6, 1));
    CHECK(g_Output.Underflows() == 0);
  }
  CHECK(&core::out::device() == &core::out::kRytmMk2);   // the box gone: the Rytm's table again

  // The A4 beside the Rytm
  memcpy(g_a4, g_rytm, sizeof g_rytm);
  CHECK(g_a4[kInAlt3Mps] == 0x70 && g_a4[kInAlt3Mps + 1] == 0x01);
  g_a4[kInAlt3Mps] = 0x00, g_a4[kInAlt3Mps + 1] = 0x01;
  g_a4[kOutAlt3Mps] = 0xc8, g_a4[kOutAlt3Mps + 1] = 0x00;
  {
    ob::Layout a;
    CHECK(ob::find_layout(g_a4, sizeof g_a4, ob::kAlt, &a) && a.in_channels == 8 && a.out_channels == 6 && a.midi_iface == 5);
  }
  CKernelOptions* opt = CKernelOptions::Get();
  {  // a4=0, the default: not claimed, Circle goes on to its interfaces
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob == nullptr);
    a.ob = nullptr;
  }
  core::device::g_a4_chosen = true;
  {  // the A4 as the box in use: CObUsb, 256 / 200, each voice on its own channel, IN widened to 12
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob && a.ob->Configure());
    same_sequence(a.host);
    CHECK(CObUsb::Streaming() && !CObFollow::Streaming());
    CHECK(a.host.in.size() == 8 && a.host.in.front()->len == 256);
    CHECK(&core::out::device() == core::device::kA4.audio);
    CHECK(g_rytm_usb == 0);
    int16_t fr[64][8] = {};
    for (auto& f : fr)
      for (int v = 0; v < 8; ++v) f[v] = (int16_t)(1000 * (v + 1));
    g_Output.SetMode(Output::kTracks);
    g_Output.Push(fr, 64);
    g_Output.Push(fr, 64);
    uint16_t c = 0;
    long at = 0;
    a.in_a4(1, c, at, 1000.0);
    CHECK(a.host.out.size() == 1 && a.host.out[0]->len == 200);
    const uint8_t* o = a.host.out[0]->buf;
    a.out_done();
    for (int k = 0; k < 12; ++k) {         // past the fade-in
      a.in_a4(1, c, at, 1000.0);
      o = a.host.out.front()->buf;
      if (k == 11)
        for (int ch = 0; ch < 4; ++ch) CHECK(ob::block_sample(o, 6, 6, 2 + ch) != 0);
      CHECK(ob::block_sample(o, 6, 6, 0) == 0 && ob::block_sample(o, 6, 6, 1) == 0);
      a.out_done();
    }
    CHECK(g_Output.Underflows() == 0);
    uint8_t in[256], w[368];
    memset(in, 0, 256);
    for (int f = 0; f < 7; ++f)
      for (int ch = 0; ch < 8; ++ch) ob::put_be32(in + 32 + 4 * (8 * f + ch), (f + 1) * 16 + ch);
    memset(w, 0xAA, sizeof w);
    CObUsb::WidenIn(in, w, 8);
    for (int f = 0; f < 7; ++f) {
      for (int ch = 0; ch < 6; ++ch) CHECK(ob::block_sample(w, 12, f, ch) == (f + 1) * 16 + ch);
      for (int ch = 6; ch < 10; ++ch) CHECK(ob::block_sample(w, 12, f, ch) == 0);
      CHECK(ob::block_sample(w, 12, f, 10) == (f + 1) * 16 + 6 && ob::block_sample(w, 12, f, 11) == (f + 1) * 16 + 7);
    }
  }
  CHECK(&core::out::device() == &core::out::kRytmMk2);
  core::device::g_a4_chosen = false;
  {  // the Rytm alone with the A4's code in the build: OUT as ever, PRE silent
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    for (int f = 0; f < 7; ++f) CHECK(ob::block_sample(r.host.out[0]->buf, 12, f, 2) == 0 && ob::block_sample(r.host.out[0]->buf, 12, f, 3) == 0);
  }
  opt->num["a4"] = 1;
  opt->num["a4att"] = 0;
  CXHCIEndpoint::s_valid = false;
  {  // the controller refuses the second stream: no stream, nothing left running
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob && !a.ob->Configure());
    CHECK(a.host.in.empty() && !CObFollow::Streaming());
  }
  CXHCIEndpoint::s_valid = true;
  {  // a wrong descriptor under the A4's id
    Bus a(g_rytm, 0x0b47);
    CHECK(a.ob && !a.ob->Configure());
  }
  {
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    Bus a(g_a4, 0x0b47);                  // one driver each, on one bus each here
    CHECK(a.ob && a.ob->Configure());
    CHECK(CObUsb::Streaming() && CObFollow::Streaming());
    CHECK(&core::out::device() == &core::out::kRytmMk2);   // the A4 changes nothing of the Rytm's
    {
      Bus b(g_a4, 0x0b47);                // a second A4: no
      CHECK(b.ob == nullptr);
      b.ob = nullptr;
    }
    same_sequence(a.host);
    CHECK(a.host.in.size() == 8 && a.host.in.front()->len == 256);
    uint16_t cr = 0, ca = 0;
    long at = 0;
    double peak_l = 0, peak_r = 0, other = 0;
    bool antiphase = true;
    for (int b = 0; b < 8000; ++b) {
      a.in_a4(1, ca, at, 1000.0);
      CHECK(a.host.out.size() == 1 && a.host.out[0]->len == 200);   // its OUT: one silent block for each IN block
      const uint8_t* o = a.host.out[0]->buf;
      CHECK(o[0] == 0x07 && o[1] == 0xFF && (uint16_t)(o[2] << 8 | o[3]) == (uint16_t)(ca - 7));
      for (int ch = 0; ch < 6; ++ch) CHECK(ob::block_sample(o, 6, 6, ch) == 0);
      a.out_done();
      r.in(1, cr);
      const uint8_t* q = r.host.out.back()->buf;
      for (int f = 0; f < 7; ++f) {
        int32_t l = ob::block_sample(q, 12, f, 2), rr = ob::block_sample(q, 12, f, 3);
        peak_l = fmax(peak_l, fabs((double)l)), peak_r = fmax(peak_r, fabs((double)rr));
        antiphase = antiphase && fabs((double)l + (double)rr) < 1e5;
        other = fmax(other, fabs((double)ob::block_sample(q, 12, f, 0)));   // POST stays the Rytm's
      }
      r.out_done();
    }
    CHECK(fabs(peak_l / 2147483648.0 - 0.5) < 0.01 && fabs(peak_r / 2147483648.0 - 0.5) < 0.01);   // unity at a4att=0, the tone on PRE L and R
    CHECK(antiphase);
    CHECK(other < 3e7);                   // what the voices left there, not the A4
    CLogger::Get()->lines.clear();
    CObFollow::LogStats(CLogger::Get());
    uint16_t c2 = cr;
    delete a.ob;                          // the A4 goes: the Rytm's stream goes on, PRE fades out
    a.ob = nullptr;
    CHECK(!CObFollow::Streaming() && CObUsb::Streaming());
    for (int b = 0; b < 200; ++b) {
      r.in(1, c2);
      r.out_done();
    }
    r.in(1, c2);
    const uint8_t* q = r.host.out.back()->buf;
    for (int f = 0; f < 7; ++f) CHECK(ob::block_sample(q, 12, f, 2) == 0);
  }
  opt->num.clear();

  printf("test_ob_usb: ok\n");
  return 0;
}
