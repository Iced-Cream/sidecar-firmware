/* usb_uac2: USB Audio Class 2 streaming for the Teensy 4 core (rytm-teensy, MIT licence). See usb_uac2.h.
 * Everything here runs in the USB interrupt (or EP0 setup handling inside it) and is FLASHMEM: the
 * host build has little ITCM headroom, and 4 IN + 4 OUT completions per ms run fine from the I-cache. */
#include "usb_dev.h"
#include "usb_uac2.h"
#include "core_pins.h"
#include "avr/pgmspace.h"
#include <string.h>

#if defined(UAC2_INTERFACE)

extern volatile uint8_t usb_high_speed;

volatile struct usb_uac2_stats usb_uac2_stats;
volatile int32_t usb_uac2_fb_offset;
#define ST usb_uac2_stats

#define CLOCK_ID 0x10
// The channels on the wire: UAC2_CH_IN / UAC2_CH_OUT (the build's maximum, the buffers' size) or fewer, by the
// mode (usb_uac2_mode: multi = the maximum, stereo = 2 / 2, off = no audio function). The application's frames
// keep the maximum's layout (in_fill / out_read: UAC2_CH_IN / UAC2_CH_OUT samples a frame, the unused ones 0).
volatile uint8_t usb_uac2_mode;
static uint8_t ch_in = UAC2_CH_IN, ch_out = UAC2_CH_OUT;
#define OUT_FRAME_BYTES (ch_out * UAC2_SUBSLOT)
#define IN_FRAME_BYTES (ch_in * UAC2_SUBSLOT)
#define BUFSZ(n) ((((n) + 31) / 32) * 32)
#ifdef UAC2_HS_ONLY   // buffers for high speed only (RAM2 is short in the host build): full speed gets as much as fits
#define OUT_BUF BUFSZ(UAC2_OUT_SIZE_480)
#define IN_BUF BUFSZ(UAC2_IN_SIZE_480)
#else
#define OUT_BUF BUFSZ(UAC2_OUT_SIZE_12 > UAC2_OUT_SIZE_480 ? UAC2_OUT_SIZE_12 : UAC2_OUT_SIZE_480)
#define IN_BUF BUFSZ(UAC2_IN_SIZE_12 > UAC2_IN_SIZE_480 ? UAC2_IN_SIZE_12 : UAC2_IN_SIZE_480)
#endif
#ifndef UAC2_BUF_ATTR
#define UAC2_BUF_ATTR DMAMEM    // the DMA buffers: RAM2; -DUAC2_BUF_ATTR= puts them in RAM1 (DTCM: the USB DMA reaches it)
#endif
#ifndef UAC2_RING_ATTR
#define UAC2_RING_ATTR DMAMEM   // the OUT ring (CPU only): RAM2; -DUAC2_RING_ATTR= puts it in RAM1
#endif
#define MAXFR (UAC2_FRAMES_12 + 1)

static transfer_t out_tr[UAC2_NQ] __attribute__((aligned(32)));
static transfer_t in_tr[UAC2_NQ] __attribute__((aligned(32)));
static transfer_t fb_tr __attribute__((aligned(32)));
UAC2_BUF_ATTR static uint8_t out_buf[UAC2_NQ][OUT_BUF] __attribute__((aligned(32)));
UAC2_BUF_ATTR static uint8_t in_buf[UAC2_NQ][IN_BUF] __attribute__((aligned(32)));
UAC2_BUF_ATTR static uint32_t fb_buf[8] __attribute__((aligned(32)));
#ifdef UAC2_RING_MEM   // the application's memory (-DUAC2_RING_MEM=name: at least UAC2_RING_FRAMES x UAC2_CH_OUT int32,
extern int32_t UAC2_RING_MEM[];   // its own while usb_uac2_pause(1) holds: the Sidecar Mini's pool with the recorder)
#define ring UAC2_RING_MEM
#else
UAC2_RING_ATTR static int32_t ring[UAC2_RING_FRAMES * UAC2_CH_OUT] __attribute__((aligned(32)));
#endif
volatile uint8_t usb_uac2_paused;

static volatile uint32_t ring_head, ring_tail;   // frames written (USB interrupt) / read (out_read's caller)
volatile uint32_t usb_uac2_in_delay;
volatile uint8_t usb_uac2_ext_consumer;
volatile uint32_t usb_uac2_in_underrun;
#define BARRIER() __asm__ volatile("" ::: "memory")
static uint8_t priming;
static uint32_t nom;                    // frames per packet: 12 (480) or 48 (12)
static uint32_t out_size, in_size;
static uint64_t in_pos, sink_pos;       // device-clock frame positions consumed by IN / by the sink
static uint32_t last_fr, win_uf;
static uint64_t win_q0;
static uint8_t win_started;
static int32_t lvl_q8;                  // smoothed OUT ring level, frames x 256
static uint8_t req_buf[16];

