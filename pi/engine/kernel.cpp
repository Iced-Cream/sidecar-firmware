// pi/engine/kernel.cpp - the Pi engine (docs/pi-engine.md). Core 0: USB (the Rytm's Overbridge stream in the xHCI
// interrupt, its USB MIDI, the USB-A MIDI devices), the network (Circle's scheduler; the TCP link to rytm-emu, tcp_link.h,
// with link=tcp), the console, the stats, the card queue, the clean stop before a reboot. Cores 1-3: cores.cpp.
#include "kernel.h"
#include "idlewait.h"
#include "mbtrace.h"
#include "blackbox_pi.h"	// the black box (2026-10-30): the whole log appended to /.rytm/blackbox.log
#include "crumb.h"		// the crash breadcrumb (2026-10-29): kept over a watchdog reset, reported here at the next boot

static volatile unsigned s_nTempC;	// the SoC temperature as the stats line last read it (once a second); /status and the UI show this

#include <circle/alloc.h>
#include <circle/bcmpropertytags.h>
#include <circle/devicetreeblob.h>
#include <circle/machineinfo.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/memory.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <circle/util.h>
#include <circle/version.h>
#include <fatfs/ff.h>

#include "bridge.h"
#include "audio_return.h"	// /status "aret"
#include "fmt/fmt.h"
// A small card file written whole (core 0): core/fmt's atomic write (um_pi.h's, which this build does not have)
static boolean SdWriteWhole (const char *pPath, const char *pData, unsigned n)
{
	return core::fmt::write_atomic (pPath, pData, n) ? TRUE : FALSE;
}
#include "shared.h"
#include "usb_box.h"	// which USB MIDI device is the host link
#include "card_pass.h"	// MIDI between the card requests
#include "paircode_pi.h"	// the pairing code that gates the network (paircode.h)
#include "midibridge_pi.h"	// the Rytm MIDI bridge (midibridge.h): off at every boot
#include "sdfiles.h"
#include "wifi.h"
#include "fmt/fmt.h"		// write_atomic (SdWriteWhole)
#include "stor_pi.h"
#include "version.h"
#include <circle/new.h>

#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
extern bool g_bCardOff;			// platform_pi.cpp: testsecs=
#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
extern bool g_bCardGuard;			// card_guard.cpp: testcard=1
#endif
#endif

// The network options' defaults when cmdline.txt has none: build.sh writes hosted/net_defaults.h from PI_IP,
// PI_NETMASK and EMUHOST (its defaults: the ones below). cmdline.txt wins; it can be edited from the Mac with the card
// in the Pi (pi/circle/sdboot.py cmdline, sdfiles.h), but it also applies to every RAM kernel flashed later.
#if __has_include("hosted/net_defaults.h")
#include "hosted/net_defaults.h"
#endif
#ifndef GADGET_DEFAULT
#define GADGET_DEFAULT		"sidecar"
#endif
#ifndef NET_DEFAULT_IP
#define NET_DEFAULT_IP		"192.168.77.2"
#define NET_DEFAULT_NETMASK	"255.255.255.0"
#define NET_DEFAULT_EMUHOST	"192.168.77.1:7301"
#endif

static const char From[] = "engine";
static const char RebootMagic[] = "RYTM-REBOOT";
static volatile boolean s_bRebootRequested = FALSE;
static void RebootRequest (void) { s_bRebootRequested = TRUE; }   // UART IRQ: core 0's loop stops cleanly first
static unsigned s_nMidiInLost;		// the Rytm's USB MIDI bytes lost: a full g_MidiIn (the stats line's "lost")

static unsigned s_nLoopMax, s_nLoopN, s_nLoopSum;	// core 0's loop: its MsSleep (1) in us (/status "core0", reset when read)

// "a.b.c.d" or "a.b.c.d:port" -> IP (and port if asked); FALSE if malformed, trailing junk included ("1.2.3.4x",
// ":7301abc", a port where none is asked).
static boolean CKernel_ParseIP (const char *p, u8 IP[4], u16 *pPort = 0)
{
	if (p == 0) return FALSE;
	for (int i = 0; i < 4; i++)
	{
		if (*p < '0' || *p > '9') return FALSE;
		unsigned v = 0;
		while (*p >= '0' && *p <= '9' && v <= 255) v = v * 10 + (*p++ - '0');
		if (v > 255) return FALSE;
		IP[i] = (u8) v;
		if (i < 3 && *p++ != '.') return FALSE;
	}
	if (*p == 0) return TRUE;
	if (pPort == 0 || *p++ != ':' || *p < '0' || *p > '9') return FALSE;
	unsigned n = 0;
	while (*p >= '0' && *p <= '9' && n <= 65535) n = n * 10 + (*p++ - '0');
	if (*p != 0 || n == 0 || n > 65535) return FALSE;
	*pPort = (u16) n;
	return TRUE;
}

static const u8 *NetOption (CKernelOptions *pOpt, const char *pName, const char *pDefault, u8 Out[4])
{
	if (!CKernel_ParseIP (pOpt->GetAppOptionString (pName, pDefault), Out)) CKernel_ParseIP (pDefault, Out);
	return Out;
}

CKernel::CKernel (void)
:	m_Serial (&m_Interrupt),
	m_LogTee (&m_Serial),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_CPUThrottle (CPUSpeedMaximum),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),		// TRUE: plug-and-play
	m_Net (*(CNetSubSystem *) m_NetStore),		// constructed in Initialize (Ethernet or Wi-Fi: wifi.h)
	m_Cores (CMemorySystem::Get ()),
	m_Console (&m_Serial),
	m_Tcp (&m_Net),
	m_pTransport (&m_Usb),
	m_pMIDI (0),
	m_pSidecar (0),
	m_pGadget (0),
	m_nLastStats (0),
	m_pGadgetMode ("0"),
	m_bNetOK (FALSE),
	m_bWeb (FALSE),
	m_bSaveClock (FALSE),
	m_pWifi (0)
{
	// no m_ActLED.Blink (3) (the skeleton's): it held the boot 2.1 s before the first log line (measured 2026-10-16);
	// the boot screen shows the start now
}

// Each message as a record, its length (u16 LE) and its bytes: the TCP link sends whole messages (the emulator's
// USB MIDI queue splits at message boundaries). A record that does not fit is dropped whole (counted), so the
// records stay in step (core 3 is the only producer: the room can only grow meanwhile).
void CKernel::ToHost (const u8 *pData, int nLength)
{
	if (nLength <= 0) return;
	if (nLength > 4096)				// longer than one 'M' (tcp_link.h's m_Msg): dropped, counted
	{
		g_nToHostFull += nLength;
		return;
	}
	if (g_MidiOut.size () + 2 + nLength > kMidiOutBytes)
	{
		g_nToHostFull += nLength;
		return;
	}
	g_MidiOut.push ((u8) nLength);
	g_MidiOut.push ((u8) (nLength >> 8));
	for (int i = 0; i < nLength; i++) g_MidiOut.push (pData[i]);
}


static void CrumbPanic (void) { crumb::Why ("a panic (the log's last line says which)"); }
static u32 s_nPmRsts;			// PM_RSTS as this boot found it (crumb.h's report)

