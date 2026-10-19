// pi/lvgl-test/main.cpp - the display test kernel's entry (as pi/engine/main.cpp)
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