static uint32_t clk_last;
static uint64_t clk_q16, clk_rem;
volatile int32_t usb_uac2_clock_ppm;

// Default device clock: the CPU cycle counter (crystal-derived), F_CPU_ACTUAL / 48000 cycles per frame,
// usb_uac2_clock_ppm fast (debug). Called from the USB interrupt only, at least every SOF, so the 32-bit
// counter never wraps unseen. Accumulated per call with the remainder carried: exact over any run.
FLASHMEM __attribute__((weak)) uint64_t usb_uac2_clock_q16(void)
{
	uint32_t c = ARM_DWT_CYCCNT;
	uint64_t d = (uint32_t)(c - clk_last);
	clk_last = c;
	uint64_t den = (uint64_t)(F_CPU_ACTUAL / UAC2_RATE) * 1000000u;
	uint64_t num = (d << 16) * (uint64_t)(1000000 + usb_uac2_clock_ppm) + clk_rem;
	clk_q16 += num / den;
	clk_rem = num % den;
	return clk_q16;
}

FLASHMEM __attribute__((weak)) void usb_uac2_in_fill(int32_t *dst, unsigned frames)
{
	memset(dst, 0, frames * UAC2_CH_IN * sizeof(int32_t));
}

FLASHMEM __attribute__((weak)) unsigned usb_uac2_in_frames(unsigned nominal)
{
	return 0;
}

FLASHMEM unsigned usb_uac2_out_read(int32_t *dst, unsigned frames)
{
	if (usb_uac2_paused) {   // the ring's memory is lent: silence, nothing counted
		memset(dst, 0, frames * UAC2_CH_OUT * sizeof(int32_t));
		return 0;
	}
	uint32_t level = ring_head - ring_tail;
	unsigned got = 0;
	if (priming && ST.out_alt && level >= UAC2_OUT_TARGET) priming = 0;
	if (!priming) {
		got = frames;
		if (level < frames) {
			got = level;
			ST.out_underrun++;
			priming = 1;
		}
		for (unsigned f = 0; f < got; f++) {
			const int32_t *s = ring + (ring_tail & (UAC2_RING_FRAMES - 1)) * UAC2_CH_OUT;
			for (int c = 0; c < UAC2_CH_OUT; c++) *dst++ = s[c];
			BARRIER();
			ring_tail++;
		}
	}
	if (got < frames) memset(dst, 0, (frames - got) * UAC2_CH_OUT * sizeof(int32_t));
	return got;
}

FLASHMEM void usb_uac2_stats_reset_minmax(void)
{
	__disable_irq();
	ST.out_level_min = 0xFFFFFFFF;
	ST.out_level_max = 0;
	__enable_irq();
}

static FLASHMEM void ring_reset(void)
{
	ring_head = ring_tail = 0;
	priming = 1;
	lvl_q8 = 0;
}

// From the main loop: lend the OUT ring's memory (1) or take it back (0). Both reset the ring (priming: the feedback
// reports the measured clock, uncorrected, while paused); the endpoints stay as they are (the computer keeps the device,
// IN carries what usb_uac2_in_fill gives: the application's silence while paused).
FLASHMEM void usb_uac2_pause(int on)
{
	__disable_irq();
	ring_reset();
	usb_uac2_paused = on ? 1 : 0;
	__enable_irq();
}

static FLASHMEM uint32_t clamp_frames(uint64_t *pos, uint64_t due, uint32_t *clamp, uint32_t *resync)
{
	int64_t d = (int64_t)(due - *pos);
	if (d > (int64_t)(8 * nom) || d < -(int64_t)(8 * nom)) {
		*pos = due - nom;
		d = nom;
		(*resync)++;
	}
	uint32_t n;
	if (d < (int64_t)nom - 1) { n = nom - 1; (*clamp)++; }
	else if (d > (int64_t)nom + 1) { n = nom + 1; (*clamp)++; }
	else n = (uint32_t)d;
	*pos += n;
	return n;
}

static void in_event(transfer_t *t);
static void out_event(transfer_t *t);
static void fb_event(transfer_t *t);