// The previous boot's breadcrumb on the card, /.rytm/prevboot.txt: the report's lines, then its whole log tail. Once,
// 60 s into the boot (a card that hangs a write would not hang every boot at once: no reboot loop), in its own section
// (41): skipped when the previous boot's core 0 died there. A planned reboot's (an update, the page's reboot) keeps the
// file there: the last crash's.
static void CrumbToCard (CLogger *pLogger)
{
	const char *From = "crumb";
	if (!crumb::had_prev ()) return;
	if (crumb::PrevPlanned ())
	{
		pLogger->Write (From, LogNotice, "crumb: the previous boot ended with a planned reboot: /.rytm/prevboot.txt kept (the last crash's)");
		return;
	}
	if (crumb::PrevDiedIn (41))
	{
		pLogger->Write (From, LogWarning, "crumb: the previous boot hung writing /.rytm/prevboot.txt: not written again");
		return;
	}
	crumb::Section Cs (41);
	static char s_Text[crumb::kLog + 8192];
	unsigned n = 0;
	crumb::Report ([&n] (const char *pLine) {
		while (*pLine && n < 8192 - 2) s_Text[n++] = *pLine++;
		s_Text[n++] = '\n';
	}, s_nPmRsts, 0);
	n += (unsigned) core_snprintf (s_Text + n, 128, "---- the previous boot's log, its last %u KB ----\n", crumb::kLog / 1024);
	n += crumb::PrevLog (s_Text + n, sizeof s_Text - n);
	boolean bOK = SdWriteWhole ("/.rytm/prevboot.txt", s_Text, n);
	pLogger->Write (From, bOK ? LogNotice : LogWarning, "crumb: the previous boot's report and log (%u bytes) %s /.rytm/prevboot.txt",
			n, bOK ? "in" : "NOT written to");
}

boolean CKernel::Initialize (void)
{
	crumb::Begin ();				// first: the previous boot's breadcrumb aside, before anything logs (crumb.h)
	boolean bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (921600);
	if (bOK) bOK = m_Logger.Initialize (&m_LogTee);	// the serial port, and the web page's copy
	if (bOK) bOK = m_Timer.Initialize ();
	if (!bOK) return FALSE;
	m_Logger.Write (From, LogNotice, "==== rytm-pi engine ==== Sidecar " SIDECAR_VERSION_FULL ", CPU %u MHz", m_CPUThrottle.GetClockRate () / 1000000);
#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
	// testsecs=S (docs/pi5.md "Engine test mode"): a run on a card that is not Sidecar's, a tryboot on Raspberry Pi OS. The
	// card is never mounted for the engine, so nothing is written to that system's boot partition; the watchdog runs
	// from here on, and at S seconds the Pi reboots, which ends the tryboot. testcard=1 with it: the engine uses the card
	// as usual, projects, presets and plugins on a tryboot, but changes nothing outside Sidecar's folders (card_guard.h)
	m_nTestSecs = m_Options.GetAppOptionDecimal ("testsecs", 0);
	if (m_nTestSecs != 0)
	{
		m_bTestCard = m_Options.GetAppOptionDecimal ("testcard", 0) != 0;
		g_bCardOff = !m_bTestCard;
		g_bCardGuard = m_bTestCard;
		m_Watchdog.Start (15);
		m_Logger.Write (From, LogNotice, "testsecs=%u: %s, the watchdog on, a reboot at the end", m_nTestSecs,
				m_bTestCard ? "testcard=1: the card in use, Sidecar's folders only" : "no card for the engine");
	}
#endif
	m_Logger.RegisterPanicHandler (CrumbPanic);
	s_nPmRsts = read32 (ARM_PM_RSTS);
	crumb::Report ([this] (const char *pLine) { m_Logger.Write ("crumb", LogNotice, "%s", pLine); }, s_nPmRsts, 6144);
	ReadPower (TRUE);				// the supply's state in the boot log (a weak one: Wi-Fi and the SD fail first)
	if (!m_EMMC.Initialize ()) m_Logger.Write (From, LogError, "SD: no card");
	BootStep (1);
	BootStep (2);
	unsigned t0 = m_Timer.GetClockTicks ();
	boolean bCard = bridge::begin (ToHost);	// the generic engine's core (core/generic): the card's state, the Rytm's profile
	m_Logger.Write (From, LogNotice, "engine: generic services, begin %s in %u ms (heap %u KB free)",
			bCard ? "ok" : "WITHOUT a card", (m_Timer.GetClockTicks () - t0) / 1000,
			(unsigned) (CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_ANY) >> 10));
	// the card queue (core/msamp/cardq.h): the streams' reads on this core, before the recorder's writes; cardq=0: on
	// core 3, as before
	bridge::card_worker (m_Options.GetAppOptionDecimal ("cardq", 1) != 0);
	BootStep (3);
	m_Usb.SetSend (m_Options.GetAppOptionDecimal ("tohost", 1) != 0);	// tohost=0: the replies dropped, as before
	LoadClock (bCard);				// the CPU clock option (cpumhz=)
	// fifo=: the target for every link; without it each link's own (UseLink: FifoUsb, FifoTcp)
	if (m_Options.GetAppOptionString ("fifo", 0) != 0) CEngineCores::FixFifoTarget ();
	CEngineCores::Configure (m_Options.GetAppOptionDecimal ("fifo", FifoUsb));
	CEngineCores::SetIdleWfe (m_Options.GetAppOptionDecimal ("idlewfe", 1) != 0);	// docs/pi-engine.md "Idle sleep"
	m_Console.Initialize ();
	BootStep (4);
	if (!m_Cores.Initialize ()) return FALSE;		// cores 1-3 start (cores.cpp)
	BootStep (5);
	BootStep (6);
	if (!m_USBHCI.Initialize ()) return FALSE;		// the Rytm is enumerated by plug-and-play in Run
	// the volumes besides the card (stor_pi.h, docs/STORAGE.md): USB drives as they come (core 0's loop); the Pi 5's
	// NVMe SSD now if one answers (nvme=0 in cmdline.txt skips it)
	stor_pi::Init (&m_USBHCI, &m_Interrupt, m_Options.GetAppOptionDecimal ("nvme", 1) != 0);
	BootStep (7);
	m_Console.SetCore0Handler (Core0Command, this);	// cpumhz= (and the Sidecar's commands)
	// USB-C, the computer's side: gadget=sidecar (default: UAC2 + USB MIDI, sidecar_gadget.h), midi (P1's MIDI
	// echo, gadget.h) or 0. The default can be built in (GADGET=midi pi/engine/build.sh): the serial bootloader
	// card's cmdline.txt is not edited from the Mac. A gadget that fails to start is left alone (no destructor).
#if RASPPI >= 5
	// what pi/circle/sdboot.py and make-sd-dual.sh tell a Pi 5 kernel by, gadget or not (older ones: the line below)
	m_Logger.Write (From, LogNotice, "board: Raspberry Pi 5 build (kernel_2712.img)");
#endif
#ifdef SIDECAR_NO_GADGET
	const char *pGadgetMode = "0";			// Pi 5 without PI5_GADGET=1 (docs/pi5.md); gadget= is ignored
	m_pGadgetMode = pGadgetMode;
	m_Logger.Write (From, LogNotice, "gadget: none on this board (Pi 5 build)");
	if (0)
#else
	const char *pGadgetMode = m_Options.GetAppOptionString ("gadget", GADGET_DEFAULT);
	m_pGadgetMode = pGadgetMode;
	if (strcmp (pGadgetMode, "sidecar") == 0)
#endif
	{
		CSidecarGadget *pSidecar = new CSidecarGadget (&m_Interrupt);
		if (pSidecar->Start ()) m_pSidecar = pSidecar;
		m_Logger.Write (From, m_pSidecar ? LogNotice : LogError, "gadget: USB-C Sidecar %s (%u in / %u out, 48 kHz, 24-bit, + MIDI)",
				m_pSidecar ? "up" : "failed to start", CSidecarGadget::ChIn, CSidecarGadget::ChOut);
		if (m_pSidecar != 0 && m_pSidecar->OsUpdate ())
			m_Logger.Write (From, LogWarning, "gadget: usbmidi=os:%s: Elektron OS upgrades from the computer pass to %s",
					m_pSidecar->OsUpdateName (), m_pSidecar->OsPass () ? "the Rytm on USB too" : "the emulator only");
		if (m_pSidecar != 0 && m_pSidecar->UsbReboot ())
			m_Logger.Write (From, LogWarning, "gadget: usbreboot=1: the computer's reboot SysEx on the MIDI port reboots the Pi");
	}
	else if (strcmp (pGadgetMode, "midi") == 0)
	{
		CSidecarMIDIGadget *pGadget = new CSidecarMIDIGadget (&m_Interrupt);
		if (pGadget->Start ()) m_pGadget = pGadget;
		m_Logger.Write (From, m_pGadget ? LogNotice : LogError, "gadget: USB-C MIDI device %s",
				m_pGadget ? "up (Sidecar)" : "failed to start");
	}
