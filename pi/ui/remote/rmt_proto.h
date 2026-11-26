// pi/ui/remote/rmt_proto.h - the remote UI's wire protocol v1 (docs/remote-ui.md): frames, message payloads, the byte
// reassembler. Portable C (no LVGL, no Circle, no libc beyond string.h/stdint.h), shared by the Pi's service
// (pi/engine/remote_srv.cpp) and the app (rmt_cli.c). All integers little-endian; every payload is length-prefixed
// by the frame, and a reader ignores trailing bytes it does not know (append-only messages).
//
//   frame:  u8 type | u8 flags | u16 len | u32 seq | payload[len]      (TCP: no CRC; flags = 0, reserved)
//
//   HELLO    app -> Pi   "SCRM" | u16 proto_min | u16 proto_max | u32 app_build | u32 schema_hash | u16 w | u16 h | u16 dpi
//   WELCOME  Pi -> app   u16 proto | u16 flags (bit0: AUTH needed) | u32 session | u8 nonce[16] | u32 schema_hash |
//                        u8 page (the Pi's page now) | u8 fwlen | fw[fwlen] | u8 nviews | nviews x (u8 view | u16 schema | u8 rate_hz)
//   AUTH     app -> Pi   u8 hmac[32] = HMAC-SHA256(pairing code, nonce || session LE)   (only when WELCOME said so)
//   SUB      app -> Pi   u8 view | u8 rate_hz (0: the view's default)      UNSUB  u8 view
//   VIEW     Pi -> app   u8 view | u8 section | u8 instance | u8 reserved | section data
//   CMD      app -> Pi   a console line (<= 63 chars, no control characters); answered by OK or ERR with the CMD's seq
//   OK       Pi -> app   u32 seq acknowledged         ERR  u32 seq | u16 code | text
//   PING / PONG          u32 token (the PONG repeats it); the Pi pings every 5 s
//   RESYNC   either way  the sender forgets what it sent: the Pi sends every section again
//   PAGE     Pi -> app   u8 page: the Pi's local page changed (sidecar_ui's page number)
//   BYE      either way  u16 code | text: the connection is closed after it
#ifndef RMT_PROTO_H
#define RMT_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RMT_PROTO		1
#define RMT_HDR			8
#define RMT_MAX_PAYLOAD		2048
#define RMT_MAX_FRAME		(RMT_HDR + RMT_MAX_PAYLOAD)
#define RMT_PORT		7340
#define RMT_CMD_MAX		63
#define RMT_PING_S		5
#define RMT_NONCE		16

enum
{
	RMT_HELLO = 1, RMT_WELCOME = 2, RMT_AUTH = 3, RMT_SUB = 4, RMT_UNSUB = 5, RMT_VIEW = 6, RMT_CMD = 7, RMT_OK = 8,
	RMT_ERR = 9, RMT_PING = 10, RMT_PONG = 11, RMT_RESYNC = 12, RMT_PAGE = 13, RMT_BYE = 14
};

// ERR / BYE codes
enum
{
	RMT_E_BAD = 1,		// a message this side cannot parse, or one that is not allowed now
	RMT_E_CMD = 2,		// the command is not on the allowlist
	RMT_E_AUTH = 3,		// wrong or missing proof
	RMT_E_VERSION = 4,	// no common protocol version
	RMT_E_BUSY = 5,		// another client is connected
	RMT_E_VIEW = 6,		// unknown view
	RMT_E_RATE = 7,		// too many commands
	RMT_E_TIMEOUT = 8	// no PONG
};

typedef struct { uint8_t type, flags; uint16_t len; uint32_t seq; } rmt_hdr_t;

// little-endian writer / reader over a buffer; a write past the end or a read past the end sets err and does nothing
typedef struct { uint8_t *p; size_t cap, n; int err; } rmt_w;
typedef struct { const uint8_t *p; size_t len, n; int err; } rmt_r;

void rmt_w_init (rmt_w *w, uint8_t *buf, size_t cap);
void rmt_w_u8 (rmt_w *w, unsigned v);
void rmt_w_u16 (rmt_w *w, unsigned v);
void rmt_w_u32 (rmt_w *w, uint32_t v);
void rmt_w_bytes (rmt_w *w, const void *p, size_t n);
void rmt_r_init (rmt_r *r, const uint8_t *buf, size_t len);
unsigned rmt_r_u8 (rmt_r *r);			// a read past the end gives 0 and sets err
unsigned rmt_r_u16 (rmt_r *r);
uint32_t rmt_r_u32 (rmt_r *r);
void rmt_r_bytes (rmt_r *r, void *out, size_t n);
size_t rmt_r_left (const rmt_r *r);

// a whole frame into buf (cap >= RMT_HDR + len); the bytes written, 0 when it does not fit
size_t rmt_frame (uint8_t *buf, size_t cap, int type, uint32_t seq, const void *payload, size_t len);
// the header at p (>= RMT_HDR bytes)
void rmt_hdr_parse (const uint8_t *p, rmt_hdr_t *h);

// The stream reassembler: bytes in as they arrive, whole frames out. A length above RMT_MAX_PAYLOAD is a protocol error
// (err = 1, nothing more is parsed: the caller closes the connection).
typedef struct { uint8_t buf[RMT_MAX_FRAME]; size_t n; int err; } rmt_rx_t;
void rmt_rx_init (rmt_rx_t *rx);
// Appends up to nlen bytes, returns how many were taken (all, unless a frame is ready: call rmt_rx_next first, then again)
size_t rmt_rx_put (rmt_rx_t *rx, const uint8_t *data, size_t nlen);
// 1: a frame is complete: *h and *payload (valid until the next put/next) set, and it is consumed on the next call
int rmt_rx_next (rmt_rx_t *rx, rmt_hdr_t *h, const uint8_t **payload);

// FNV-1a, the delta's section hash and the schema hash
uint32_t rmt_fnv (uint32_t h, const void *p, size_t n);
#define RMT_FNV_INIT 2166136261u

// a command line fit to send: 1..RMT_CMD_MAX printable ASCII (0x20..0x7E)
int rmt_cmd_ok (const char *s, size_t n);

#ifdef __cplusplus
}
#endif

#endif
