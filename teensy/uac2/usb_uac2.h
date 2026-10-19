/* usb_uac2: USB Audio Class 2 streaming for the Teensy 4 core (rytm-teensy, MIT licence).
 * Built only for USB type USB_SERIAL_UAC2 (usb_desc.h). Async, one fixed 48 kHz clock, 24-bit samples in
 * 3-byte subslots, explicit feedback for the OUT (host to Teensy) endpoint.
 *
 * The device clock sets the rate both ways. IN packets carry as many frames as the clock advanced
 * (nominal +-1), and the feedback endpoint reports the clock's frames per packet interval measured against
 * the host's SOFs, corrected by the OUT ring's fill level. The default clock is the CPU cycle counter
 * (12500 cycles per frame at 600 MHz); a build overrides usb_uac2_clock_q16() with its real audio clock.
 *
 * Application hooks (weak, called from the USB interrupt):
 *   usb_uac2_in_fill(dst, frames): write frames x UAC2_CH_IN interleaved samples (24-bit, right-aligned
 *     in int32) for the next IN packet; the default sends silence.
 * Application calls (from the USB interrupt or one of the same priority - the host build's USB host interrupt):
 *   usb_uac2_out_read(dst, frames): frames x UAC2_CH_OUT samples from the OUT ring; zeros while the ring
 *     primes or on underrun. Returns the number of real frames.
 */
#pragma once
#include "usb_desc.h"
#include <stdint.h>

#if defined(UAC2_INTERFACE)

#ifndef UAC2_RING_FRAMES
#define UAC2_RING_FRAMES 512   // OUT ring, power of 2
#endif
#ifndef UAC2_OUT_TARGET
#define UAC2_OUT_TARGET 48     // OUT ring fill the feedback steers to (frames, 1 ms)
#endif
#define UAC2_NQ 4               // transfers queued per streaming endpoint (4 x 250 us)
#ifndef UAC2_FB_WINDOW
#define UAC2_FB_WINDOW 8192     // microframes a feedback base measurement spans (1.024 s); a coarse clock wants more
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct usb_uac2_stats {
	uint32_t sof;            // SOF interrupts seen
	uint32_t uframes;        // microframes elapsed by FRINDEX (8 per ms at full speed too)
	uint32_t fb_base;        // measured device frames per microframe, 16.16
	uint32_t fb;             // last feedback value, 16.16 frames per microframe (sent per packet: << (bInterval-1))
	uint32_t fb_sent;        // feedback packets sent
	uint32_t in_pkts, in_frames, in_clamp, in_resync;
	uint32_t out_pkts, out_frames, out_badlen, out_err;
	uint32_t out_hist[3];    // OUT packets of nominal-1, nominal, nominal+1 frames
	uint32_t out_overrun;    // frames dropped, ring full
	uint32_t out_underrun;   // reads that found the ring short (after priming)
	uint32_t out_level;      // ring fill now (frames)
	uint32_t out_level_min, out_level_max; // since the last usb_uac2_stats_reset_minmax()
	uint32_t sink_frames;    // OUT frames dropped at the clock rate while IN is idle
	uint32_t out_paused;     // OUT frames dropped while paused (usb_uac2_pause)
	uint8_t in_alt, out_alt, high_speed, configured;
	uint32_t set_cur, get_req, bad_req;
};
extern volatile struct usb_uac2_stats usb_uac2_stats;
extern volatile int32_t usb_uac2_fb_offset;   // debug: added to every feedback value (16.16)
extern volatile int32_t usb_uac2_clock_ppm;   // debug: the default clock runs this many ppm fast
// IN packets carry the clock's frames up to this many frames ago: room for a source that delivers in bursts
// (the Rytm's Overbridge: 7 or 14 frames a transfer) so usb_uac2_in_fill finds them already there. Default 0.
extern volatile uint32_t usb_uac2_in_delay;
// 1: something else reads the OUT ring (usb_uac2_out_read from another interrupt of the same priority, e.g. the
// host build's Overbridge OUT): the driver's own sink, which drains it while IN is idle, stays off.
extern volatile uint8_t usb_uac2_ext_consumer;
// IN ring-less sources: how many IN frames usb_uac2_in_fill could not fill (the application counts them here).
extern volatile uint32_t usb_uac2_in_underrun;

// The mode (set before usb_init, e.g. in startup_middle_hook, or before a detach/attach): the device's descriptor.
#define UAC2_MODE_MULTI 0    // UAC2_CH_IN in / UAC2_CH_OUT out (the build's channels)
#define UAC2_MODE_STEREO 1   // 2 in / 2 out (the first two of each)
#define UAC2_MODE_OFF 2      // no audio function (Serial, MIDI when built with it)
extern volatile uint8_t usb_uac2_mode;
void usb_uac2_channels(uint8_t *in, uint8_t *out);   // the mode's channel counts
void usb_uac2_config_desc(uint8_t *buf, uint32_t *len, int hs);   // usb.c: the descriptor for the mode
void usb_uac2_detach(void);   // soft disconnect (the computer sees the device go)
void usb_uac2_attach(void);   // and back: it enumerates again
void usb_uac2_configure(void);
int usb_uac2_set_interface(uint16_t iface, uint16_t alt);
int usb_uac2_get_interface(uint16_t iface);
const uint8_t *usb_uac2_get_request(uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint32_t *len);
int usb_uac2_set_request_ok(uint16_t wValue, uint16_t wIndex);
void usb_uac2_set_request(uint16_t wValue, uint16_t wIndex, const uint8_t *data, uint16_t len);
void usb_uac2_sof(void);

uint64_t usb_uac2_clock_q16(void);   // weak: device clock, frames x 65536, monotonic
void usb_uac2_in_fill(int32_t *dst, unsigned frames);   // weak
// weak: the frames for the next IN packet (kept to nominal +-1), or 0 (the default): as the clock advanced. A source
// with a FIFO of its own (the host build) sizes packets by its fill level instead: no jitter from a bursty clock.
unsigned usb_uac2_in_frames(unsigned nominal);
unsigned usb_uac2_out_read(int32_t *dst, unsigned frames);
// Pause (1) / resume (0), from the main loop: while paused the OUT ring is not touched (its memory may be lent, see
// UAC2_RING_MEM in usb_uac2.c): OUT packets are received and dropped, usb_uac2_out_read gives silence. The device
// stays enumerated and streaming; IN is the application's (usb_uac2_in_fill).
void usb_uac2_pause(int on);
extern volatile uint8_t usb_uac2_paused;
void usb_uac2_stats_reset_minmax(void);

#ifdef __cplusplus
}
#endif

#endif