#ifndef SIDECAR_NO_GADGET
	else m_Logger.Write (From, LogNotice, "gadget: off");
#endif
#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
	if (m_nTestSecs != 0)
	{
		m_Watchdog.Start (15);
		if (m_pSidecar != 0 && m_Options.GetAppOptionDecimal ("uac2test", 0) != 0) m_pSidecar->Command ("uac2test=on");
		if (m_pSidecar != 0 && m_Options.GetAppOptionDecimal ("midifake", 0) != 0) m_pSidecar->Command ("midifake=on");
		blackbox_pi::TestLog (m_bTestCard);		// the boot's log, should the rest not get far
	}
#endif
	// the network: one stack on one device (Circle), Ethernet (static: the direct cable has no DHCP server) or, with
	// wifi=1 and a stored network, Wi-Fi (DHCP unless wifi.txt has ip=); mDNS: sidecar.local on either (web.h)
	m_pWifi = new CWifi;				// reads /.rytm/wifi.txt (the card is mounted by bridge::begin)
	CWifi::TMode WifiMode = m_pWifi->BootMode (&m_Options);
	if (WifiMode != CWifi::ModeOff && !m_pWifi->StartChip ()) WifiMode = CWifi::ModeOff;
	if (WifiMode == CWifi::ModeNet)
	{
		u8 GW[4], DNS[4];
		boolean bStatic = m_pWifi->StaticIP (m_IP, m_NetMask, GW, DNS);
		if (!bStatic) memset (m_IP, 0, sizeof m_IP);
		new (m_NetStore) CNetSubSystem (bStatic ? m_IP : 0, m_NetMask, bStatic ? GW : 0, bStatic ? DNS : 0, "sidecar",
						NetDeviceTypeWLAN);
	}
	else new (m_NetStore) CNetSubSystem (NetOption (&m_Options, "ip", NET_DEFAULT_IP, m_IP),
					     NetOption (&m_Options, "netmask", NET_DEFAULT_NETMASK, m_NetMask), 0, 0, "sidecar");
	m_bNetOK = m_Net.Initialize (FALSE);
	if (!m_bNetOK) m_Logger.Write (From, LogError, "network: init failed (link=tcp refused, usb only)");
	else if (WifiMode == CWifi::ModeNet)
	{
		m_Logger.Write (From, LogNotice, "network: Wi-Fi (%s; Ethernet unused, wifi=0 for Ethernet)", m_IP[0] ? "static" : "DHCP");
	}
	else m_Logger.Write (From, LogNotice, "network: %u.%u.%u.%u/%u.%u.%u.%u (Ethernet; link comes up by itself)%s",
			     m_IP[0], m_IP[1], m_IP[2], m_IP[3], m_NetMask[0], m_NetMask[1], m_NetMask[2], m_NetMask[3],
			     WifiMode == CWifi::ModeScan ? "; Wi-Fi chip up for scans" : "");
	m_pWifi->StartSupplicant (m_bNetOK);		// ModeNet: joins; any mode: a join's trial may judge from now on
	UseLink (m_Options.GetAppOptionString ("link", "usb"));
	// the pairing code (paircode.h, docs/pi-engine.md "Pairing code"): before anything on the network is served, so
	// no request can arrive ungated. cmdline paircode=0 turns the gate off (loudly), paircode=<8 digits> fixes the code.
	paircode_pi::Init (m_Options.GetAppOptionString ("paircode", 0));
	// the web page (web.h): http://sidecar.local once the Ethernet link is up; web=0 leaves it off
	if (m_bNetOK && m_Options.GetAppOptionDecimal ("web", 1) != 0)
	{
		new CWebStart (&m_Net, &m_LogTee, &m_Console, WebStatus, WebHealth, this);
		m_bWeb = TRUE;
		m_Logger.Write (From, LogNotice, "web: on (sidecar.local once the Ethernet link is up; web=0 turns it off)");
	}
	else m_Logger.Write (From, LogNotice, "web: off");
	// the SD card's boot files on TCP 7310 (sdfiles.h, pi/circle/sdboot.py: install to SD); sdport=0 turns it off
	if (m_bNetOK && m_Options.GetAppOptionDecimal ("sdport", 1) != 0) new CSdFiles (&m_Net);
	new CSdFiles (0, &m_Serial);		// and on the serial port after the console's ":sdf"
	// the Rytm's USB MIDI over TCP 7330 for hardware testing (midibridge_pi.h): listening, but OFF until turned on
	if (m_bNetOK) midibridge_pi::Start (&m_Net);
	return TRUE;
}

// GET /health's members (core 0: a web task), the one answer that needs no pairing code besides the page and /log:
// is this unit alive and well? The build, how long it has been up, the temperature, whether the Rytm and the stream
// are there, the supply's flags and the underruns. Deliberately nothing else - no host
// name, no address, no settings, no machine names, no counts of paired devices - and it is a function of its own, not
// a filtered /status, so a field added to /status can never appear here by accident.
void CKernel::WebHealth (CString *pOut, void *pParam)
{
	CKernel *k = (CKernel *) pParam;
	CString s;		// temp: the stats line's reading (about 1 s old), as /status: this endpoint is open, so no firmware call per request
	s.Format ("\"version\":\"" SIDECAR_VERSION "\",\"commit\":\"" SIDECAR_COMMIT "\",\"dirty\":%s,\"uptime\":%u,"
		  "\"temp\":%u,\"streaming\":%s,\"rytmMidi\":%s,\"underruns\":%u,\"pair\":\"%s\"",
		  SIDECAR_DIRTY ? "true" : "false", CTimer::Get ()->GetUptime (), (unsigned) s_nTempC,
		  CObUsb::Streaming () ? "true" : "false", k->m_pMIDI != 0 ? "true" : "false",
		  0u,					// (no output stage of its own: nothing to underrun)
		  paircode_pi::Gate ().mode () == paircode::kOff ? "off"
		  : !paircode_pi::Gate ().rng_ok () ? "rng-failed"	// refusing everything: paircode.h
		  : paircode_pi::Gate ().mode () == paircode::kFixed ? "fixed" : "on");
	pOut->Append (s);
	char Power[400];		// the supply's flags and its warning (power_state.h): a unit nobody can see
	powerstate::json (k->m_Power, Power, sizeof Power);
	pOut->Append (",");
	pOut->Append (Power);
}

