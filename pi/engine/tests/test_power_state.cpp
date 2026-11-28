// pi/engine/tests/test_power_state.cpp - power_state.cpp: get_throttled's bits and the Pi 5's max_current to the
// warning, the /status JSON and the log line.
#include <string.h>

#include "check.h"
#include "power_state.h"

using namespace powerstate;

int main ()
{
	char b[512];
	State s;
	CHECK (warning (s) == 0 && short_warning (s) == 0);		// nothing known: no warning
	CHECK (json (s, b, sizeof b) > 0 && strstr (b, "\"known\":false") && strstr (b, "\"maxCurrent\":null") && strstr (b, "\"warn\":null,\"note\":null}"));
	line (s, b, sizeof b);
	CHECK (strstr (b, "throttled unknown") != 0);

	CHECK (s.board == 4);						// the default: the Pi 4
	s.known = true;
	s.throttled = 0;
	CHECK (warning (s) == 0);
	line (s, b, sizeof b);
	CHECK (strstr (b, "throttled 0x0 ") && strstr (b, "| ok"));

	s.throttled = 0x50000;						// under-voltage and throttling since boot, not now
	CHECK (warning (s) && strstr (warning (s), "since boot") && short_warning (s));
	json (s, b, sizeof b);
	CHECK (strstr (b, "\"uvNow\":false") && strstr (b, "\"uvBoot\":true") && strstr (b, "\"throttledBoot\":true"));
	CHECK (strstr (b, "\"warn\":\"power supply too weak"));

	CHECK (strstr (warning (s), "official 15 W (5.1 V 3 A) supply") && !strstr (warning (s), "27 W"));	// Pi 4's supply
	CHECK (strcmp (short_warning (s), "POWER TOO WEAK: use 15 W supply") == 0);
	s.board = 5;							// the same flags on a Pi 5: its adapter
	CHECK (strstr (warning (s), "since boot") && strstr (warning (s), "27 W (5 V 5 A) adapter directly") && !strstr (warning (s), "15 W"));
	CHECK (strcmp (short_warning (s), "POWER TOO WEAK: use 27 W adapter") == 0);
	json (s, b, sizeof b);
	CHECK (strstr (b, "27 W (5 V 5 A) adapter directly"));
	line (s, b, sizeof b);
	CHECK (strstr (b, "| power supply too weak (under-voltage since boot): use the 27 W (5 V 5 A) adapter directly"));
	s.board = 4;
	line (s, b, sizeof b);
	CHECK (strstr (b, "use the official 15 W (5.1 V 3 A) supply"));
	for (int bd = 4; bd <= 5; bd++)					// every short form fits the display's bottom line (36)
	{
		s.board = bd;
		CHECK (strlen (short_warning (s)) <= 36);
	}
	s.board = 4;

	s.throttled = 0x50005;						// under-voltage now wins
	CHECK (strstr (warning (s), "now"));
	json (s, b, sizeof b);
	CHECK (strstr (b, "\"uvNow\":true") && strstr (b, "\"throttledNow\":true") && strstr (b, "\"throttled\":327685"));

	s.throttled = 0;
	s.max_ma = 3000;						// Pi 5 without the 5 A PD negotiation
	s.board = 5;
	CHECK (warning (s) == 0 && short_warning (s) == 0);		// 3 A mode alone: a note, not a warning
	char nb[128];
	CHECK (note (s, nb, sizeof nb) > 0 && strstr (nb, "3 A mode (no 5 A USB-PD)") && strstr (nb, "600 mA") && !strstr (nb, "weak"));
	json (s, b, sizeof b);
	CHECK (strstr (b, "\"maxCurrent\":3000") && strstr (b, "\"warn\":null") && strstr (b, "\"note\":\"3 A mode"));
	line (s, b, sizeof b);
	CHECK (strstr (b, "max_current 3000 mA") && strstr (b, "| ok | 3 A mode"));
	s.throttled = 0x50000;						// with under-voltage since boot: the warning, no note
	CHECK (warning (s) && strstr (warning (s), "since boot") && note (s, nb, sizeof nb) == 0 && nb[0] == 0);
	s.throttled = 0;
	s.max_ma = 1500;						// rc040 review M1: the owner's hub reports 1.5 A
	CHECK (warning (s) == 0 && short_warning (s) == 0);
	CHECK (note (s, nb, sizeof nb) > 0 && !strncmp (nb, "1.5 A mode (no 5 A USB-PD)", 26));
	CHECK (!strstr (nb, "3 A mode") && !strstr (nb, "600 mA") && strstr (nb, "27 W adapter"));
	json (s, b, sizeof b);
	CHECK (strstr (b, "\"maxCurrent\":1500") && strstr (b, "\"note\":\"1.5 A mode (no 5 A USB-PD)"));
	line (s, b, sizeof b);
	CHECK (strstr (b, "max_current 1500 mA") && strstr (b, "| ok | 1.5 A mode"));
	s.max_ma = 2250;
	CHECK (note (s, nb, sizeof nb) > 0 && !strncmp (nb, "2.25 A mode", 11));
	s.max_ma = 2050;
	CHECK (note (s, nb, sizeof nb) > 0 && !strncmp (nb, "2.05 A mode", 11));
	s.max_ma = 900;
	CHECK (note (s, nb, sizeof nb) > 0 && !strncmp (nb, "0.9 A mode", 10));
	s.max_ma = 4000;						// (between 3 and 5 A: the current, no claim about the ports)
	CHECK (note (s, nb, sizeof nb) > 0 && !strcmp (nb, "4 A mode (no 5 A USB-PD)"));
	CHECK (note (s, nb, 8) == (int) strlen ("4 A mode (no 5 A USB-PD)") && strlen (nb) == 7);	// cut, length counted
	s.max_ma = 5000;
	CHECK (warning (s) == 0 && note (s, nb, sizeof nb) == 0);

	CHECK (json (s, b, 16) > 16 && strlen (b) == 15);		// cut, still terminated
	printf ("power_state: OK\n");
	return 0;
}
