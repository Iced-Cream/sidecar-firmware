// pi/ui/remote/rmt_cli.c - see rmt_cli.h
#include "rmt_cli.h"
#include "rmt_sha256.h"
#include <string.h>

static int put (rmt_cli_t *c, int type, uint32_t seq, const void *p, size_t n)
{
	uint8_t buf[RMT_MAX_FRAME];
	size_t fl = rmt_frame (buf, sizeof buf, type, seq, p, n);
	return fl && c->cfg.send (c->cfg.ud, buf, fl) == 0 ? 0 : -1;
}

void rmt_cli_init (rmt_cli_t *c, const rmt_cli_cfg_t *cfg)
{
	memset (c, 0, sizeof *c);
	c->cfg = *cfg;
}

void rmt_cli_lost (rmt_cli_t *c)
{
	rmt_cli_cfg_t cfg = c->cfg;
	uint8_t want[RMT_V_COUNT];
	const int refused = c->refused_code;			// why the Sidecar said no stays for the overlay, until a connection is ready
	memcpy (want, c->want, sizeof want);
	memset (c, 0, sizeof *c);
	c->cfg = cfg;
	c->refused_code = refused;
	memcpy (c->want, want, sizeof want);
}

void rmt_cli_connected (rmt_cli_t *c, uint32_t now_ms)
{
	rmt_cli_lost (c);
	c->now_ms = c->last_rx_ms = now_ms;
	uint8_t b[32];
	rmt_w w;
	rmt_w_init (&w, b, sizeof b);
	rmt_w_bytes (&w, "SCRM", 4);
	rmt_w_u16 (&w, RMT_PROTO), rmt_w_u16 (&w, RMT_PROTO), rmt_w_u32 (&w, c->cfg.app_build), rmt_w_u32 (&w, rmt_schema_hash ());
	rmt_w_u16 (&w, c->cfg.w), rmt_w_u16 (&w, c->cfg.h), rmt_w_u16 (&w, c->cfg.dpi);
	c->state = RMT_CLI_HELLO;
	if (put (c, RMT_HELLO, ++c->seq, b, w.n) != 0) c->state = RMT_CLI_REFUSED;
}

int rmt_cli_ready (const rmt_cli_t *c) { return c->state == RMT_CLI_READY; }

static int send_sub (rmt_cli_t *c, int view)
{
	uint8_t b[2] = {(uint8_t) view, c->want[view] == 255 ? 0 : c->want[view]};
	int r = put (c, RMT_SUB, ++c->seq, b, 2);
	if (r != 0) c->resub = 1;				// not sent: every wanted view is asked again at the next tick
	return r;
}

void rmt_cli_want (rmt_cli_t *c, int view, int rate_hz)
{
	if (view < 1 || view >= RMT_V_COUNT) return;
	uint8_t was = c->want[view];
	c->want[view] = (uint8_t) rate_hz;
	if (c->state != RMT_CLI_READY || !c->offered[view] || was == c->want[view]) return;
	if (rate_hz == 0)
	{
		uint8_t b = (uint8_t) view;
		if (put (c, RMT_UNSUB, ++c->seq, &b, 1) != 0) c->resub = 1;
	}
	else send_sub (c, view);
}

int rmt_cli_view_ok (const rmt_cli_t *c, int view) { return view >= 1 && view < RMT_V_COUNT && c->offered[view]; }

void *rmt_cli_view (rmt_cli_t *c, int view)
{
	switch (view)
	{
	case RMT_V_STATUS: return &c->status;
	case RMT_V_VOICE: return &c->voice;
	case RMT_V_INSFX: return &c->insfx;
	case RMT_V_MFX: return &c->mfx;
	case RMT_V_SEQ: return &c->sq;
	case RMT_V_SEQPOS: return &c->seqpos;
	case RMT_V_PAT: return &c->pat;
	}
	return 0;
}

int rmt_cli_take (rmt_cli_t *c, int view)
{
	if (view < 1 || view >= RMT_V_COUNT || !c->dirty[view]) return 0;
	c->dirty[view] = 0;
	return 1;
}

int rmt_cli_cmd (rmt_cli_t *c, const char *line)
{
	size_t n = strlen (line);
	if (c->state != RMT_CLI_READY || !rmt_cmd_ok (line, n)) return 0;
	uint32_t s = ++c->seq;
	return put (c, RMT_CMD, s, line, n) == 0 ? (int) s : 0;
}

void rmt_cli_resync (rmt_cli_t *c)
{
	if (c->state == RMT_CLI_READY) put (c, RMT_RESYNC, ++c->seq, 0, 0);
}