// /status's members (core 0: a web task), the generic engine's. Plain reads of state other cores write: a stale value at worst.
void CKernel::WebStatus (CString *pOut, void *pParam)
{
	CKernel *k = (CKernel *) pParam;
	CString s;
	unsigned nTempWeb = s_nTempC;
	const CEngineCores::TStall ST = CEngineCores::Stall ();
	u8 IP[4];
	memcpy (IP, k->m_IP, sizeof IP);
	if (k->m_bNetOK) k->m_Net.GetConfig ()->GetIPAddress ()->CopyTo (IP);	// DHCP on Wi-Fi: the lease's
	s.Format ("\"engine\":\"generic\",\"ip\":\"%u.%u.%u.%u\",\"link\":\"%s\",\"gadget\":\"%s\",\"sidecar\":%s,\"mac\":%s,"
		  "\"streaming\":%s,\"rytmMidi\":%s,\"temp\":%u,\"tempWarn\":%s,\"clockLow\":%d,\"fifoTarget\":%u,"
		  "\"stall\":{\"gapUs\":%u,\"over2ms\":%u,\"midi\":%u,\"cmds\":%u,\"tick\":%u,\"idle\":%u}",
		  IP[0], IP[1], IP[2], IP[3], k->m_pTransport->Name (), k->m_pGadgetMode,
		  k->m_pSidecar ? "true" : "false", k->m_pSidecar ? (k->m_pSidecar->MacOn () ? "true" : "false") : "null",
		  CObUsb::Streaming () ? "true" : "false", k->m_pMIDI != 0 ? "true" : "false",
		  nTempWeb, k->m_Thermal.warn ? "true" : "false",
		  thermal::clock_low_mhz ((int) k->m_nClockMHz, (int) k->AskedMHz (), (int) nTempWeb), CEngineCores::FifoTarget (),
		  ST.gap_us, ST.gaps_over_2ms, ST.sec_us[0], ST.sec_us[2], ST.sec_us[3], ST.sec_us[5]);
	pOut->Append (s);
	s.Format (",\"usbmidi\":{\"os\":\"%s\",\"osUpgrade\":%s}",
		  k->m_pSidecar ? k->m_pSidecar->OsUpdateName () : "off", k->m_pSidecar && k->m_pSidecar->OsPass () ? "true" : "false");
	pOut->Append (s);
	{	// the SysEx messages that took core 3 over 1 ms
		CEngineCores::TSlowSx Sx[CEngineCores::kSlowSx];
		unsigned nSx = CEngineCores::SlowSysex (Sx);
		pOut->Append (CEngineCores::WaitPolled () ? ",\"ctlwait\":\"poll\",\"slowSx\":[" : ",\"ctlwait\":\"wfe\",\"slowSx\":[");
		for (unsigned i = 0; i < nSx; i++)
		{
			s.Format ("%s[%u,%u,%u,%u]", i ? "," : "", Sx[i].cmd, Sx[i].arg, Sx[i].len, Sx[i].us);
			pOut->Append (s);
		}
		pOut->Append ("]");
	}
	{	// the firmware calls core 0 made recently (mbtrace.h)
		unsigned n = mbtrace::count (), k2 = n < mbtrace::kN ? n : mbtrace::kN;
		pOut->Append (",\"mb\":[");
		for (unsigned i = 0; i < k2; i++)
		{
			const mbtrace::Rec &r = mbtrace::ring ()[(n - k2 + i) % mbtrace::kN];
			s.Format ("%s[%u,%u,%u]", i ? "," : "", r.id, r.at, r.dur_us);
			pOut->Append (s);
		}
		pOut->Append ("]");
	}
	s.Format (",\"cpu\":{\"setting\":%u,\"mhz\":%u,\"askedMhz\":%u,\"fullMhz\":%u,\"minMhz\":%u,\"measuredMhz\":%u,\"socMaxC\":%u,\"capped\":%s}",
		  k->m_nClockSetting, k->m_nClockMHz, k->AskedMHz (), k->m_CPUThrottle.GetMaxClockRate () / 1000000,
		  k->m_CPUThrottle.GetMinClockRate () / 1000000, k->m_nMeasuredMHz, k->m_Options.GetSoCMaxTemp (),
		  k->m_bClockCapped ? "true" : "false");
	pOut->Append (s);
	s.Format (",\"idle\":{\"wfe\":%s,\"asleepPct\":[%u,%u,%u,%u],\"wakes\":[%u,%u,%u,%u]}", CEngineCores::IdleWfe () ? "true" : "false",
		  k->m_nAsleepPct[0], k->m_nAsleepPct[1], k->m_nAsleepPct[2], k->m_nAsleepPct[3],
		  k->m_nWakes[0], k->m_nWakes[1], k->m_nWakes[2], k->m_nWakes[3]);
	pOut->Append (s);
	{
		static char Gen[1024];		// the host link's router, the Rytm's clock, kv and locks, the processor slots, the remote screen
		if (bridge::generic_json (Gen, sizeof Gen) > 0) { pOut->Append (","); pOut->Append (Gen); }
		char Store[240];		// the RAM store and the writes still on their way to the card
		unsigned long nHeapFree = (unsigned long) CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW);
		if (bridge::store_json (Store, sizeof Store, nHeapFree) > 0) { pOut->Append (","); pOut->Append (Store); }
	}
	s.Format (",\"aret\":{\"inDropped\":%u,\"inMissing\":%u}", aret::InDropped (), aret::InMissing ());
	pOut->Append (s);
	s.Format (",\"version\":\"" SIDECAR_VERSION "\",\"commit\":\"" SIDECAR_COMMIT "\",\"dirty\":%s,\"build\":\"%s %s\",\"circle\":\"%s %s\"",
		  SIDECAR_DIRTY ? "true" : "false", __DATE__, __TIME__, CIRCLE_NAME, CIRCLE_VERSION_STRING);
	pOut->Append (s);
	{
		char Power[400];
		powerstate::json (k->m_Power, Power, sizeof Power);
		pOut->Append (",");
		pOut->Append (Power);
	}
	if (k->m_pWifi != 0) { pOut->Append (",\"wifi\":"); k->m_pWifi->Status (pOut); }
	{
		char Bb[128];
		blackbox_pi::Status (Bb, sizeof Bb);
		pOut->Append (",\"blackbox\":");
		pOut->Append (Bb);
	}
	stor_pi::Status (pOut);
	s.Format (",\"core0\":{\"sleep1msAvgUs\":%u,\"sleep1msMaxUs\":%u,\"loops\":%u}", s_nLoopN ? s_nLoopSum / s_nLoopN : 0,
		  s_nLoopMax, s_nLoopN);
	s_nLoopMax = s_nLoopN = s_nLoopSum = 0;
	pOut->Append (s);
}


// "usb", "tcp" or "tcp:IP:PORT" (default emuhost= from cmdline, else NET_DEFAULT_EMUHOST): the engine's host. One at a
// time: in tcp the USB stream (a real Rytm, if attached) gets silence and its MIDI is dropped.
// The switch relies on the xHCI interrupt staying on core 0, this loop's core: FillOut (g_Output's consumer with
// usb) and MIDIPacketHandler (g_MidiIn's producer) then never run while this task is inside the TCP link's Pull or
// push, so flipping CObUsb::SetActive here hands the single-consumer/single-producer queues over cleanly. Routing
// that IRQ to another core would need a handshake (the IRQ acknowledging the flag) before the TCP link touches them.
void CKernel::UseLink (const char *pSpec)
{
	if (pSpec != 0 && strncmp (pSpec, "tcp", 3) == 0 && (pSpec[3] == 0 || pSpec[3] == ':'))
	{
		if (!m_bNetOK)
		{
			m_Logger.Write (From, LogWarning, "link: tcp refused - the network did not initialize; staying %s",
					m_pTransport->Name ());
			return;
		}
		u8 IP[4];
		u16 nPort = 7301;
		const char *pHost = pSpec[3] == ':' ? pSpec + 4 : m_Options.GetAppOptionString ("emuhost", NET_DEFAULT_EMUHOST);
		if (!CKernel_ParseIP (pHost, IP, &nPort))
		{
			m_Logger.Write (From, LogWarning, "link: bad address \"%s\"", pHost);
			return;
		}
		m_Tcp.SetPeer (IP, nPort);
		CObUsb::SetActive (FALSE);
		m_pTransport = &m_Tcp;
		CEngineCores::LinkFifoTarget (FifoTcp);
		m_Logger.Write (From, LogNotice, "link: tcp to %u.%u.%u.%u:%u (fifo target %u frames)", IP[0], IP[1], IP[2],
				IP[3], nPort, CEngineCores::FifoTarget ());
		return;
	}
	if (pSpec == 0 || strcmp (pSpec, "usb") != 0)
	{
		m_Logger.Write (From, LogWarning, "link: usb|tcp|tcp:IP:PORT, not \"%s\" - usb", pSpec ? pSpec : "");
	}
	m_Tcp.Stop ();
	if (m_pTransport == &m_Tcp)
	{
		// tcp -> usb: the frames the TCP link left go, and the USB stream (already running: no new-stream Reset in
		// ob_usb.cpp) fades in again instead of a click. No consumer meanwhile: the TCP link stopped, FillOut leaves
		// g_Output alone until SetActive (TRUE).
		aret::Reset ();
	}
	CObUsb::SetActive (TRUE);
	if (m_pTransport == &m_Tcp) m_pTransport = &m_Usb;
	CEngineCores::LinkFifoTarget (FifoUsb);
	m_Logger.Write (From, LogNotice, "link: %s", m_pTransport->Name ());
}

