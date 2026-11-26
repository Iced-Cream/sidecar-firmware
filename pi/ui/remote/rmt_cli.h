// pi/ui/remote/rmt_cli.h - the app's end of the remote UI protocol (docs/remote-ui.md): the connection's state, the
// decoded copies of the views, subscriptions, commands. No sockets, no clock of its own: the caller supplies a send
// function, feeds the bytes it reads, and ticks it with a millisecond clock. Portable C (the app, the tests).
//
// Life of a connection: rmt_cli_connected (the socket is up) -> HELLO -> WELCOME (-> AUTH when the Pi asks) -> ready:
// the views wanted by rmt_cli_want are subscribed and fill in. rmt_cli_lost (the socket closed, or rmt_cli_feed / tick
// said so) forgets everything the Pi sent: the next connection starts from a full snapshot, no state kept.
#ifndef RMT_CLI_H
#define RMT_CLI_H

#include "rmt_proto.h"
#include "rmt_views.h"
#include "../sidecar_status.h"
#include "../sidecar_voice.h"
#include "../sidecar_insfx.h"
#include "../sidecar_mfx.h"
#include "../sidecar_seqed.h"
#include "rmt_seqview.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RMT_CLI_DEAD_MS	(3 * RMT_PING_S * 1000 + 2000)	// nothing heard this long: the link is gone (three missed pings)

enum { RMT_CLI_IDLE = 0, RMT_CLI_HELLO, RMT_CLI_AUTH, RMT_CLI_READY, RMT_CLI_REFUSED };

typedef struct rmt_cli rmt_cli_t;
// returns 0 on success, -1 when the connection must be closed
typedef int (*rmt_cli_send_fn) (void *ud, const uint8_t *data, size_t n);
typedef struct
{
	rmt_cli_send_fn send;
	void *ud;
	uint32_t app_build;
	unsigned w, h, dpi;
	const char *code;			// the pairing code for a Pi with usbtrust=0 (kept in memory only); 0: none
	void (*on_cmd_result) (void *ud, uint32_t seq, int ok, int code, const char *why);	// 0: ignored
	void (*on_page) (void *ud, int page);						// the Pi's local page changed; 0: ignored
} rmt_cli_cfg_t;

struct rmt_cli
{
	rmt_cli_cfg_t cfg;
	rmt_rx_t rx;
	int state, refused_code, resub;		// refused_code: RMT_E_* of the last refusal, kept across rmt_cli_lost until ready; resub: a SUB did not go out
	uint32_t seq, session, now_ms, last_rx_ms;
	uint8_t nonce[RMT_NONCE];
	int pi_page, pi_proto;
	char fw[24];
	uint8_t offered[RMT_V_COUNT];		// the Pi sends this view (and its schema is compatible)
	uint8_t want[RMT_V_COUNT];		// rate asked of the app (0: not wanted); 255: the view's default
	uint8_t dirty[RMT_V_COUNT];		// a section arrived since rmt_cli_take
	uint32_t n_views;			// VIEW frames applied (tests, stats)
	sidecar_status_view_t status;
	sidecar_voice_view_t voice;
	sidecar_insfx_view_t insfx;
	sidecar_mfx_view_t mfx;
	sidecar_seq_view_t sq;			// SEQ (seq is the message counter)
	rmt_seqpos_view_t seqpos;
	rmt_pat_view_t pat;
};

void rmt_cli_init (rmt_cli_t *c, const rmt_cli_cfg_t *cfg);
void rmt_cli_connected (rmt_cli_t *c, uint32_t now_ms);			// the socket is up: HELLO goes out
void rmt_cli_lost (rmt_cli_t *c);						// back to idle, the views forgotten
int rmt_cli_feed (rmt_cli_t *c, const uint8_t *data, size_t n, uint32_t now_ms);	// 0, or -1: close the connection
int rmt_cli_tick (rmt_cli_t *c, uint32_t now_ms);				// -1: nothing heard for RMT_CLI_DEAD_MS
int rmt_cli_ready (const rmt_cli_t *c);
void rmt_cli_want (rmt_cli_t *c, int view, int rate_hz);			// 0: not wanted (UNSUB); 255: the default rate
int rmt_cli_view_ok (const rmt_cli_t *c, int view);				// the Pi offers it and its schema is one this app reads
void *rmt_cli_view (rmt_cli_t *c, int view);					// the decoded struct (0: unknown view)
int rmt_cli_take (rmt_cli_t *c, int view);					// 1 once per change (clears the flag)
int rmt_cli_cmd (rmt_cli_t *c, const char *line);				// the CMD's seq (>0), 0: not sent (not ready, or a bad line)
void rmt_cli_resync (rmt_cli_t *c);

#ifdef __cplusplus
}
#endif

#endif
