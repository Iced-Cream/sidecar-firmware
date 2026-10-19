// pi/ui/sidecar_ui.h - the Pi display's screen (docs/pi-display.md): LVGL only, no Circle, so the same file builds
// in the Pi kernel and on the Mac (pi/ui/host: a PNG of the layout). The caller owns LVGL (lv_init, the display,
// the input device, lv_timer_handler) and calls these from that one thread.
#ifndef SIDECAR_UI_H
#define SIDECAR_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#define SIDECAR_UI_TRACKS	8
#define SIDECAR_UI_CORES	4
#define SIDECAR_UI_SENDS	12		// the Rytm's tracks, each with a send level to FX 1 and FX 2

typedef struct
{
	int		rytm;				// 1: the Rytm is enumerated
	unsigned	blocks;				// Overbridge blocks seen
	unsigned	underruns;
	int		load[SIDECAR_UI_CORES];		// percent per core, -1: not known
	const char	*track[SIDECAR_UI_TRACKS];	// voice/machine names, 0: "-"
	const char	*note;				// a free line at the bottom (build, link), 0: none
	// the settings page's state (the web page's /status): re-read by the caller, so a change from the web page, the
	// serial console or this page shows here
	int		notes;				// MIDI notes on
	int		out_tracks;			// 0: out=main, 1: out=tracks
	int		mac;				// computer audio: 0/1, -1: no USB-C Sidecar gadget
	int		drone;				// -1 off, 0..7 one voice, 8 all
	unsigned	fifo;				// FIFO target, frames
	int		web;				// the web page runs (cmdline web=, not switchable at run time)
	char		fx_type[2][12];			// FX 1 / FX 2 TYPE names ("OFF" when none)
	unsigned char	fx_post[2];			// 1 = returned POST (0/1, clean), 0 = PRE (2/3, through the analog dist + comp)
	unsigned char	send[2][SIDECAR_UI_SENDS];	// 0..127: track t's send level to FX 1 / FX 2 (the kit's)
	char		ip[20], host[40];		// "169.254.61.60", "sidecar.local" ("" until mDNS runs)
	char		link[12], gadget[12];		// "usb", "sidecar"
	char		build[48], circle[24];		// "0.1.0 (1332761)" (core/version.h), "Circle 51.1.1"
	int		touch_hidden;			// 1: the home page's touch test is hidden (touchtest=0, /.rytm/display.txt)
	int		screensaver;			// minutes without a touch until black (0: never), -1: not known
	int		brightness;			// 10..100 %: the picture dimmed (the panel has no backlight control); 0: not known
	// the user machines (core/user/um.h UiView): the Rytm's project and U1..U5 as the Rytm shows them
	char		project[24];			// "" until the engine has published
	char		uslot[5][12];			// "ACID", "---" empty, "!ACID" missing, "ACID..." loading
	unsigned char	uslot_state[5];			// 0 empty 1 ready 6 loading, else a problem (rmp_slot_state)
	int		rec;				// the recorder: 0 off, 1 armed (waits for PLAY), 2 recording
	unsigned	rec_secs;			// the take's length so far
} sidecar_ui_status_t;

// A command line as the serial console takes it (":notes=1", without the colon): the caller runs it (CConsole::Command)
// and re-reads its state shortly after. Called from sidecar_ui's event handlers, on the thread that calls LVGL.
typedef void (*sidecar_ui_command_t) (const char *cmd, void *param);
void sidecar_ui_set_command (sidecar_ui_command_t fn, void *param);
// 0 home, 1 settings/Sound, 2 settings/Send FX, 3 settings/About (the host render shows each)
void sidecar_ui_show (int page);

void sidecar_ui_create (void);					// on lv_screen_active ()
void sidecar_ui_status (const sidecar_ui_status_t *s);		// labels change only when their text does
void sidecar_ui_touch (int pressed, int raw_x, int raw_y, int x, int y);	// the calibration readout
// The picture's brightness, 10..100 %: a black layer over everything (software dimming: the 5" HDMI (H) panel's
// backlight has no software control without a hardware change). Applied at once; 100: no layer.
void sidecar_ui_dim (int percent);

#ifdef __cplusplus
}
#endif

#endif
