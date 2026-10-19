// rmp_pool.cpp - see rmp_pool.h. Placement on the Teensy: rmp_pool_read and rmp_pool_decode run in the
// render path (per block, per frame: ITCM, the default); the rest is job code between requests whose time
// is the card read's (refill) or a few instructions per loop pass (ahead): program flash.
#include "rmp_pool.h"
#include <string.h>
#if defined(__IMXRT1062__)
#define RMP_POOL_COLD __attribute__((section(".flashmem")))
#else
#define RMP_POOL_COLD
#endif

static const uint32_t kBackFrames = 64;   // frames kept below the lowest frame read (interpolators look back)

RMP_POOL_COLD void rmp_pool_attach(rmp_pool_reader *r, uint8_t *head, uint32_t head_cap, uint8_t *ring, uint32_t ring_cap,
                                   uint32_t chunk) {
  memset(r, 0, sizeof *r);
  r->head = head; r->head_cap = head_cap; r->ring = ring; r->ring_cap = ring_cap; r->chunk = chunk;
  r->low_reset = true;
  r->seek = r->wait = r->hi = RMP_POOL_NONE;
}
RMP_POOL_COLD void rmp_pool_clear(rmp_pool_reader *r) {
  r->slot = r->want = r->bad = 0;
  r->hlen = r->rs = r->fill = r->next = r->snext = r->low = r->back = 0;
  r->low_reset = true;
  r->seek = r->wait = r->hi = RMP_POOL_NONE;
}
static uint32_t channels_out(const rmp_pool_reader *r) { return r->src.channels > 2 ? 2u : r->src.channels; }

RMP_POOL_COLD int32_t rmp_pool_info_get(rmp_pool_reader *r, int slot, rmp_pool_info *info) {
  if (slot < 1 || slot > 127 || slot == r->bad) return RMP_POOL_EMPTY;
  if (slot != r->slot || !r->hlen) {
    r->want = slot;
    return RMP_POOL_NOT_READY;
  }
  info->frames = r->src.data_len / r->bpf;
  info->channels = channels_out(r);
  info->rate = r->src.rate;
  info->where = RMP_POOL_HEAD;   // also when the head holds it whole: then head_frames == frames
  info->head_frames = r->hlen / r->bpf;
  return RMP_POOL_OK;
}

RMP_POOL_COLD bool rmp_pool_prepare(rmp_pool_reader *r, int slot, const rmp_pool_src *src, rmp_pool_rd rd, void *ctx) {
  rmp_pool_clear(r);
  uint32_t bytes = src->bits / 8u;
  bool fmt = (src->bits == 16 || src->bits == 24 || src->bits == 32) && src->channels >= 1;
  uint32_t bpf = fmt ? bytes * src->channels : 0;
  uint32_t h = bpf ? (src->data_len < r->head_cap ? src->data_len : r->head_cap) : 0;
  h -= bpf ? h % bpf : 0;
  if (!bpf || !h || !rd(ctx, src->data_offset, r->head, h)) {
    r->bad = slot;
    return false;
  }
  r->src = *src; r->bpf = bpf; r->hlen = h; r->rd = rd; r->ctx = ctx;
  r->slot = slot; r->rs = h; r->fill = 0; r->next = 0; r->snext = h / bpf;
  // the back margin: 64 frames, at most a quarter of what a refill leaves free (so a refill still fits)
  uint32_t room = r->ring_cap > r->chunk ? (r->ring_cap - r->chunk) / 4 : 0;
  uint32_t back = kBackFrames * bpf < room ? kBackFrames * bpf : room;
  r->back = back - back % bpf;
  return true;
}

