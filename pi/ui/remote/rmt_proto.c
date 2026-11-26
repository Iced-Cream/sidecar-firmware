// pi/ui/remote/rmt_proto.c - see rmt_proto.h
#include "rmt_proto.h"
#include <string.h>

void rmt_w_init (rmt_w *w, uint8_t *buf, size_t cap) { w->p = buf, w->cap = cap, w->n = 0, w->err = 0; }
void rmt_w_bytes (rmt_w *w, const void *p, size_t n)
{
	if (w->err || w->n + n > w->cap) { w->err = 1; return; }
	if (n) memcpy (w->p + w->n, p, n);
	w->n += n;
}
void rmt_w_u8 (rmt_w *w, unsigned v) { uint8_t b = (uint8_t) v; rmt_w_bytes (w, &b, 1); }
void rmt_w_u16 (rmt_w *w, unsigned v) { uint8_t b[2] = {(uint8_t) v, (uint8_t) (v >> 8)}; rmt_w_bytes (w, b, 2); }
void rmt_w_u32 (rmt_w *w, uint32_t v)
{
	uint8_t b[4] = {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)};
	rmt_w_bytes (w, b, 4);
}

void rmt_r_init (rmt_r *r, const uint8_t *buf, size_t len) { r->p = buf, r->len = len, r->n = 0, r->err = 0; }
void rmt_r_bytes (rmt_r *r, void *out, size_t n)
{
	if (r->err || r->n + n > r->len) { r->err = 1; memset (out, 0, n); return; }
	memcpy (out, r->p + r->n, n);
	r->n += n;
}
unsigned rmt_r_u8 (rmt_r *r) { uint8_t b = 0; rmt_r_bytes (r, &b, 1); return b; }
unsigned rmt_r_u16 (rmt_r *r) { uint8_t b[2]; rmt_r_bytes (r, b, 2); return (unsigned) b[0] | (unsigned) b[1] << 8; }
uint32_t rmt_r_u32 (rmt_r *r)
{
	uint8_t b[4];
	rmt_r_bytes (r, b, 4);
	return (uint32_t) b[0] | (uint32_t) b[1] << 8 | (uint32_t) b[2] << 16 | (uint32_t) b[3] << 24;
}
size_t rmt_r_left (const rmt_r *r) { return r->len - r->n; }

size_t rmt_frame (uint8_t *buf, size_t cap, int type, uint32_t seq, const void *payload, size_t len)
{
	if (len > RMT_MAX_PAYLOAD || cap < RMT_HDR + len) return 0;
	rmt_w w;
	rmt_w_init (&w, buf, cap);
	rmt_w_u8 (&w, (unsigned) type), rmt_w_u8 (&w, 0), rmt_w_u16 (&w, (unsigned) len), rmt_w_u32 (&w, seq);
	rmt_w_bytes (&w, payload, len);
	return w.err ? 0 : w.n;
}

void rmt_hdr_parse (const uint8_t *p, rmt_hdr_t *h)
{
	h->type = p[0], h->flags = p[1], h->len = (uint16_t) (p[2] | p[3] << 8);
	h->seq = (uint32_t) p[4] | (uint32_t) p[5] << 8 | (uint32_t) p[6] << 16 | (uint32_t) p[7] << 24;
}

void rmt_rx_init (rmt_rx_t *rx) { rx->n = 0, rx->err = 0; }

static size_t frame_len (const rmt_rx_t *rx)	// the whole frame's length once its header is in, else 0
{
	if (rx->n < RMT_HDR) return 0;
	rmt_hdr_t h;
	rmt_hdr_parse (rx->buf, &h);
	return RMT_HDR + h.len;
}

size_t rmt_rx_put (rmt_rx_t *rx, const uint8_t *data, size_t nlen)
{
	size_t took = 0;
	while (took < nlen && !rx->err)
	{
		size_t want = rx->n < RMT_HDR ? RMT_HDR - rx->n : frame_len (rx) - rx->n;
		if (want == 0) break;				// a complete frame waits: the caller takes it first
		if (want > nlen - took) want = nlen - took;
		memcpy (rx->buf + rx->n, data + took, want);
		rx->n += want, took += want;
		if (rx->n >= RMT_HDR)
		{
			rmt_hdr_t h;
			rmt_hdr_parse (rx->buf, &h);
			if (h.len > RMT_MAX_PAYLOAD) { rx->err = 1; break; }
		}
	}
	return took;
}

int rmt_rx_next (rmt_rx_t *rx, rmt_hdr_t *h, const uint8_t **payload)
{
	static const uint8_t kNone = 0;
	size_t fl = frame_len (rx);
	if (rx->err || fl == 0 || rx->n < fl) return 0;
	rmt_hdr_parse (rx->buf, h);
	*payload = h->len ? rx->buf + RMT_HDR : &kNone;
	// put never takes a byte past a frame's end, so the buffer is empty after it; the payload stays valid until the next put
	rx->n = 0;
	return 1;
}

uint32_t rmt_fnv (uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *) p;
	for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
	return h;
}

int rmt_cmd_ok (const char *s, size_t n)
{
	if (n == 0 || n > RMT_CMD_MAX) return 0;
	for (size_t i = 0; i < n; i++) if ((unsigned char) s[i] < 0x20 || (unsigned char) s[i] > 0x7E) return 0;
	return 1;
}