boolean CKernel::Core0Command (const char *pCmd, void *pParam)
{
	CKernel *pThis = (CKernel *) pParam;
	if (strncmp (pCmd, "cpumhz", 6) == 0 && (pCmd[6] == 0 || pCmd[6] == '='))	// Settings > System: the CPU clock option
	{
		if (pCmd[6] == '=')
		{
			unsigned n = 0;
			const char *v = pCmd + 7;
			boolean bDigits = *v >= '0' && *v <= '9';
			for (; *v >= '0' && *v <= '9' && n < 100000; v++) n = n * 10 + (*v - '0');
			if (!bDigits || *v || cpuclock::normalize (n) != n)
			{
				pThis->m_Logger.Write (From, LogWarning, "cpumhz=1500|1200|1000 (1500: the board's full clock)");
				return TRUE;
			}
			pThis->SetClock (n);
			pThis->m_bSaveClock = TRUE;	// into /.rytm/cpu.txt (audit 2026-10-26: it was never saved)
		}
		pThis->m_Logger.Write (From, LogNotice, "cpu: setting %u MHz, ARM clock %u MHz", pThis->m_nClockSetting,
				       pThis->m_nClockMHz);
		return TRUE;
	}
	return pThis->m_pSidecar != 0 && pThis->m_pSidecar->Command (pCmd);
}

// cpumhz= into /.rytm/cpu.txt (the default, 1500, leaves it empty)
void CKernel::SaveClock (void)
{
	m_bSaveClock = FALSE;
	CString s;
	if (m_nClockSetting != 1500) s.Format ("cpumhz=%u\n", m_nClockSetting);
	boolean bOK = SdWriteWhole ("/.rytm/cpu.txt", (const char *) s, s.GetLength ());
	m_Logger.Write (From, bOK ? LogNotice : LogWarning, "/.rytm/cpu.txt %s", bOK ? "written" : "NOT written");
}

void CKernel::MIDIPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength)
{
	// the Rytm's MIDI to the computer (the Sidecar's filter drops the host link and MIDI-CI): Circle hands over each
	// event packet's MIDI bytes, its header byte is the one before them in the endpoint's buffer
	// (usbmidi=os: on or emu: only while USB is the host; under link=tcp the emulator's MIDI goes to the computer, tcp_link.cpp)
	CSidecarGadget *pSidecar = CSidecarGadget::Get ();
	if (pSidecar != 0 && (!pSidecar->OsUpdate () || CObUsb::Active ())) pSidecar->FromRytmMIDI (pPacket - 1);
	midibridge_pi::FromRytm (pPacket, nLength);	// and to the Rytm MIDI bridge's client, if one is connected
	if (!CObUsb::Active ()) return;	// the TCP link is the host: a real Rytm's MIDI is not the engine's
	if (nLength >= 3 && (pPacket[0] & 0xF0) == 0x90 && pPacket[2] != 0) CObUsb::CaptureMarkMIDI ();
	for (unsigned i = 0; i < nLength; i++)
		if (!g_MidiIn.push (pPacket[i])) s_nMidiInLost++;	// a full queue: counted, as the TCP link's
	asm volatile ("dsb sy; sev");
}

void CKernel::MIDIRemoved (CDevice *pDevice, void *pContext)
{
	((CKernel *) pContext)->m_pMIDI = 0;
	g_bRytmGone = true;				// the link reset (the control core's)
}

// The USB-A MIDI devices (core/devmidi.h): every other USB MIDI device in the USB-A ports, up to four, as sources 0..3
// (in plug order), their channel and realtime messages to the control core (g_KeysIn). Gone on unplug (Circle's removed handler, from UpdatePlugAndPlay on core 0's loop: IRQs masked for the
// queue, whose other producers are the IRQs).
static const unsigned MaxKeyboards = 4;
static CDevice *volatile s_pKeyboard[MaxKeyboards];
static unsigned s_nKeysLost;			// keyboard messages lost: a full g_KeysIn
static void KeysPush (u32 w)
{
	if (!g_KeysIn.push (w)) s_nKeysLost++;
	asm volatile ("dsb sy; sev");
}
static void KeysPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength, unsigned nDevice, void *pParam)
{
	unsigned i = (unsigned) (uintptr) pParam;
	if (i >= MaxKeyboards || nLength < 1 || pPacket[0] < 0x80) return;
	u8 s = pPacket[0];
	if (s >= 0xF0 ? !(s == 0xF8 || s == 0xFA || s == 0xFB || s == 0xFC) : nLength < 2) return;	// channel messages, realtime
	KeysPush ((u32) i << 24 | (u32) s << 16 | (u32) (nLength > 1 ? pPacket[1] : 0) << 8 | (nLength > 2 ? pPacket[2] : 0));
}
static void KeysRemoved (CDevice *pDevice, void *pContext)
{
	unsigned i = (unsigned) (uintptr) pContext;
	if (i >= MaxKeyboards) return;
	s_pKeyboard[i] = 0;
	EnterCritical ();
	KeysPush ((u32) i << 24 | 0xF4u << 16);
	LeaveCritical ();
}
static TUsbBox BoxOf (CDevice *p)
{
	return usb_box (p->GetProperty (CDevice::PropertyVendor), p->GetProperty (CDevice::PropertyProduct));
}

// The Rytm's USB MIDI interface (Circle's umidiN) once enumerated: read only. Another Elektron box: left alone (usb_box.h). Every other USB MIDI device: a USB-A MIDI
// device (core/devmidi.h). While the Rytm is missing every pass, else four times a second (a device plugged in).
void CKernel::UpdateMIDI (void)
{
	static unsigned s_nScanAt;
	unsigned nNow = m_Timer.GetClockTicks ();
	if (m_pMIDI != 0 && nNow - s_nScanAt < 250000) return;
	s_nScanAt = nNow;
	// umidiN in Circle's numbering: the gadget's own MIDI device (USB-C, gadget.h) takes a number too
	for (unsigned n = 1; n <= 8; n++)
	{
		CString Name;
		Name.Format ("umidi%u", n);
		CDevice *p = CDeviceNameService::Get ()->GetDevice (Name, FALSE);
		if (p == 0 || p == m_pMIDI || (m_pGadget != 0 && m_pGadget->IsMIDIDevice (p))) continue;
		boolean bKnown = FALSE;
		for (unsigned i = 0; i < MaxKeyboards; i++) bKnown |= s_pKeyboard[i] == p;
		if (bKnown) continue;
		TUsbBox Box = BoxOf (p);
		if (Box == UsbBoxOther)
		{
			static u32 s_nSaid;				// umidiN already logged
			if (!(s_nSaid & 1u << n))
			{
				const char *pName = p->GetProperty (CDevice::PropertyProduct);
				m_Logger.Write (From, LogNotice, "midi: %s (%s) is not the host link: nothing goes to it", pName != 0 && *pName ? pName : "an Elektron box", (const char *) Name);
			}
			s_nSaid |= 1u << n;
			continue;
		}
		if (Box == UsbBoxHost)
		{
			if (m_pMIDI != 0) continue;
			CUSBMIDIDevice *pMIDI = (CUSBMIDIDevice *) p;
			pMIDI->RegisterRemovedHandler (MIDIRemoved, this);
			pMIDI->RegisterPacketHandler (MIDIPacketHandler);
			m_Usb.SetMIDI (0);			// a replug's umidi may sit at the old one's address: the sender sees it as new
			m_pMIDI = pMIDI;
			m_Logger.Write (From, LogNotice, "midi: the Rytm's USB MIDI found (%s), listening; the engine's replies %s",
					(const char *) Name,
					m_Options.GetAppOptionDecimal ("tohost", 1) != 0 ? "go to it (host link)" : "are dropped (tohost=0)");
			continue;
		}
		unsigned i = 0;
		while (i < MaxKeyboards && s_pKeyboard[i] != 0) i++;
		static CDevice *s_pRefused;
		if (i == MaxKeyboards)
		{
			if (s_pRefused != p) m_Logger.Write (From, LogWarning, "usba: %s not used (%u MIDI devices at most)", (const char *) Name, MaxKeyboards);
			s_pRefused = p;
			continue;
		}
		const char *pProduct = p->GetProperty (CDevice::PropertyProduct);
		strncpy (g_KeyNames[i], pProduct != 0 && *pProduct ? pProduct : "USB MIDI", sizeof g_KeyNames[i] - 1);
		g_KeyNames[i][sizeof g_KeyNames[i] - 1] = 0;
		s_pKeyboard[i] = p;
		p->RegisterRemovedHandler (KeysRemoved, (void *) (uintptr) i);
		((CUSBMIDIDevice *) p)->RegisterPacketHandler (KeysPacketHandler, (void *) (uintptr) i);
		EnterCritical ();
		KeysPush ((u32) i << 24 | 0xF4u << 16 | 1u << 8);
		LeaveCritical ();
		m_Logger.Write (From, LogNotice, "usba: MIDI device %u: %s (%s) (core/devmidi.h)", i + 1, g_KeyNames[i], (const char *) Name);
	}
}

