// pi/engine/tests/test_cpuclock.cpp - cpuclock.h: the setting, the clock asked for, and the ARM clock read back.
#include "check.h"
#include "cpuclock.h"

using namespace cpuclock;

int main ()
{
	CHECK (normalize (1500) == 1500 && normalize (1200) == 1200 && normalize (1000) == 1000);
	CHECK (normalize (0) == 1500 && normalize (900) == 1500 && normalize (2400) == 1500);	// anything else: the default

	// the Pi 4: 600..1500 MHz
	CHECK (target_hz (1500, 600000000, 1500000000) == 1500000000u);
	CHECK (target_hz (1200, 600000000, 1500000000) == 1200000000u);
	CHECK (target_hz (1000, 600000000, 1500000000) == 1000000000u);
	CHECK (target_hz (777, 600000000, 1500000000) == 1500000000u);				// not an option: the default
	// the Pi 5 (1500..2400): the default stays at its own full clock; 1000 cannot go under the board's minimum
	CHECK (target_hz (1500, 1500000000, 2400000000u) == 2400000000u);
	CHECK (target_hz (1000, 1500000000, 2400000000u) == 1500000000u);

	// what the setting asks for: a Pi 5's default is its 2400, its 1200 and 1000 are the firmware's 1500; a Pi 4 as named
	CHECK (asked_mhz (1500, 1500000000, 2400000000u) == 2400 && asked_mhz (1200, 1500000000, 2400000000u) == 1500);
	CHECK (asked_mhz (1000, 1500000000, 2400000000u) == 1500);
	CHECK (asked_mhz (1500, 600000000, 1500000000) == 1500 && asked_mhz (1000, 600000000, 1500000000) == 1000);
	// measured against reported: over 5 % apart only; unknowns are not news
	CHECK (!measured_off (2400, 2400) && !measured_off (2400, 2300) && measured_off (2400, 2200) && measured_off (1500, 2400));
	CHECK (!measured_off (0, 2400) && !measured_off (2400, 0));

	// the ARM clock as read back (Circle's CCPUThrottle drops 1500 to its minimum when hot, behind the setting's
	// back): the new MHz when it moved, 0 when it did not or the read failed
	CHECK (follow_mhz (1500, 600000000) == 600);					// throttled
	CHECK (follow_mhz (600, 1500000000) == 1500);					// and back
	CHECK (follow_mhz (1500, 1500000000) == 0);					// no change: nothing to do
	CHECK (follow_mhz (1500, 1499999999) == 0 && follow_mhz (1500, 1500400000) == 0);	// rounds to the same MHz
	CHECK (follow_mhz (1500, 0) == 0);						// the mailbox failed: unchanged
	CHECK (follow_mhz (0, 600000000) == 600);					// not known yet
	CHECK (follow_mhz (1200, 1200000000) == 0 && follow_mhz (1200, 600000000) == 600);	// a capped setting too
	puts ("cpuclock: PASS");
	return 0;
}
