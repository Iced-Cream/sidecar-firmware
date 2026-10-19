// pi/ui/host/splash.c - the boot screen (sidecar_splash.c) on the Mac: 800x480, the bar at 3 of 8 stages, as a PPM.
#include "../sidecar_splash.h"
#include <stdio.h>
#define W 800
#define H 480
static uint16_t s_Fb[W * H];
int main (int argc, char **argv)
{
	sidecar_splash_draw (s_Fb, W, W, H);
	sidecar_splash_progress (s_Fb, W, W, H, 3, 8);
	FILE *f = fopen (argc > 1 ? argv[1] : "splash.ppm", "wb");
	fprintf (f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++)
	{
		unsigned c = s_Fb[i];
		unsigned char px[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2), (unsigned char) ((c & 31) << 3)};
		fwrite (px, 1, 3, f);
	}
	fclose (f);
	return 0;
}