// The stats line, once a second (the generic engine's)
void CKernel::LogStats (void)
{
	m_pTransport->LogStats (&m_Logger);
	CEngineCores::TStats s;
	CEngineCores::TakeStats (&s);
	unsigned nTemp;		// the one reading a second (a mailbox call, traced): stats line, /status, the warning
	{ mbtrace::Scope Mb (3); nTemp = m_CPUThrottle.GetTemperature (); }
	s_nTempC = m_nTempC = nTemp;
	ThermalStep (nTemp);
	FollowClock ();
	auto pct = [&] (u64 t) { return (unsigned) (t * 1000 / s.nTicks); };	// tenths of a percent
	for (int c = 0; c < 4; c++)
	{
		m_nAsleepPct[c] = (pct (s.nSleep[c]) + 5) / 10;
		m_nWakes[c] = s.nWakes[c];
	}
	unsigned c3 = pct (s.nBusy[3]);
	{	// the connect splash's address line on the Rytm's screen (core/screen), when it changes
		static u8 s_SaidIP[4];
		static boolean s_bSaid = FALSE;
		u8 IP[4] = {0, 0, 0, 0};
		if (m_bNetOK && m_Net.IsRunning ()) m_Net.GetConfig ()->GetIPAddress ()->CopyTo (IP);
		if (!s_bSaid || memcmp (IP, s_SaidIP, 4) != 0)
		{
			s_bSaid = TRUE;
			memcpy (s_SaidIP, IP, 4);
			CString a;
			if (IP[0]) a.Format ("%u.%u.%u.%u", IP[0], IP[1], IP[2], IP[3]);
			bridge::screen_address ((const char *) a);
		}
	}
	m_Logger.Write (From, LogNotice,
		"engine: generic | cpu c3 %u.%u %% | pass max %u us | midi %u B (lost %u) sysex %u notes %u | to-host sent %u B %u msgs fail %u dropped %u | %u C",
		c3 / 10, c3 % 10, s.nMaxPassUs, s.nMidiBytes, s_nMidiInLost, s.nSysEx, s.nNotes,
		m_Usb.MIDIOutBytes (), m_Usb.MIDIOutMessages (), m_Usb.MIDIOutFailed (),
		m_Usb.MIDIOutDropped () + m_Tcp.MIDIOutDropped () + g_nToHostFull + bridge::to_host_dropped (), nTemp);
	static unsigned s_nPowerTick;
	if (++s_nPowerTick % 10 == 0) { mbtrace::Scope Mb (1); ReadPower (FALSE); }
	if (s_nPowerTick % 10 == 5)
	{
		MeasureClock ();
		m_Logger.Write (From, LogNotice, "idle: asleep %u %u %u %u %%, wakes %u %u %u %u a second (cores 0-3), wfe %s, ARM clock %u MHz, measured %u, SoC %u C",
				m_nAsleepPct[0], m_nAsleepPct[1], m_nAsleepPct[2], m_nAsleepPct[3], m_nWakes[0], m_nWakes[1], m_nWakes[2], m_nWakes[3],
				CEngineCores::IdleWfe () ? "on" : "off", m_nClockMHz, m_nMeasuredMHz, nTemp);
	}
	if (s_nPowerTick % 60 == 0 && m_bWeb) CWebServer::LogStats (&m_Logger);
	if (s_nPowerTick % 60 == 0) paircode_pi::WarnTick ();
	if (m_pGadget != 0) m_pGadget->LogStats (&m_Logger);
	if (m_pSidecar != 0) m_pSidecar->LogStats (&m_Logger);
}


// The supply (power_state.h): get_throttled read without clearing (request value 0 keeps the sticky bits), and on the
// Pi 5 the firmware's /chosen/power/max_current (read once: the PD negotiation is done before the kernel starts).
// Logged at boot, at once when the flags change (a warning while a problem shows), and once a minute.
void CKernel::ReadPower (boolean bLog)
{
	static boolean s_bMaxRead = FALSE;
	static unsigned s_nLogged = ~0u, s_nCalls = 0;
	if (!s_bMaxRead)
	{
		s_bMaxRead = TRUE;
		m_Power.board = RASPPI >= 5 ? 5 : 4;	// the warning names this board's official supply
		const CDeviceTreeBlob *pDTB = CMachineInfo::Get ()->GetDTB ();
		const TDeviceTreeNode *pNode = pDTB != 0 ? pDTB->FindNode ("/chosen/power") : 0;
		const TDeviceTreeProperty *pProp = pNode != 0 ? pDTB->FindProperty (pNode, "max_current") : 0;
		if (pProp != 0 && pDTB->GetPropertyValueLength (pProp) == 4) m_Power.max_ma = (int) pDTB->GetPropertyValueWord (pProp, 0);
	}
	CBcmPropertyTags Tags;
	TPropertyTagSimple Throttled;
	Throttled.nValue = 0;
	m_Power.known = Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled, sizeof Throttled, 4);
	m_Power.throttled = m_Power.known ? Throttled.nValue : 0;
	unsigned nKey = m_Power.known ? m_Power.throttled : ~1u;
	boolean bChanged = nKey != s_nLogged;
	if (bLog || bChanged || ++s_nCalls % 6 == 0)
	{
		char Line[256];
		powerstate::line (m_Power, Line, sizeof Line);
		m_Logger.Write (From, powerstate::warning (m_Power) != 0 ? LogWarning : LogNotice, "power: %s", Line);
		s_nLogged = nKey;
	}
}

// The CPU clock option (cpuclock.h: 1500 / 1200 / 1000 MHz). cpumhz= in /.rytm/cpu.txt (an older card's /.rytm/display.txt
// before there is one), cmdline.txt's wins; read before the boot bench. 1500 is the board's full clock (the Pi 4's 1500; a Pi 5 keeps its own maximum).
void CKernel::LoadClock (boolean bCard)
{
	unsigned nMHz = 1500;
	if (bCard)
	{
		static char Text[1536];
		FIL f;
		UINT n = 0;
		Text[0] = 0;
		if (f_open (&f, "SD:/.rytm/cpu.txt", FA_READ) == FR_OK || f_open (&f, "SD:/.rytm/display.txt", FA_READ) == FR_OK)
		{
			f_read (&f, Text, sizeof Text - 1, &n);
			f_close (&f);
		}
		Text[n] = 0;
		const char *p = strstr (Text, "cpumhz=");
		if (p != 0 && (p == Text || p[-1] == '\n'))
		{
			unsigned v = 0;
			for (p += 7; *p >= '0' && *p <= '9' && v < 100000; p++) v = v * 10 + (*p - '0');
			nMHz = cpuclock::normalize (v);
		}
	}
	if (m_Options.GetAppOptionString ("cpumhz", 0) != 0) nMHz = cpuclock::normalize (m_Options.GetAppOptionDecimal ("cpumhz", nMHz));
	m_nClockSetting = nMHz;
	if (nMHz == 1500) m_nClockMHz = (m_CPUThrottle.GetClockRate () + 500000) / 1000000;	// as Circle set it (its maximum): no change
	else SetClock (nMHz);
	m_Logger.Write (From, LogNotice, "cpu: setting %u MHz, ARM clock %u MHz (asks for %u; the firmware's range %u-%u, Circle lowers it above %u C)",
			m_nClockSetting, m_nClockMHz, AskedMHz (), m_CPUThrottle.GetMinClockRate () / 1000000, m_CPUThrottle.GetMaxClockRate () / 1000000,
			m_Options.GetSoCMaxTemp ());
}

