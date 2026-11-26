// pi/ui/remote/rmt_sha256.h - SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104) for the remote UI's pairing proof. Portable C.
#ifndef RMT_SHA256_H
#define RMT_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t total; size_t n; } rmt_sha256_t;
void rmt_sha256_init (rmt_sha256_t *c);
void rmt_sha256_update (rmt_sha256_t *c, const void *p, size_t n);
void rmt_sha256_final (rmt_sha256_t *c, uint8_t out[32]);
void rmt_hmac_sha256 (const void *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
// the proof for a pairing code: HMAC (code, nonce[16] || session as 4 bytes LE)
void rmt_auth_proof (const char *code, const uint8_t nonce[16], uint32_t session, uint8_t out[32]);
int rmt_ct_equal (const uint8_t *a, const uint8_t *b, size_t n);	// constant time: 1 when equal

#ifdef __cplusplus
}
#endif

#endif
