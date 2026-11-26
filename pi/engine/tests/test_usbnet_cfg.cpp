// pi/engine/tests/test_usbnet_cfg.cpp - usbnet_cfg.h: the USB-C network's cmdline.txt keys, the boot's choice (and
// that a Sidecar with no computer on USB-C keeps today's network), the trust rule, the MACs.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "usbnet_cfg.h"

using namespace usbnet;

static Probe run (BootProbe &p, unsigned nSofAt, unsigned nUpAt, unsigned *pAt)
{
	for (unsigned t = 0; t <= 60000; t += 10)
	{
		Probe r = p.step (t, t >= nSofAt, t >= nUpAt);
		if (r != kWaiting) { *pAt = t; return r; }
	}
	return kWaiting;
}

int main ()
{
	// ---- keys
	Net n = kAuto;
	CHECK (parse_net ("usb", &n) && n == kUsb);
	CHECK (parse_net ("wifi", &n) && n == kWifi);
	CHECK (parse_net ("lan", &n) && n == kLan && parse_net ("ethernet", &n) && n == kLan && parse_net ("eth", &n) && n == kLan);
	CHECK (parse_net ("auto", &n) && n == kAuto);
	CHECK (!parse_net ("USB", &n) && !parse_net ("", &n) && !parse_net (0, &n) && n == kAuto);
	CHECK (!strcmp (net_name (kUsb), "usb") && !strcmp (net_name (kLan), "lan") && !strcmp (net_name (kAuto), "auto"));
	CHECK (parse_onoff ("1", false) && parse_onoff ("on", false) && !parse_onoff ("0", true) && !parse_onoff ("off", true));
	CHECK (parse_onoff (0, true) && !parse_onoff (0, false) && parse_onoff ("yes", true) && !parse_onoff ("yes", false));
	uint8_t ip[4] = {9, 9, 9, 9};
	CHECK (parse_ip ("172.31.77.1", ip) && ip[0] == 172 && ip[1] == 31 && ip[2] == 77 && ip[3] == 1);
	CHECK (parse_ip ("10.0.0.254", ip) && ip[3] == 254);
	const char *bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.0", "1.2.3.255", "0.1.2.3", "127.0.0.1", "224.0.0.1",
			     "255.255.255.255", "1.2.3.4 ", "1..3.4", "01234.1.1.1", "a.b.c.d", "1.2.3.-4"};
	for (const char *b : bad) { uint8_t t[4] = {7, 7, 7, 7}; CHECK (!parse_ip (b, t) && t[0] == 7); }
	uint8_t c[4];
	client_ip (kDefaultIp, c);
	CHECK (c[0] == 172 && c[1] == 31 && c[2] == 77 && c[3] == 2);
	const uint8_t two[4] = {10, 1, 2, 2};
	client_ip (two, c);
	CHECK (c[3] == 1 && c[2] == 2);

	// defaults: everything absent
	Options o = options (0, 0, 0, 0, 0, 0);
	CHECK (o.net == kAuto && o.usbnet && o.trust && !memcmp (o.ip, kDefaultIp, 4) && o.wait_s == 5 && o.quiet_ms == 700);
	CHECK (!o.bad_net && !o.bad_ip);
	o = options ("lan", "off", "10.9.8.1", "12", "300", "0");
	CHECK (o.net == kLan && !o.usbnet && !o.trust && o.ip[0] == 10 && o.ip[3] == 1 && o.wait_s == 12 && o.quiet_ms == 300);
	o = options ("bogus", "maybe", "300.1.1.1", "999", "1", "x");	// not understood: the defaults, said so
	CHECK (o.net == kAuto && o.bad_net && o.usbnet && o.bad_ip && !memcmp (o.ip, kDefaultIp, 4) && o.wait_s == 30 && o.quiet_ms == 100 && o.trust);
	o = options (0, 0, 0, "0", "x9", 0);
	CHECK (o.wait_s == 1 && o.quiet_ms == 700);

	// ---- the plan
	const char *why = 0;
	Inputs in;
	in.function = true;
	CHECK (plan (in, &why) == kProbe && why);
	in.net = kWifi;
	CHECK (plan (in, &why) == kToday && !strcmp (why, "net=wifi"));
	in.net = kLan;
	CHECK (plan (in, &why) == kToday);
	in.net = kUsb;
	CHECK (plan (in, &why) == kForced);
	in.wifi_trial = true;
	CHECK (plan (in, &why) == kForced);			// asked for: USB even then (the trial reverts, docs say so)
	in.net = kAuto;
	CHECK (plan (in, &why) == kToday);			// a Wi-Fi join being tried wins over the cable
	in.wifi_trial = false;
	in.link_tcp = true;
	CHECK (plan (in, &why) == kToday);
	in.link_tcp = false;
	in.function = false;					// usbnet=off, OVERBRIDGE, no gadget: never USB, even net=usb
	CHECK (plan (in, &why) == kToday);
	in.net = kUsb;
	CHECK (plan (in, &why) == kToday);

	// ---- the probe: no computer -> today's network after the quiet window
	unsigned at = 0;
	{
		BootProbe p (700, 5);
		CHECK (run (p, ~0u, ~0u, &at) == kNoHost && at == 700 && !p.host_seen ());
	}
	// a computer whose USB network starts at 1.2 s -> USB at 1.2 s
	{
		BootProbe p (700, 5);
		CHECK (run (p, 150, 1200, &at) == kUsbUp && at == 1200 && p.host_seen ());
	}
	// a computer on the bus that never starts a USB network (Windows 10 without the driver) -> today's after usbwait
	{
		BootProbe p (700, 5);
		CHECK (run (p, 150, ~0u, &at) == kNoDriver && at == 5000);
	}
	// SOF seen late (just before the quiet window ends) still counts as a computer
	{
		BootProbe p (700, 5);
		CHECK (run (p, 690, 3000, &at) == kUsbUp && at == 3000);
	}
	// the network already up when the wait starts (the overlap with the Wi-Fi chip's start): at once
	{
		BootProbe p (700, 5);
		CHECK (p.step (2500, true, true) == kUsbUp);
		CHECK (p.step (2600, false, false) == kUsbUp);	// decided once
	}
	// the first step after the quiet window (the Wi-Fi chip took longer): not decided on one sample, kMinWatchMs first
	{
		BootProbe p (700, 5);
		CHECK (p.step (1800, false, false) == kWaiting);
		CHECK (p.step (1850, false, false) == kWaiting);
		CHECK (p.step (1800 + kMinWatchMs, false, false) == kNoHost);
	}
	// ... and a bus reset at that moment (no SOF for a few ms), then start-of-frames: a computer, waited for
	{
		BootProbe p (700, 5);
		CHECK (p.step (1800, false, false) == kWaiting);
		CHECK (p.step (1830, true, false) == kWaiting && p.host_seen ());
		CHECK (p.step (2500, true, true) == kUsbUp);
	}
	// past usbwait at the first step too: watched first
	{
		BootProbe p (700, 5);
		CHECK (p.step (6000, true, false) == kWaiting);
		CHECK (p.step (6000 + kMinWatchMs, true, false) == kNoDriver);
	}

	// ---- trust: the cable only, never by default on another interface, usbtrust=0 turns it off
	CHECK (trusted (kIfUsb, true));
	CHECK (!trusted (kIfUsb, false));
	CHECK (!trusted (kIfWifi, true) && !trusted (kIfEthernet, true) && !trusted (kIfNone, true));

	// ---- the late computer (net=auto): asked on the touchscreen, never rebooted by itself
	{
		typedef LatePrompt L;
		L p;
		CHECK (p.mode () == L::kHidden && !p.step (0, false, true, L::kNoAns));		// nothing on the cable
		CHECK (!p.step (1000, true, true, L::kNoAns) && p.mode () == L::kHidden);		// a driver came up: let it settle
		CHECK (!p.step (1000 + L::kSettleMs - 1, true, true, L::kNoAns) && p.mode () == L::kHidden);
		CHECK (!p.step (1000 + L::kSettleMs, true, true, L::kNoAns) && p.mode () == L::kAsk);
		for (unsigned t2 = 4000; t2 < 600000; t2 += 7000) CHECK (!p.step (t2, true, true, L::kNoAns) && p.mode () == L::kAsk);	// it waits for the answer, for ever
		// NOT NOW: gone for this plug, never asked again while the computer stays
		CHECK (!p.step (700000, true, true, L::kNotNow) && p.mode () == L::kHidden);
		for (unsigned t2 = 701000; t2 < 900000; t2 += 9000) CHECK (!p.step (t2, true, true, L::kNoAns) && p.mode () == L::kHidden);
		CHECK (!p.step (900000, true, true, L::kRestart));					// (a stray answer when nothing is asked is nothing)
		// the cable pulled and plugged again: asked again
		CHECK (!p.step (910000, false, true, L::kNoAns));
		CHECK (!p.step (920000, true, true, L::kNoAns) && p.mode () == L::kHidden);
		CHECK (!p.step (920000 + L::kSettleMs, true, true, L::kNoAns) && p.mode () == L::kAsk);
		// RESTART: true exactly once; the drain succeeded (the caller reboots) or failed (a message, no reboot, no second ask)
		L r;
		r.step (0, true, true, L::kNoAns), r.step (L::kSettleMs, true, true, L::kNoAns);
		CHECK (r.mode () == L::kAsk);
		CHECK (r.step (3000, true, true, L::kRestart));
		CHECK (!r.step (3010, true, true, L::kRestart) && !r.step (3020, true, true, L::kNoAns));	// not twice
		L f;
		f.step (0, true, true, L::kNoAns), f.step (L::kSettleMs, true, true, L::kNoAns);
		CHECK (f.step (3000, true, true, L::kRestart));
		f.drain_failed ();									// the saves did not settle
		CHECK (f.mode () == L::kFailed);
		CHECK (!f.step (3010, true, true, L::kNoAns) && f.mode () == L::kFailed);			// the message stays until OK
		CHECK (!f.step (3020, true, true, L::kRestart) && f.mode () == L::kHidden);			// (OK is not a RESTART: it never restarts)
		for (unsigned t2 = 4000; t2 < 200000; t2 += 5000) CHECK (!f.step (t2, true, true, L::kNoAns) && f.mode () == L::kHidden);
		CHECK (!f.step (210000, false, true, L::kNoAns));
		CHECK (!f.step (220000, true, true, L::kNoAns));						// a new plug asks again
		CHECK (!f.step (220000 + L::kSettleMs, true, true, L::kNoAns) && f.mode () == L::kAsk);
		// a flapping driver never settles
		L g;
		CHECK (!g.step (0, true, true, L::kNoAns) && !g.step (1500, false, true, L::kNoAns) && !g.step (2500, true, true, L::kNoAns));
		CHECK (!g.step (4400, true, true, L::kNoAns) && g.mode () == L::kHidden && !g.step (4500, true, true, L::kNoAns) && g.mode () == L::kAsk);
		// no touchscreen: never asks, never restarts, whatever arrives
		L u;
		for (unsigned t2 = 0; t2 < 100000; t2 += 1000) CHECK (!u.step (t2, true, false, t2 > 50000 ? L::kRestart : L::kNoAns) && u.mode () == L::kHidden);
		// the touch screen comes and goes (a USB panel plugged in late, unplugged while asked): no touch, no question
		L v;
		for (unsigned t2 = 0; t2 < 20000; t2 += 1000) CHECK (!v.step (t2, true, false, L::kNoAns) && v.mode () == L::kHidden);	// a display without touch
		CHECK (!v.step (20000, true, true, L::kNoAns) && v.mode () == L::kHidden);						// the touch appears: it settles first
		CHECK (!v.step (20000 + L::kSettleMs, true, true, L::kNoAns) && v.mode () == L::kAsk);
		CHECK (!v.step (23000, true, false, L::kNoAns) && v.mode () == L::kHidden);						// it goes away: the question goes
		CHECK (!v.step (24000, true, false, L::kRestart) && v.mode () == L::kHidden);						// (an answer nobody could give does nothing)
		// the millisecond clock wraps
		L w;
		w.step (0xFFFFFF00u, true, true, L::kNoAns);
		CHECK (!w.step (0xFFFFFF00u + L::kSettleMs, true, true, L::kNoAns) && w.mode () == L::kAsk);
		CHECK (w.step (0xFFFFFF00u + L::kSettleMs + 10, true, true, L::kRestart));
	}

	// ---- MACs: locally administered unicast, stable, different ends
	uint8_t pi[6], host[6];
	macs (0x10000000abcdef12ull, pi, host);
	CHECK (pi[0] == 0x02 && host[0] == 0x06 && memcmp (pi + 1, host + 1, 5) == 0);
	CHECK ((pi[0] & 1) == 0 && (pi[0] & 2) && (host[0] & 1) == 0 && (host[0] & 2));
	CHECK (pi[1] == 0x00 && pi[2] == 0xab && pi[3] == 0xcd && pi[4] == 0xef && pi[5] == 0x12);
	char t[13];
	mac_text (host, t);
	CHECK (!strcmp (t, "0600ABCDEF12"));

	printf ("test_usbnet_cfg: OK\n");
	return 0;
}