unsigned CKernel::AskedMHz (void) const
{
	return cpuclock::asked_mhz (m_nClockSetting, m_CPUThrottle.GetMinClockRate (), m_CPUThrottle.GetMaxClockRate ());
}

// Runtime, not config.txt's arm_freq: Settings > System can set it, it takes a tap and no reboot, and a boot file the file
// API cannot write is not a setting. Below the full clock the ARM clock is set through the mailbox (Circle's CCPUThrottle
// only knows its minimum and maximum), and CCPUThrottle::Update is no longer called: it would put the maximum back
// whenever the SoC is cool. The firmware's own limits (80 C soft, 85 C hard) stay. Back at 1500: CCPUThrottle's SetSpeed
// (maximum) and its Update again. One mailbox call to set it and one to read it back, on core 0, when the setting changes:
// a tap during playback can be heard once.
void CKernel::SetClock (unsigned nSettingMHz)
{
	nSettingMHz = cpuclock::normalize (nSettingMHz);
	m_nClockSetting = nSettingMHz;
	if (!m_CPUThrottle.IsDynamic ())
	{
		m_Logger.Write (From, LogWarning, "cpu: the firmware gave no clock range, the CPU clock is left as it is");
		return;
	}
	const unsigned nHz = cpuclock::target_hz (nSettingMHz, m_CPUThrottle.GetMinClockRate (), m_CPUThrottle.GetMaxClockRate ());
	const boolean bFull = nHz >= m_CPUThrottle.GetMaxClockRate ();
	if (bFull)
	{
		m_CPUThrottle.SetSpeed (CPUSpeedMaximum, FALSE);
		m_bClockCapped = FALSE;
	}
	else
	{
		CBcmPropertyTags Tags;
		TPropertyTagSetClockRate Tag;
		Tag.nClockId = CLOCK_ID_ARM;
		Tag.nRate = nHz;
		Tag.nSkipSettingTurbo = 0;
		if (!Tags.GetTag (PROPTAG_SET_CLOCK_RATE, &Tag, sizeof Tag, 12))
			m_Logger.Write (From, LogWarning, "cpu: the firmware refused %u MHz", nHz / 1000000);
		m_bClockCapped = TRUE;
	}
	CTimer::SimpleusDelay (355);				// (the clock's transition: Circle waits as long)
	m_nClockMHz = (m_CPUThrottle.GetClockRate () + 500000) / 1000000;
	if (m_nClockMHz == 0) m_nClockMHz = nHz / 1000000;
	m_Logger.Write (From, LogNotice, "cpu: ARM clock %u MHz (setting %u)", m_nClockMHz, m_nClockSetting);
}

// The temperature warning (thermal.h): on at 80 C, off below 75 C, from the second's one reading. Logged once per
// crossing (the clock the unit runs at then: one mailbox call, at the crossing only). The Rytm's popup is owed once
// per crossing: sent each second until the Rytm's remote UI has shown it (it cannot while a page is open, the connect
// splash runs, or the Rytm does not understand popups), dropped when the warning clears first. It never overrides a page.
// Its text says so when Circle's throttle has lowered the clock (socmaxtemp 78: by 80 C it usually has).
void CKernel::ThermalStep (unsigned nTempC)
{
	thermal::Event e = thermal::update (m_Thermal, (int) nTempC);
	if (e == thermal::kWarned)
		m_Logger.Write (From, LogWarning, "thermal: SoC %u C, warning on (from %d C, off below %d C), warning %u this boot, ARM clock %u MHz",
				nTempC, (int) thermal::kWarnC, (int) thermal::kClearBelowC, m_Thermal.crossings,
				m_CPUThrottle.GetClockRate () / 1000000);
	else if (e == thermal::kCleared)
		m_Logger.Write (From, LogNotice, "thermal: SoC %u C, warning off (peak %d C)", nTempC, m_Thermal.peak_c);
	if (thermal::popup_due (m_ThermalPopup, m_Thermal, e, bridge::rui_popups ()))
	{
		char L1[33], L2[33];
		thermal::popup_text (L1, L2, (int) nTempC, thermal::clock_low_mhz ((int) m_nClockMHz, (int) AskedMHz (), (int) nTempC));
		bridge::rui_popup (L1, L2);
	}
}

// The ARM clock the cores really run at, read back once a second here (one mailbox call, as the temperature above) and
// right after each CCPUThrottle::Update that can act (the main loop): at 1500 Circle's Update drops the clock to its
// minimum - 600 MHz on the Pi 4 - when the SoC passes its enforced temperature and raises it again 3 C below. Logged
// when it moves; /status and the thermal popup's text read it.
void CKernel::FollowClock (void)
{
	unsigned nHz;
	{ mbtrace::Scope Mb (4); nHz = m_CPUThrottle.GetClockRate (); }
	const unsigned nMHz = cpuclock::follow_mhz (m_nClockMHz, nHz);
	if (nMHz == 0) return;
	m_Logger.Write (From, LogWarning, "cpu: ARM clock %u MHz (was %u, setting %u, SoC %u C)", nMHz, m_nClockMHz,
			m_nClockSetting, m_nTempC);
	m_nClockMHz = nMHz;
}

// What the firmware measures, not what it says it set: GET_CLOCK_RATE repeats the rate asked for, so the firmware's own
// throttle (80 C soft, 85 C hard, undervoltage) is invisible to FollowClock. One mailbox call, every 10 s, not per /status
// request; logged when the two part and when they agree again. Only reported.
void CKernel::MeasureClock (void)
{
	static boolean s_bOff = FALSE;
	CBcmPropertyTags Tags;
	TPropertyTagClockRate Tag;
	Tag.nClockId = CLOCK_ID_ARM;
	boolean bOK;
	{ mbtrace::Scope Mb (5); bOK = Tags.GetTag (PROPTAG_GET_CLOCK_RATE_MEASURED, &Tag, sizeof Tag, 4); }
	if (!bOK) return;
	m_nMeasuredMHz = (Tag.nRate + 500000) / 1000000;
	boolean bOff = cpuclock::measured_off (m_nClockMHz, m_nMeasuredMHz);
	if (bOff == s_bOff) return;
	s_bOff = bOff;
	m_Logger.Write (From, bOff ? LogWarning : LogNotice, "cpu: measured %u MHz, reported %u MHz (SoC %u C)", m_nMeasuredMHz, m_nClockMHz, m_nTempC);
}

void CKernel::BootStep (unsigned nStep)
{
#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
	if (m_nTestSecs != 0) m_Watchdog.Start (15);	// fed through the boot
#endif
	(void) nStep;
}

void CKernel::StopAndReboot (const char *pWhy, boolean bTryboot)
{
	m_Logger.Write (From, LogNotice, "reboot: %s - stopping the %s stream first", pWhy, m_pTransport->Name ());
	{
		char Why[68];
		core_snprintf (Why, sizeof Why, "a planned reboot: %s", pWhy);
		crumb::Why (Why);
	}
	m_pTransport->Stop ();
	if (m_pTransport != &m_Usb) m_Usb.Stop ();		// a Rytm that streams anyway (no-op without one)
	m_Timer.MsDelay (50);					// the log line out
	if (bTryboot && !CSdFiles::SetTryboot ()) m_Logger.Write (From, LogWarning, "reboot: the tryboot flag was refused (a plain boot follows)");
	blackbox_pi::Flush ();					// and onto the card (the black box; after the flag: a slow card cannot lose it)
	m_Watchdog.Stop ();
	reboot ();
}