static FLASHMEM void in_queue(int i)
{
	int32_t tmp[MAXFR * UAC2_CH_IN];
	uint64_t due = (usb_uac2_clock_q16() >> 16) - usb_uac2_in_delay;
	uint32_t n = usb_uac2_in_frames(nom);
	if (n) {
		if (n < nom - 1) n = nom - 1;
		if (n > nom + 1) n = nom + 1;
		in_pos += n;
	} else {
		n = clamp_frames(&in_pos, due, (uint32_t *)&ST.in_clamp, (uint32_t *)&ST.in_resync);
	}
	if (n * IN_FRAME_BYTES > in_size) n = in_size / IN_FRAME_BYTES;   // full speed with many channels: capped
	usb_uac2_in_fill(tmp, n);
	uint8_t *p = in_buf[i];
	for (uint32_t f = 0; f < n; f++)
		for (uint32_t k = 0; k < ch_in; k++) {
			int32_t v = tmp[f * UAC2_CH_IN + k];
			*p++ = v; *p++ = v >> 8; *p++ = v >> 16;
		}
	uint32_t len = n * IN_FRAME_BYTES;
	arm_dcache_flush_delete(in_buf[i], BUFSZ(len));
	usb_prepare_transfer(&in_tr[i], in_buf[i], len, i);
	usb_transmit(UAC2_IN_ENDPOINT, &in_tr[i]);
	ST.in_frames += n;
}

static FLASHMEM void in_event(transfer_t *t)
{
	ST.in_pkts++;
	in_queue(t->callback_param);
}

static FLASHMEM void out_queue(int i)
{
	arm_dcache_delete(out_buf[i], OUT_BUF);
	usb_prepare_transfer(&out_tr[i], out_buf[i], out_size, i);
	usb_receive(UAC2_OUT_ENDPOINT, &out_tr[i]);
}

static FLASHMEM void out_event(transfer_t *t)
{
	int i = t->callback_param;
	uint32_t status = t->status;
	uint32_t len = out_size - ((status >> 16) & 0x7FFF);
	if (status & 0x68) ST.out_err++;    // halted, data buffer error, transaction error
	arm_dcache_delete(out_buf[i], OUT_BUF);
	ST.out_pkts++;
	if (len % OUT_FRAME_BYTES) ST.out_badlen++;
	uint32_t n = len / OUT_FRAME_BYTES;
	if (n + 1 == nom) ST.out_hist[0]++;
	else if (n == nom) ST.out_hist[1]++;
	else if (n == nom + 1) ST.out_hist[2]++;
	ST.out_frames += n;
	if (usb_uac2_paused) {   // received and dropped: the ring's memory is lent
		ST.out_paused += n;
		out_queue(i);
		return;
	}
	const uint8_t *p = out_buf[i];
	for (uint32_t f = 0; f < n; f++) {
		if (ring_head - ring_tail >= UAC2_RING_FRAMES) {
			ST.out_overrun += n - f;
			break;
		}
		int32_t *d = ring + (ring_head & (UAC2_RING_FRAMES - 1)) * UAC2_CH_OUT;
		for (int c = ch_out; c < UAC2_CH_OUT; c++) d[c] = 0;
		for (int c = 0; c < ch_out; c++) {
			d[c] = ((int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24))) >> 8;
			p += 3;
		}
		BARRIER();
		ring_head++;
	}
	out_queue(i);
}

static FLASHMEM void fb_event(transfer_t *t)
{
	if (t) ST.fb_sent++;
	int32_t base = ST.fb_base;
	int32_t corr = 0;
	if (ST.out_alt && !priming) {
		// about 1 s to pull the level back: (target - level) frames / 8000 microframes, in 16.16
		corr = (int32_t)(((int64_t)(UAC2_OUT_TARGET * 256 - lvl_q8) * 65536) / (8000 * 256));
		int32_t lim = base / 200;   // +-0.5 %
		if (corr > lim) corr = lim;
		if (corr < -lim) corr = -lim;
	}
	uint32_t v = base + corr + usb_uac2_fb_offset;
	ST.fb = v;
	// High speed: 16.16 frames per data packet interval. macOS (measured, 26.x) ignores a value given per
	// microframe when the data endpoints' bInterval is 2 (it sends nominal packets whatever the value);
	// per packet it follows. Full speed: 10.14 frames per ms in 3 bytes.
	fb_buf[0] = usb_high_speed ? v << (UAC2_BINTERVAL_480 - 1) : v * 2;
	arm_dcache_flush_delete(fb_buf, 32);
	usb_prepare_transfer(&fb_tr, fb_buf, usb_high_speed ? 4 : 3, 0);
	usb_transmit(UAC2_FB_ENDPOINT, &fb_tr);
}

