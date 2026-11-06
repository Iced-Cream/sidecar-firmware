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
void Ask (int nMode) { s_nAsk = nMode; }
int TakeAsk ()
{
	int n = s_nAsk;
	if (n >= 0) s_nAsk = -1;
	return n;
}

}