static int on_welcome (rmt_cli_t *c, rmt_r *r)
{
	unsigned proto = rmt_r_u16 (r), flags = rmt_r_u16 (r);
	c->session = rmt_r_u32 (r);
	rmt_r_bytes (r, c->nonce, RMT_NONCE);
	rmt_r_u32 (r);						// the Pi's schema hash: informational, the per-view schema decides
	c->pi_page = (int) rmt_r_u8 (r);
	unsigned fwlen = rmt_r_u8 (r);
	char fw[256] = {0};
	rmt_r_bytes (r, fw, fwlen);
	fw[sizeof c->fw - 1] = 0;
	memcpy (c->fw, fw, sizeof c->fw - 1);
	unsigned nviews = rmt_r_u8 (r);
	for (unsigned i = 0; i < nviews && !r->err; i++)
	{
		unsigned id = rmt_r_u8 (r), schema = rmt_r_u16 (r);
		rmt_r_u8 (r);
		const rmt_view_info_t *vi = rmt_view_info ((int) id);
		if (vi && (vi->schema >> 8) == (schema >> 8)) c->offered[id] = 1;	// an unknown view or another major: greyed
	}
	if (r->err) return -1;
	c->pi_proto = (int) proto;
	if (proto != RMT_PROTO) { c->state = RMT_CLI_REFUSED, c->refused_code = RMT_E_VERSION; return -1; }
	if (flags & 1)
	{
		if (!c->cfg.code || !*c->cfg.code) { c->state = RMT_CLI_REFUSED, c->refused_code = RMT_E_AUTH; return -1; }
		uint8_t proof[32];
		rmt_auth_proof (c->cfg.code, c->nonce, c->session, proof);
		c->state = RMT_CLI_AUTH;
		return put (c, RMT_AUTH, ++c->seq, proof, sizeof proof);
	}
	return 0;
}

static void subscribe_all (rmt_cli_t *c)
{
	c->state = RMT_CLI_READY;
	c->refused_code = 0;
	for (int v = 1; v < RMT_V_COUNT; v++)
		if (c->want[v] && c->offered[v]) send_sub (c, v);
}

static int on_frame (rmt_cli_t *c, const rmt_hdr_t *h, const uint8_t *p)
{
	rmt_r r;
	rmt_r_init (&r, p, h->len);
	switch (h->type)
	{
	case RMT_WELCOME:
		if (c->state != RMT_CLI_HELLO) return -1;
		if (on_welcome (c, &r) != 0) return -1;
		if (c->state == RMT_CLI_HELLO) subscribe_all (c);	// no proof asked: ready now
		return 0;
	case RMT_OK:
		if (c->state == RMT_CLI_AUTH) { subscribe_all (c); return 0; }
		if (c->cfg.on_cmd_result) c->cfg.on_cmd_result (c->cfg.ud, rmt_r_u32 (&r), 1, 0, "");
		return 0;
	case RMT_ERR:
	{
		uint32_t seq = rmt_r_u32 (&r);
		unsigned code = rmt_r_u16 (&r);
		char why[64] = {0};
		size_t n = rmt_r_left (&r);
		rmt_r_bytes (&r, why, n < sizeof why - 1 ? n : sizeof why - 1);
		if (c->state == RMT_CLI_AUTH) { c->state = RMT_CLI_REFUSED, c->refused_code = (int) code; return -1; }
		if (c->cfg.on_cmd_result) c->cfg.on_cmd_result (c->cfg.ud, seq, 0, (int) code, why);
		return 0;
	}
	case RMT_VIEW:
	{
		if (c->state != RMT_CLI_READY) return -1;
		int view = (int) rmt_r_u8 (&r), sec = (int) rmt_r_u8 (&r), inst = (int) rmt_r_u8 (&r);
		rmt_r_u8 (&r);
		void *v = rmt_cli_view (c, view);
		if (!v || !c->offered[view] || r.err) return 0;		// an unknown view: ignored
		if (rmt_sec_decode (view, sec, inst, v, p + r.n, (int) rmt_r_left (&r)) == 0) c->dirty[view] = 1, c->n_views++;
		return 0;
	}
	case RMT_PING:
		return put (c, RMT_PONG, h->seq, p, h->len > 4 ? 4 : h->len);
	case RMT_PONG:
		return 0;
	case RMT_PAGE:
		c->pi_page = (int) rmt_r_u8 (&r);
		if (c->cfg.on_page) c->cfg.on_page (c->cfg.ud, c->pi_page);
		return 0;
	case RMT_RESYNC:
		return 0;
	case RMT_BYE:
		c->refused_code = (int) rmt_r_u16 (&r);
		if (c->state != RMT_CLI_READY) c->state = RMT_CLI_REFUSED;
		return -1;
	default:
		return 0;						// a message this app does not know: ignored
	}
}

int rmt_cli_feed (rmt_cli_t *c, const uint8_t *data, size_t n, uint32_t now_ms)
{
	c->now_ms = c->last_rx_ms = now_ms;
	while (n > 0)
	{
		size_t took = rmt_rx_put (&c->rx, data, n);
		data += took, n -= took;
		if (c->rx.err) return -1;
		rmt_hdr_t h;
		const uint8_t *p;
		while (rmt_rx_next (&c->rx, &h, &p))
			if (on_frame (c, &h, p) != 0) return -1;
		if (took == 0) return -1;				// (cannot happen: a frame is taken before the next put)
	}
	return 0;
}

int rmt_cli_tick (rmt_cli_t *c, uint32_t now_ms)
{
	c->now_ms = now_ms;
	if (c->state == RMT_CLI_IDLE || c->state == RMT_CLI_REFUSED) return 0;
	if (c->resub && c->state == RMT_CLI_READY)		// a SUB / UNSUB that did not go out: say it all again
	{
		c->resub = 0;
		for (int v = 1; v < RMT_V_COUNT; v++)
		{
			if (!c->offered[v]) continue;
			if (c->want[v]) send_sub (c, v);
			else { uint8_t b = (uint8_t) v; if (put (c, RMT_UNSUB, ++c->seq, &b, 1) != 0) c->resub = 1; }
		}
	}
	return (uint32_t) (now_ms - c->last_rx_ms) > RMT_CLI_DEAD_MS ? -1 : 0;
}