FLASHMEM void usb_uac2_sof(void)
{
	ST.sof++;
	uint32_t fr = USB1_FRINDEX & 0x3FFF;
	uint32_t d = (fr - last_fr) & 0x3FFF;
	last_fr = fr;
	uint64_t q = usb_uac2_clock_q16();
	if (!win_started) {
		win_started = 1;
		win_q0 = q;
		win_uf = 0;
		return;
	}
	ST.uframes += d;
	win_uf += d;
	if (win_uf >= UAC2_FB_WINDOW) {   // device frames per microframe in 16.16
		ST.fb_base = (uint32_t)((q - win_q0) / win_uf);
		win_q0 = q;
		win_uf = 0;
	}
	uint32_t level = ring_head - ring_tail;
	ST.out_level = level;
	if (level < ST.out_level_min) ST.out_level_min = level;
	if (level > ST.out_level_max) ST.out_level_max = level;
	lvl_q8 += ((int32_t)(level << 8) - lvl_q8) >> 6;
	// no IN stream to consume the OUT ring: drain it at the clock's rate so the feedback stays true
	uint64_t due = q >> 16;
	if (!ST.in_alt && ST.out_alt && !usb_uac2_ext_consumer) {
		int64_t dd = (int64_t)(due - sink_pos);
		if (dd > 4096 || dd < 0) sink_pos = due;
		uint32_t n = due - sink_pos;
		sink_pos = due;
		int32_t tmp[UAC2_CH_OUT * 8];
		while (n) {
			uint32_t k = n > 8 ? 8 : n;
			ST.sink_frames += usb_uac2_out_read(tmp, k);
			n -= k;
		}
	} else {
		sink_pos = due;
	}
}

FLASHMEM void usb_uac2_configure(void)
{
	uint8_t hs = usb_high_speed;
	memset((void *)&usb_uac2_stats, 0, sizeof(usb_uac2_stats));
	if (usb_uac2_mode == UAC2_MODE_OFF) return;   // no audio function in the descriptor: nothing to stream
	ST.high_speed = hs;
	ST.configured = 1;
	ST.out_level_min = 0xFFFFFFFF;
	nom = hs ? UAC2_FRAMES_480 : UAC2_FRAMES_12;
	usb_uac2_channels(&ch_in, &ch_out);
	out_size = (nom + 1) * ch_out * UAC2_SUBSLOT;
	in_size = (nom + 1) * ch_in * UAC2_SUBSLOT;
	if (out_size > OUT_BUF) out_size = OUT_BUF;   // UAC2_HS_ONLY at full speed: what fits (no clean stream)
	if (in_size > IN_BUF) in_size = IN_BUF;
	ST.fb_base = 6u << 16;   // 48 kHz nominal per microframe until the first window
	ring_reset();
	win_started = 0;
	uint64_t due = usb_uac2_clock_q16() >> 16;
	in_pos = due - usb_uac2_in_delay - UAC2_NQ * nom;
	sink_pos = due;
	memset(out_tr, 0, sizeof(out_tr));
	memset(in_tr, 0, sizeof(in_tr));
	memset(&fb_tr, 0, sizeof(fb_tr));
	usb_config_rx_iso(UAC2_OUT_ENDPOINT, out_size, 1, out_event);
	for (int i = 0; i < UAC2_NQ; i++) out_queue(i);
	usb_config_tx_iso(UAC2_FB_ENDPOINT, hs ? 4 : 3, 1, fb_event);
	fb_event(NULL);
	usb_config_tx_iso(UAC2_IN_ENDPOINT, in_size, 1, in_event);
	for (int i = 0; i < UAC2_NQ; i++) in_queue(i);
	usb_start_sof_interrupts(UAC2_INTERFACE);
}

FLASHMEM void usb_uac2_channels(uint8_t *in, uint8_t *out)
{
	if (usb_uac2_mode == UAC2_MODE_STEREO) { *in = 2; *out = 2; }
	else { *in = UAC2_CH_IN; *out = UAC2_CH_OUT; }
}

