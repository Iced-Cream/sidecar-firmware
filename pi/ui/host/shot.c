// pi/ui/host/shot.c - the Pi screen's layout on the Mac: LVGL renders sidecar_ui into an 800x480 RGB565 buffer with
// made-up status and a touch, written as a PPM (make shot: shot.png). No window, no SDL.
#include "lvgl.h"
#include "../sidecar_ui.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define W 800
#define H 480
static uint16_t s_Fb[W * H];
static uint16_t s_Buf[W * H / 10];

static void flush (lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	const uint16_t *p = (const uint16_t *) px;
	for (int y = a->y1; y <= a->y2; y++)
		for (int x = a->x1; x <= a->x2; x++) s_Fb[y * W + x] = *p++;
	lv_display_flush_ready (d);
}

static void on_command (const char *c, void *p)
{
	(void) p;
	printf ("command: %s\n", c);
}

// a finger at (x,y): pressed for a few LVGL ticks, then lifted
static int s_Px, s_Py, s_Down;
static void pointer (lv_indev_t *i, lv_indev_data_t *d)
{
	d->state = s_Down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	d->point.x = s_Px;
	d->point.y = s_Py;
}
static void run (int n) { for (int i = 0; i < n; i++) { lv_tick_inc (40); lv_timer_handler (); } }
static void tap (int x, int y)
{
	s_Px = x; s_Py = y; s_Down = 1; run (3);
	s_Down = 0; run (3);
}

static void write_ppm (const char *name)
{
	FILE *f = fopen (name, "wb");
	fprintf (f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++)
	{
		uint16_t c = s_Fb[i];
		unsigned char rgb[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2), (unsigned char) ((c & 31) << 3)};
		fwrite (rgb, 1, 3, f);
	}
	fclose (f);
}

int main (int argc, char **argv)
{
	lv_init ();
	lv_display_t *d = lv_display_create (W, H);
	lv_display_set_flush_cb (d, flush);
	lv_display_set_buffers (d, s_Buf, 0, sizeof s_Buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_indev_t *ind = lv_indev_create ();
	lv_indev_set_type (ind, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb (ind, pointer);
	sidecar_ui_create ();
	sidecar_ui_set_command (on_command, 0);
	static const char *const tracks[8] = {"PLAITS", "FM", "JUNO", "DSPS", "DAISY", "DD", "PLAITS", "DSPS"};
	sidecar_ui_status_t s = {1, 123456, 0, {-1, 38, 61, 57}, {0}, "link: usb"};
	for (int i = 0; i < 8; i++) s.track[i] = tracks[i];
	s.notes = 1; s.out_tracks = 0; s.mac = 1; s.drone = -1; s.fifo = 96; s.web = 1; s.rec = 2; s.rec_secs = 83;
	strcpy (s.fx_type[0], "CHORUS"); strcpy (s.fx_type[1], "COMB");
	for (int t = 0; t < SIDECAR_UI_SENDS; t++) { s.send[0][t] = (t * 11) % 128; s.send[1][t] = 127 - (t * 9) % 128; }
	strcpy (s.ip, "169.254.61.60"); strcpy (s.host, "sidecar.local"); strcpy (s.link, "usb"); strcpy (s.gadget, "sidecar");
	strcpy (s.build, "Oct 16 2026 12:00:00"); strcpy (s.circle, "Circle 51.1.1");
	strcpy (s.project, "SHARED SET");
	static const char *const us[5] = {"ACID", "GRAIN", "!WOBBLE", "METAL...", "---"};
	static const unsigned char ust[5] = {1, 1, 2, 6, 0};
	for (int i = 0; i < 5; i++) { strcpy (s.uslot[i], us[i]); s.uslot_state[i] = ust[i]; }
	sidecar_ui_status (&s);
	sidecar_ui_touch (1, 2010, 1240, 392, 145);
	run (10);
	const char *base = argc > 1 ? argv[1] : "obj/shot";
	char name[128];
	for (int p = 0; p < 4; p++)
	{
		sidecar_ui_show (p);
		run (10);
		snprintf (name, sizeof name, "%s%d.ppm", base, p);
		write_ppm (name);
	}
	// taps on the controls: the commands they send (a Sound page layout: tab strip y 60..108, content from y 114)
	sidecar_ui_show (0); run (3);
	tap (714, 25);					// Settings (on the home page: opens the last page, here About)
	tap (140, 84);					// tab: Sound
	tap (100, 114 + 48 + 24 + 0);			// notes: Off (left half of the segment)
	tap (300, 114 + 118 + 24);			// output: Tracks
	tap (300, 114 + 188 + 24);			// computer audio: On
	tap (60, 114 + 258 + 24);			// FIFO -
	tap (350, 114 + 258 + 24);			// FIFO +
	tap (12 + 400 + 12 + 60, 114 + 48 + 24);	// voice 2 selected (then the machine view follows it)
	tap (12 + 400 + 12 + 364 - 20, 114 + 118 + 24);	// machine: DD on voice 2
	tap (12 + 400 + 12 + 40, 114 + 188 + 24);	// drone: Off
	tap (12 + 400 + 12 + 364 / 2, 114 + 188 + 24);	// drone: this voice
	tap (12 + 400 + 12 + 364 - 20, 114 + 188 + 24);	// drone: all
	tap (400, 84);					// tab: Send FX (the middle one)
	tap (674, 114 + 52);				// Kill tails
	tap (714, 25);					// Home
	sidecar_ui_show (0); run (3);
	tap (12 + 12 + 177, 308 + 104 + 24);		// Touch works - hide (the panel gives way to the info panel)
	run (10);
	write_ppm ("obj/shot-hidden.ppm");
	sidecar_ui_show (3); run (10);			// the About page with its Touch test toggle (Hide selected)
	write_ppm ("obj/shot-about-hidden.ppm");
	tap (12 + 190 + 150, 114 + 280 + 24);	// Touch test: Show
	return 0;
}
