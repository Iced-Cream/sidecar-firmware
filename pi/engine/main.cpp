// pi/engine/main.cpp - Circle's entry.
#include <circle/startup.h>

#include "kernel.h"

int main (void)
{
	CKernel Kernel;
	if (!Kernel.Initialize ())
	{
		halt ();
		return EXIT_HALT;
	}
	Kernel.Run ();
	halt ();
	return EXIT_HALT;
}