#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
void CKernel::UsbTestTick (void)
{
	static unsigned s_nLastLog;
	unsigned nNow = m_Timer.GetUptime ();
	if (nNow >= m_nTestSecs)
	{
		m_Logger.Write (From, LogNotice, "testsecs: %u s are up", m_nTestSecs);
		blackbox_pi::TestLog (m_bTestCard);
		StopAndReboot ("testsecs");
	}
	if (nNow >= s_nLastLog + 10)
	{
		s_nLastLog = nNow;
		blackbox_pi::TestLog (m_bTestCard);
	}
}
#endif

void CKernel::Run (void)
{
	m_Serial.RegisterMagicReceivedHandler (RebootMagic, RebootRequest);
	m_Logger.Write (From, LogNotice, "boot timestamp %u us; waiting for the Rytm (1935:0b48, OVERBRIDGE mode)",
			m_Timer.GetClockTicks ());
	m_nLastStats = m_Timer.GetTicks ();
	m_Watchdog.Start (15);
	CEngineCores::EventStream ();		// core 0's wfe at the loop's end: 152 us at most (idlewait.h)
	m_Logger.Write (From, LogNotice, "idle: wfe %s (idlewfe=0 turns it off)", CEngineCores::IdleWfe () ? "on" : "off");
	u64 nLoopT = 0;
	boolean bCrumbCard = FALSE;
	for (;;)
	{
		crumb::Beat (0);
		if (!bCrumbCard && m_Timer.GetUptime () >= 60)	// the previous boot's breadcrumb on the card (CrumbToCard)
		{
			bCrumbCard = TRUE;
			CrumbToCard (&m_Logger);
		}
		{	// a whole iteration of this loop (top to top): what the wrapped sections below do not cover (IRQs)
			u64 t, f;
			asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t));
			asm volatile ("mrs %0, cntfrq_el0" : "=r" (f));
			if (nLoopT) mbtrace::Note (30, 0, (unsigned) ((t - nLoopT) * 1000000 / f), 1000);
			nLoopT = t;
		}
		m_Watchdog.Start (15);	// fed: a hang of this loop reboots in 15 s (the BCM watchdog's maximum: a TCP connect may block for seconds)
#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
		if (m_nTestSecs != 0) UsbTestTick ();
#endif
		if (m_Console.TakeWatchdogTest ())
		{
			m_Logger.Write (From, LogWarning, "watchdog test: core 0 hangs (the watchdog reboots in 15 s)");
			for (;;) asm volatile ("");	// interrupts still run: the log line goes out
		}
		boolean bPlugAndPlay;
		{ mbtrace::Scope Mb (10, 200); bPlugAndPlay = m_USBHCI.UpdatePlugAndPlay (); }
		{ mbtrace::Scope Mb (11, 200); if (m_pGadget != 0) m_pGadget->Poll (); }
		{ mbtrace::Scope Mb (12, 200); if (m_pSidecar != 0) m_pSidecar->Poll (); }
		{ mbtrace::Scope Mb (13, 200); UpdateMIDI (); }
		// (a capped clock is held: Update would raise it again when cool.) CCPUThrottle::Update acts once every 4 s
		// and is a no-op in between; it is called on that cadence here (4 s + 10 ms since our last call, so its own
		// 4 s gate is open every time) so that the clock it may just have changed is read back at once (FollowClock)
		if (!m_bClockCapped && m_Timer.GetClockTicks () - m_nThrottleAt >= 4 * CLOCKHZ + CLOCKHZ / 100)
		{
			m_nThrottleAt = m_Timer.GetClockTicks ();
			{ mbtrace::Scope Mb (2, 200); m_CPUThrottle.Update (); }
			FollowClock ();
		}
		m_Usb.SetMIDI (m_pMIDI);			// umidi1 (or 0): the engine's replies to the Rytm
		{ mbtrace::Scope Mb (14, 200); m_pTransport->Poll (); }
		if (m_pTransport != &m_Tcp) m_Tcp.Reap ();	// a connect abandoned by :link=usb is closed when it ends
		{ mbtrace::Scope Mb (15, 200); m_Console.Poll (); }
		// the card's turns, MIDI between the requests (card_pass.h): only the USB transport's Poll, which is the merge (TCP's may connect)
		auto CardPass = [this] ()
		{
			card_pass (8, [] { mbtrace::Scope Mb (16, 200); return bridge::card_service (1); },
				   [this]
				   {
					if (m_pTransport == &m_Usb) { mbtrace::Scope Mb (14, 200); m_Usb.Poll (); }
				   });
		};
		CardPass ();								// the streams' card requests first (core/msamp/cardq.h)
		{ mbtrace::Scope Mb (19, 200); stor_pi::Poll (bPlugAndPlay); }		// USB drives in and out, the bench's step (stor_pi.h)
#if RASPPI >= 5 && !defined (SIDECAR_NO_GADGET)
		if (m_nTestSecs == 0 || m_bTestCard)
#endif
		blackbox_pi::Poll ();						// the log to /.rytm/blackbox.log (its own crumb section, 42)
		if (m_bSaveClock) SaveClock ();
		paircode_pi::Poll ();			// the Rytm's SIDECAR MENU > WEB UI asks: a new code, forget, web on / off
		char Link[40];
		if (m_Console.TakeLink (Link, sizeof Link)) UseLink (Link);
		if (m_pSidecar != 0) m_pSidecar->SetLinkTcp (m_pTransport == &m_Tcp);	// usbmidi=os:emu follows the host
		if (s_bRebootRequested) StopAndReboot ("the flasher's magic");
		if (m_pSidecar != 0 && m_pSidecar->TakeReboot ()) StopAndReboot ("asked on the USB-C MIDI port (usbreboot=1)");
		if (g_Guard.FatalWhy () != 0) StopAndReboot (g_Guard.FatalWhy ());
		if (m_pWifi->TakeReboot ()) StopAndReboot ("the Wi-Fi settings (a join, or a failed one's revert)");
		if (bridge::take_restart ()) StopAndReboot ("another device's state folder");
		CSdFiles::TReboot SdReboot = CSdFiles::TakeReboot ();
		if (SdReboot != CSdFiles::RebootNone) StopAndReboot ("asked on the SD file port", SdReboot == CSdFiles::RebootTry);
		if (m_Timer.GetTicks () - m_nLastStats >= HZ || m_Console.TakeStatsNow ())
		{
			m_nLastStats = m_Timer.GetTicks ();
			if (m_Console.Dumping ()) m_Timer.MsDelay (25);	// the TX buffer drains: a log line is not cut
			crumb::Section Cs (24);
			LogStats ();
		}
		asm volatile ("dsb sy; sev");			// core 3's heartbeat: its tick and card work run without traffic
		{
			unsigned t = m_Timer.GetClockTicks ();
			{
				crumb::Section Cs (40);
				if (CEngineCores::IdleWfe ())			// the network's tasks run in the Yields (idlewait.h)
					idlewait::spend (t, 1000, [&] { return m_Timer.GetClockTicks (); }, [&] { m_Scheduler.Yield (); },
							 [] { CEngineCores::Sleep (); });
				else m_Scheduler.MsSleep (1);			// the network's tasks run meanwhile
			}
			t = m_Timer.GetClockTicks () - t;	// how late the loop wakes: core 0's tasks (Wi-Fi's polling) measured
			s_nLoopN++;
			s_nLoopSum += t;
			if (t > s_nLoopMax) s_nLoopMax = t;
			stor_pi::NoteLoop (t);		// (the bench's busy phases: their own max, not reset by /status)
		}
	}
}
