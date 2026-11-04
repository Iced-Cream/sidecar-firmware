// pi/engine/cpuclock.h - the CPU clock option (Settings > System: 1500 / 1200 / 1000 MHz) and what follows from it
// (portable, header only: tests/test_cpuclock.cpp).
//  - the setting: 1500 (the default: the board's full clock, 1500 MHz on the Pi 4), 1200 or 1000
//  - the clock asked for on this board, the ARM clock as read back once a second (follow_mhz: Circle's own throttle to
//    600 MHz shows), and the firmware's measured clock against it (measured_off)
#ifndef _cpuclock_h
#define _cpuclock_h

namespace cpuclock {

enum : unsigned { kRefMHz = 1500, kN = 3 };
static const unsigned kOptions[kN] = {1500, 1200, 1000};

// cmdline / cpu.txt value to a setting: anything but 1200 and 1000 is the default
inline unsigned normalize (unsigned nMHz)
{
	for (unsigned i = 0; i < kN; i++) if (kOptions[i] == nMHz) return nMHz;
	return kRefMHz;
}

// The ARM clock to ask the firmware for, in Hz: the default is the board's own maximum (the Pi 5's 2400 MHz stays);
// 1200 / 1000 within the board's range
inline unsigned target_hz (unsigned nSettingMHz, unsigned nMinHz, unsigned nMaxHz)
{
	nSettingMHz = normalize (nSettingMHz);
	if (nSettingMHz == kRefMHz) return nMaxHz;
	unsigned long long hz = (unsigned long long) nSettingMHz * 1000000ull;
	if (hz < nMinHz) hz = nMinHz;
	if (hz > nMaxHz) hz = nMaxHz;
	return (unsigned) hz;
}

// The clock the setting asks for, in MHz, as the firmware's range gives it: what "the clock is lowered" is measured against.
// A Pi 5: 1500 asks for 2400; 1200 and 1000 for 1500, the firmware's minimum there. A Pi 4: 1500 / 1200 / 1000.
inline unsigned asked_mhz (unsigned nSettingMHz, unsigned nMinHz, unsigned nMaxHz)
{
	return (unsigned) (((unsigned long long) target_hz (nSettingMHz, nMinHz, nMaxHz) + 500000ull) / 1000000ull);
}

// The firmware's measured ARM clock against the clock it says it set (GET_CLOCK_RATE only repeats what was asked): more than
// 5 % apart, so its own throttle, which Circle does not see, shows. 0 on either side: not known.
inline bool measured_off (unsigned nReadMHz, unsigned nMeasuredMHz)
{
	if (nReadMHz == 0 || nMeasuredMHz == 0) return false;
	unsigned nDiff = nReadMHz > nMeasuredMHz ? nReadMHz - nMeasuredMHz : nMeasuredMHz - nReadMHz;
	return nDiff * 20 > nReadMHz;
}

// The ARM clock as read back (CCPUThrottle::GetClockRate, Hz) against the MHz last known: the new MHz when it moved,
// 0 when it did not (to the MHz, rounded) or could not be read (0 Hz: the mailbox failed). What the cores really run at,
// not the setting: at 1500 Circle's CCPUThrottle::Update drops the clock to its minimum
// (600 on the Pi 4) when the SoC passes its enforced temperature and raises it again when cool, behind the setting.
inline unsigned follow_mhz (unsigned nKnownMHz, unsigned nReadHz)
{
	if (nReadHz == 0) return 0;
	unsigned nMHz = (unsigned) (((unsigned long long) nReadHz + 500000ull) / 1000000ull);
	return nMHz == nKnownMHz ? 0 : nMHz;
}


}	// namespace cpuclock

#endif