static float dec(const uint8_t *q, uint32_t bytes) {
  if (bytes == 2) return (int16_t)(q[0] | q[1] << 8) * (1.0f / 32768.0f);
  if (bytes == 3) return (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) * (1.0f / 2147483648.0f);
  return (int32_t)((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24) * (1.0f / 2147483648.0f);
}

int32_t rmp_pool_read(rmp_pool_reader *r, int slot, uint32_t pos, float *buf, int32_t n) {
  if (slot != r->slot || !r->hlen) {
    if (slot >= 1 && slot <= 127 && slot != r->bad) r->want = slot;
    return 0;
  }
  const uint32_t bpf = r->bpf, bytes = r->src.bits / 8u, ch = channels_out(r), frames = r->src.data_len / bpf;
  const uint32_t hf = r->hlen / bpf;
  if (n > 0 && pos < frames) {
    // the frames asked for: the highest decides a re-arm, the lowest one past the head what the ring keeps
    uint32_t e = (uint32_t)n - 1u < frames - pos ? pos + (uint32_t)n - 1u : frames - 1u;
    if (r->hi == RMP_POOL_NONE || e > r->hi) r->hi = e;
    if (e >= hf) {   // a stream read: it states what the stream waits for now (a head read leaves that)
      r->wait = RMP_POOL_NONE;
      uint32_t s = pos > hf ? pos : hf;
      if (r->low_reset || s < r->low) r->low = s;
      r->low_reset = false;
    }
  }
  int32_t i = 0;
  for (; i < n; ++i) {
    uint32_t f = pos + (uint32_t)i;
    if (f >= frames) break;
    uint32_t b = f * bpf;
    const uint8_t *q;
    uint8_t tmp[8];
    if (b + bpf <= r->hlen) {
      q = r->head + b;
    } else if (b >= r->rs && b + bpf <= r->rs + r->fill) {
      uint32_t w = b % r->ring_cap;
      if (w + bpf <= r->ring_cap) {
        q = r->ring + w;
      } else {   // a frame across the ring's end (only the first two channels, <= 8 bytes, are decoded)
        for (uint32_t k = 0; k < bpf && k < sizeof tmp; ++k) tmp[k] = r->ring[(b + k) % r->ring_cap];
        q = tmp;
      }
    } else {
      // ahead of the ring's data within its reach: wait (the next refill moves the ring up to it);
      // anywhere else: restart the stream there
      if (r->seek == RMP_POOL_NONE && b >= r->rs + r->fill && b < r->rs + r->ring_cap) r->wait = f;
      else r->seek = f;
      r->misses += (uint32_t)(n - i);
      break;
    }
    for (uint32_t c = 0; c < ch; ++c) buf[(uint32_t)i * ch + c] = dec(q + c * bytes, bytes);
  }
  if (i > 0) r->next = pos + (uint32_t)i;
  if (i > 0 && pos + (uint32_t)i > hf) r->snext = pos + (uint32_t)i;   // the stream position moved
  return i;
}

int32_t rmp_pool_decode(const uint8_t *pcm, uint32_t frames, uint32_t channels, uint32_t bits, uint32_t pos, float *buf,
                        int32_t n) {
  const uint32_t bytes = bits / 8u, bpf = bytes * channels, ch = channels > 2 ? 2u : channels;
  if ((bits != 16 && bits != 24 && bits != 32) || !channels) return 0;
  int32_t i = 0;
  for (; i < n && pos + (uint32_t)i < frames; ++i) {
    const uint8_t *q = pcm + (pos + (uint32_t)i) * bpf;
    for (uint32_t c = 0; c < ch; ++c) buf[(uint32_t)i * ch + c] = dec(q + c * bytes, bytes);
  }
  return i;
}

RMP_POOL_COLD uint32_t rmp_pool_refill(rmp_pool_reader *r) {
  if (!r->slot || !r->hlen) return 0;
  const uint32_t bpf = r->bpf;
  if (r->seek != RMP_POOL_NONE) {
    r->rs = r->seek * bpf;
    r->fill = 0;
    r->next = r->snext = r->seek;
    if (r->low_reset || r->low > r->seek) r->low = r->seek;
    r->seek = r->wait = RMP_POOL_NONE;
  } else if (r->hi != RMP_POOL_NONE && r->hi < r->hlen / bpf && r->rs != r->hlen) {
    // every read since the last refill stayed in the head (a retrigger, a loop back): re-arm at its end
    r->rs = r->hlen;
    r->fill = 0;
    r->low = r->snext = r->hlen / bpf;
    r->wait = RMP_POOL_NONE;
  }
  r->hi = RMP_POOL_NONE;
  // drop what playback has passed: below the lowest stream frame read since the last refill (it persists
  // until the next stream read), less the back margin, never below the head's end. When that lies past
  // the ring's data (a frame waited for further on), the ring is emptied and starts there.
  uint64_t keep = (uint64_t)r->low * bpf;
  keep = keep > r->back ? keep - r->back : 0;
  if (keep < r->hlen) keep = r->hlen;
  if (keep > r->src.data_len) keep = r->src.data_len - r->src.data_len % bpf;
  if (keep > r->rs) {
    uint32_t d = (uint32_t)keep - r->rs;
    if (d >= r->fill) {
      r->rs = (uint32_t)keep;
      r->fill = 0;
    } else {
      r->rs += d;
      r->fill -= d;
    }
  }
  r->low_reset = true;
  uint32_t off = r->rs + r->fill;
  if (r->fill + r->chunk > r->ring_cap || off >= r->src.data_len) return 0;
  uint32_t n = r->src.data_len - off < r->chunk ? r->src.data_len - off : r->chunk;
  uint32_t w = off % r->ring_cap, a = n < r->ring_cap - w ? n : r->ring_cap - w;
  if (!r->rd(r->ctx, r->src.data_offset + off, r->ring + w, a)) return 0;
  if (n > a && !r->rd(r->ctx, r->src.data_offset + off + a, r->ring, n - a)) return 0;
  r->fill += n;
  return n;
}

RMP_POOL_COLD uint32_t rmp_pool_ahead(const rmp_pool_reader *r) {
  if (!r->slot || !r->hlen) return 0xFFFFFFFFu;
  if (r->seek != RMP_POOL_NONE) return 0;
  const uint32_t hf = r->hlen / r->bpf;
  // the stream position's lead: the ring's data past the frame after the last stream frame delivered
  uint32_t end = r->rs + r->fill, lead;
  if (r->wait != RMP_POOL_NONE && (uint64_t)r->wait * r->bpf + r->bpf > end) return 0;
  if (end >= r->src.data_len) {
    lead = 0xFFFFFFFFu;
  } else {
    uint32_t have = end / r->bpf;
    lead = have > r->snext ? have - r->snext : 0;
  }
  // reads since the last refill and the ring moved on, with a re-arm pending (all of them in the head) or the
  // last read ending in the head (a retrigger after stream reads in the same refill period: this refill
  // closes the period, the next one re-arms): the head frames left, if fewer. Without reads since the last
  // refill (a stopped voice) the refill would make no progress on this: the lead alone.
  if (r->hi != RMP_POOL_NONE && r->rs != r->hlen && (r->hi < hf || r->next < hf)) {
    uint32_t left = hf > r->next ? hf - r->next : 0;
    if (left < lead) lead = left;
  }
  return lead;
}
