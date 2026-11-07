// pi/engine/tests/test_ob_mirror.cpp - usb=mirror's box side in the real ob_usb.cpp (CIRCLE_GADGET_MIRROR) on the fake Circle of shim_ob/
// (a recording gadget, sidecar_gadget.h): the pipes opened at attach, a halted pipe cleared and resubmitted by
// MirrorPipePoll and its backoff, MirrorPipePoll leaving EP0 alone while a forwarded request is on it, a forwarded
// request's answer, and the attach generation that drops a completion of an earlier attach. The configuration is the
// Rytm's that test_ob_usb.cpp uses (the emulator's; the pipes on interface 3, USB MIDI on 4-5).
#include "check.h"
#include "device/digitakt.h"
#include "device/audio.h"
#include "audio_return.h"
#include "ob_block.h"
#include "ob_usb.h"
#include "shared.h"
#include "sidecar_gadget.h"
#include <circle/timer.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

CLogger* CLogger::Get() { static CLogger l; return &l; }
void CLogger::Write(const char*, int, const char* f, ...) {
  char b[512];
  va_list a;
  va_start(a, f);
  vsnprintf(b, sizeof b, f, a);
  va_end(a);
  lines.push_back(b);
}
namespace core { namespace device {
const Profile* by_usb(uint16_t vid, uint16_t pid) {   // device.cpp's list, without the engine it links
  static Profile rytm;
  rytm.audio = &out::kRytmMk2;
  if (vid == 0x1935 && pid == 0x0b48) return &rytm;
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

// the driver holds its URBs and endpoints for the life of the device; on the Pi nothing frees them
extern "C" const char *__asan_default_options () { return "detect_leaks=0"; }

static CSidecarGadget g_gadget;

struct Box {
  CUSBDevice dev;
  CUSBHostController host;
  CUSBFunction parent{&dev, &host};
  CObUsb* ob = nullptr;
  Box() {
    dev.config = g_rytm;
    dev.desc.idVendor = 0x1935, dev.desc.idProduct = 0x0b48;
    ob = (CObUsb*)RytmObDeviceCreate(&parent);
    CHECK(ob && ob->Configure());
  }
  ~Box() { delete ob; }
  // the pipe's URB in flight (IN: one at a time), or null
  CUSBRequest* pipe(uint8_t addr) {
    auto& q = addr & 0x80 ? host.in : host.out;
    for (CUSBRequest* u : q) if (u->ep && u->ep->desc && u->ep->desc->bEndpointAddress == addr) return u;
    return nullptr;
  }
  // the box answers it (ok: a packet of n bytes; else the transfer failed: the endpoint halted), as the xHCI IRQ would
  void complete(CUSBRequest* u, bool ok, unsigned n = 0) {
    for (auto* q : {&host.in, &host.out, &host.ep0})
      for (auto i = q->begin(); i != q->end(); ++i) if (*i == u) { q->erase(i); break; }
    u->SetStatus(ok ? 1 : 0), u->SetResultLen(ok ? n : 0);
    u->fn(u, u->param, u->ctx);
  }
  int clears(uint8_t addr) {
    int n = 0;
    for (const TCtl& c : host.ctl)
      n += c.type == (REQUEST_OUT | REQUEST_TO_ENDPOINT) && c.req == CLEAR_FEATURE && c.value == ENDPOINT_HALT && c.index == addr;
    return n;
  }
};

static void poll_at(unsigned ticks) { CTimer::s_ticks = ticks; CObUsb::MirrorPipePoll(); }
static bool logged(const char* s) {
  for (const std::string& l : CLogger::Get()->lines) if (l.find(s) != std::string::npos) return true;
  return false;
}

int main() {
  CSidecarGadget::s_pThis = &g_gadget;
  CTimer::s_ticks = 1000;

  {  // attach: the pipes of interface 3 (bulk OUT 0x02, interrupt IN 0x82), nothing on the audio interfaces' alternates
     // and no SET_INTERFACE beyond the stream's four (interface 3 has only alt 0); MIDI is not a pipe
    Box b;
    CHECK(g_gadget.box.size() == 1 && g_gadget.box.back() == 1);
    CHECK(b.host.ctl.size() == 4);
    CUSBRequest* in = b.pipe(0x82);
    CHECK(in && in->len == 64 && b.pipe(0x81) == nullptr);
    CHECK(logged("usb=mirror: 0 strings (language 0409), 2 pipes"));

    // a packet of the box: to the gadget, the next IN request at once (the gadget did not keep it)
    b.complete(in, true, 12);
    CHECK(g_gadget.pipe_in.size() == 1 && g_gadget.pipe_in[0].addr == 0x82 && g_gadget.pipe_in[0].n == 12);
    CHECK(b.pipe(0x82) == in);

    // the computer's pipe OUT packet: one at a time, the gadget told when the box has it
    static const u8 pkt[10] = {1, 2, 3};
    CHECK(CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    CUSBRequest* out = b.pipe(0x02);
    CHECK(out && out->len == 10 && memcmp(out->buf, pkt, 3) == 0);
    CHECK(!CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));   // busy: the gadget keeps it
    b.complete(out, true, 10);
    CHECK(g_gadget.pipe_out_done.size() == 1 && g_gadget.pipe_out_done[0] == 0x02);

    // MirrorPipePoll with nothing halted: nothing on EP0
    poll_at(2000);
    CHECK(b.clears(0x82) == 0 && CObUsb::MirrorPipeResets() == 0);

    // the IN pipe halts (the box STALLed it): not resubmitted until MirrorPipePoll has cleared it on the box and on the
    // xHC (Reset Endpoint, as the endpoint context is Halted); then running again, logged once
    in = b.pipe(0x82);
    in->ep->GetXHCIEndpoint()->state = 2;
    b.complete(in, false);
    CHECK(b.pipe(0x82) == nullptr && g_gadget.pipe_in.size() == 1);
    poll_at(2000);
    CHECK(b.clears(0x82) == 1 && in->ep->GetXHCIEndpoint()->resets == 1 && in->ep->GetXHCIEndpoint()->state == 1);
    CHECK(b.pipe(0x82) != nullptr && CObUsb::MirrorPipeResets() == 1);
    CHECK(logged("usb=mirror: pipe 0x82 halted: cleared, running again"));

    // an OUT pipe halts: the computer's packets are refused (the gadget drops them) until it is cleared; not resubmitted
    // (the computer's next packet is the next transfer)
    CHECK(CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    out = b.pipe(0x02);
    b.complete(out, false);
    CHECK(g_gadget.pipe_out_done.size() == 2);   // the gadget still takes the computer's next one
    CHECK(!CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    poll_at(2100);
    CHECK(b.clears(0x02) == 1 && b.pipe(0x02) == nullptr);
    CHECK(CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    b.complete(b.pipe(0x02), true, 10);

    // a box that keeps failing: every 100 ms (HZ / 10) for the first five tries, then once a second (a good packet
    // first: the count of tries starts again)
    b.complete(b.pipe(0x82), true, 1);
    in = b.pipe(0x82);
    CXHCIEndpoint* x = in->ep->GetXHCIEndpoint();
    x->state = 2, x->reset_ok = false;            // Reset Endpoint fails: the pipe stays stopped
    b.complete(in, false);
    unsigned t = 3000;
    poll_at(t);
    CHECK(b.clears(0x82) == 2);
    for (int k = 2; k <= 5; ++k) {
      poll_at(t + 9);                             // too soon
      CHECK(b.clears(0x82) == k);
      t += 10;
      poll_at(t);
      CHECK(b.clears(0x82) == k + 1);
    }
    poll_at(t + 10);                              // five tries in: 100 ms is no longer enough
    poll_at(t + 99);
    CHECK(b.clears(0x82) == 6);
    t += 100;
    poll_at(t);
    CHECK(b.clears(0x82) == 7 && b.pipe(0x82) == nullptr);

    // a forwarded request holds EP0: MirrorPipePoll waits for its answer before it clears anything
    x->reset_ok = true;
    TSetupData s = {0xC0, 0x01, 0, 0, 16};        // a vendor IN request
    CHECK(CObUsb::MirrorControl(&s, nullptr, 41));
    CHECK(b.host.ep0.size() == 1 && b.host.ep0[0]->len == 16 && b.host.ep0[0]->GetSetupData()->bRequest == 0x01);
    CHECK(!CObUsb::MirrorControl(&s, nullptr, 42));   // one at a time
    t += 100;
    poll_at(t);
    CHECK(b.clears(0x82) == 7);
    b.complete(b.host.ep0[0], true, 5);
    CHECK(g_gadget.done.size() == 1 && g_gadget.done[0].seq == 41 && g_gadget.done[0].n == 5);
    poll_at(t);
    CHECK(b.clears(0x82) == 8 && b.pipe(0x82) != nullptr);

    // the box STALLs a forwarded request: the gadget is told (< 0), and STALLs it to the computer
    CHECK(CObUsb::MirrorControl(&s, nullptr, 43));
    b.complete(b.host.ep0[0], false);
    CHECK(g_gadget.done.size() == 2 && g_gadget.done[1].seq == 43 && g_gadget.done[1].n == -1);
    TSetupData z = {0xC0, 0x01, 0, 0, 0};          // IN with no data stage: never forwarded
    CHECK(!CObUsb::MirrorControl(&z, nullptr, 44) && b.host.ep0.empty());

    // the box goes away with a pipe IN request and a forwarded request in flight (they never complete on the Pi; here
    // they are kept to be completed later), and comes back as a new attach
    in = b.pipe(0x82);
    CHECK(CObUsb::MirrorControl(&s, nullptr, 45));
    CUSBRequest* ctl = b.host.ep0[0];
    void* old_ctx = in->ctx;
    b.host.in.erase(std::find(b.host.in.begin(), b.host.in.end(), in));
    b.host.ep0.clear();
    delete b.ob;
    b.ob = nullptr;
    CHECK(g_gadget.box.size() == 2 && g_gadget.box.back() == 0);
    CHECK(CObUsb::MirrorPipeResets() == 9);   // every clear sent, both pipes (the mirror line's "reset")

    Box c;                                       // the box back: a new CObUsb, a new generation
    CHECK(g_gadget.box.back() == 1);
    CUSBRequest* nin = c.pipe(0x82);
    CHECK(nin);
    (void) old_ctx;
    // the old attach's completions, with the new object as their context, as if it were at the old one's address (the
    // Pi's heap can do that): ignored (no packet to
    // the gadget, nothing resubmitted, the new pipe untouched)
    size_t nPkt = g_gadget.pipe_in.size(), nIn = c.host.in.size(), nDone = g_gadget.done.size();
    in->ctx = nin->ctx, ctl->ctx = nin->ctx;
    in->SetStatus(1), in->SetResultLen(7);
    in->fn(in, in->param, in->ctx);
    CHECK(g_gadget.pipe_in.size() == nPkt && c.host.in.size() == nIn && c.pipe(0x82) == nin);
    in->SetStatus(0);                            // nor does an error mark the new pipe halted
    in->fn(in, in->param, in->ctx);
    poll_at(t + 1000);
    CHECK(c.clears(0x82) == 0);
    ctl->SetStatus(1), ctl->SetResultLen(4);
    ctl->fn(ctl, ctl->param, ctl->ctx);          // the old request's answer: not passed on (deletes the URB)
    CHECK(g_gadget.done.size() == nDone);
    CHECK(CObUsb::MirrorControl(&s, nullptr, 46));   // and EP0 of the new attach is free
    c.complete(c.host.ep0[0], true, 3);
    CHECK(g_gadget.done.size() == nDone + 1 && g_gadget.done.back().seq == 46);
    // the new attach's own completion still goes through
    c.complete(nin, true, 9);
    CHECK(g_gadget.pipe_in.size() == nPkt + 1 && g_gadget.pipe_in.back().n == 9);
    delete in;
  }

  {  // usb=mirror off (the default): a box is streamed as ever, no pipes, nothing told to the gadget
    g_gadget.mirror = false;
    size_t nBox = g_gadget.box.size();
    Box b;
    CHECK(b.pipe(0x82) == nullptr && b.host.ctl.size() == 4 && g_gadget.box.size() == nBox);
    TSetupData s = {0xC0, 0x01, 0, 0, 16};
    CHECK(!CObUsb::MirrorControl(&s, nullptr, 1));
  }

  puts("test_ob_mirror: ok");
  return 0;
}
