// pi/circle/pi5gadget/main.cpp
#include "kernel.h"
#include <circle/startup.h>

int main (void)
{
	CKernel Kernel;
	if (!Kernel.Initialize ())
	{
		halt ();
		return EXIT_HALT;
	}
	Kernel.Run ();		// ends in a reboot
	return EXIT_REBOOT;
}
