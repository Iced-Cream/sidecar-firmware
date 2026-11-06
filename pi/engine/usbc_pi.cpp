// pi/engine/usbc_pi.cpp - see usbc_pi.h.
#include "usbc_pi.h"

namespace usbc_pi {

static volatile int s_nMode = -1, s_nShown = 0, s_nAsk = -1;
static volatile unsigned s_nModes = 0;

void Publish (int nMode, unsigned nModes, int nShown)
{
	s_nMode = nMode;
	s_nModes = nModes;
	s_nShown = nShown;
}
int Mode () { return s_nMode; }
unsigned Modes () { return s_nModes; }
int Shown () { return s_nShown; }
void Ask (int nMode) { __atomic_store_n (&s_nAsk, nMode, __ATOMIC_RELEASE); }
// 0.4.1 builds the mirror for the Pi 4 only (pi/engine/build.sh): a Pi 5 gets OVERBRIDGE in 0.5.0
const char *NoOb ()
{
#if RASPPI >= 5
	return "PI5: IN 0.5.0";
#else
	return 0;
#endif
}
int TakeAsk ()
{
	return __atomic_exchange_n (&s_nAsk, -1, __ATOMIC_ACQ_REL);	// read and clear in one: an Ask between them is not lost
}

}