static FLASHMEM void put32le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// usb.c's GET_DESCRIPTOR (configuration, or other speed): the built descriptor (the maximum's) in buf, *len bytes,
// the audio function last from usb_uac2_desc_pos. For the mode: off cuts the function off (3 interfaces fewer);
// otherwise the channel counts, channel configs and the data endpoints' sizes are set from the mode's channels.
FLASHMEM void usb_uac2_config_desc(uint8_t *buf, uint32_t *len, int hs)
{
	extern const uint16_t usb_uac2_desc_pos;
	uint32_t pos = usb_uac2_desc_pos;
	if (usb_uac2_mode == UAC2_MODE_OFF) {
		*len = pos;
		buf[2] = pos & 255; buf[3] = pos >> 8;
		buf[4] -= 3;
		return;
	}
	uint8_t ci, co;
	usb_uac2_channels(&ci, &co);
	uint32_t fr = (hs ? UAC2_FRAMES_480 : UAC2_FRAMES_12) + 1;
	for (uint32_t p = pos; p + 2 <= *len && buf[p] >= 2; p += buf[p]) {
		uint8_t *d = buf + p;
		uint32_t cfg;
		if (d[1] == 0x24 && d[2] == 0x02 && d[0] == 17) {          // input terminal: 0x01 = USB OUT, 0x03 = the IN path
			uint8_t n = d[3] == 0x01 ? co : ci;
			cfg = n <= 2 ? (1u << n) - 1 : 0;
			d[8] = n; put32le(d + 9, cfg);
		} else if (d[1] == 0x24 && d[2] == 0x01 && d[0] == 16) {   // AS general: terminal 0x01 = OUT, 0x04 = IN
			uint8_t n = d[3] == 0x01 ? co : ci;
			cfg = n <= 2 ? (1u << n) - 1 : 0;
			d[10] = n; put32le(d + 11, cfg);
		} else if (d[1] == 5 && (d[3] & 3) == 1 && !(d[3] & 0x10)) {   // iso data endpoint (not the feedback one)
			uint32_t sz = fr * ((d[2] & 0x80) ? ci : co) * UAC2_SUBSLOT;
			if (sz > 1023 && !hs) sz = 1023;
			d[4] = sz & 255; d[5] = sz >> 8;
		}
	}
}

// Soft detach (no pull-up: the computer sees the device go) and attach again; the computer then enumerates it
// with the mode's descriptor. usb_configuration 0 while away: Serial and MIDI writes return at once.
FLASHMEM void usb_uac2_detach(void)
{
	extern volatile uint8_t usb_configuration;
	__disable_irq();
	USB1_USBCMD &= ~USB_USBCMD_RS;
	usb_configuration = 0;
	ST.in_alt = ST.out_alt = 0;
	ST.configured = 0;
	ring_reset();
	__enable_irq();
}

FLASHMEM void usb_uac2_attach(void)
{
	USB1_USBCMD |= USB_USBCMD_RS;
}

FLASHMEM int usb_uac2_set_interface(uint16_t iface, uint16_t alt)
{
	if (alt > 1) return 0;
	if (iface == UAC2_INTERFACE) return alt == 0;
	if (iface == UAC2_INTERFACE + 1) {
		ST.out_alt = alt;
		ring_reset();
		return 1;
	}
	if (iface == UAC2_INTERFACE + 2) {
		ST.in_alt = alt;
		return 1;
	}
	return 0;
}

FLASHMEM int usb_uac2_get_interface(uint16_t iface)
{
	if (iface == UAC2_INTERFACE) return 0;
	if (iface == UAC2_INTERFACE + 1) return ST.out_alt;
	if (iface == UAC2_INTERFACE + 2) return ST.in_alt;
	return -1;
}

static FLASHMEM void put32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

FLASHMEM const uint8_t *usb_uac2_get_request(uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint32_t *len)
{
	uint32_t n = 0;
	if ((wIndex & 0xFF) == UAC2_INTERFACE && (wIndex >> 8) == CLOCK_ID) {
		uint8_t cs = wValue >> 8;
		if (bRequest == 1 && cs == 1) { put32(req_buf, UAC2_RATE); n = 4; }             // CUR sampling freq
		else if (bRequest == 1 && cs == 2) { req_buf[0] = 1; n = 1; }                   // CUR clock valid
		else if (bRequest == 2 && cs == 1) {                                           // RANGE: one subrange
			req_buf[0] = 1; req_buf[1] = 0;
			put32(req_buf + 2, UAC2_RATE); put32(req_buf + 6, UAC2_RATE); put32(req_buf + 10, 0);
			n = 14;
		}
	}
	if (!n) { ST.bad_req++; return NULL; }
	ST.get_req++;
	if (n > wLength) n = wLength;
	*len = n;
	return req_buf;
}

FLASHMEM int usb_uac2_set_request_ok(uint16_t wValue, uint16_t wIndex)
{
	return (wIndex & 0xFF) == UAC2_INTERFACE && (wIndex >> 8) == CLOCK_ID && (wValue >> 8) == 1;
}

FLASHMEM void usb_uac2_set_request(uint16_t wValue, uint16_t wIndex, const uint8_t *data, uint16_t len)
{
	if (usb_uac2_set_request_ok(wValue, wIndex)) ST.set_cur++;   // the clock is fixed: accept, ignore
}

#endif
