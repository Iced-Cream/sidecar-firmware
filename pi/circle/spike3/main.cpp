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

	Kernel.Run ();

	halt ();
	return EXIT_HALT;
}
