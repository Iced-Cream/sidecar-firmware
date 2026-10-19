// rmp_pool.h - a voice's reader of a project pool sample for a plugin (pool_info / pool_read, plugin
// API 2), without PSRAM: on the voice's DSP SAMPLE buffers (stream_voice.h's 8 KB head and 16 KB
// ring), the sample's first head_cap bytes with random access, the rest streamed in order through the
// ring, chunk bytes a refill between requests (the caller's loop job; never in render). One stream per
// voice: a frame asked for outside head and ring restarts the stream there (silence until refilled); a
// frame ahead of the ring's data within its reach is waited for (the next refill moves the ring up to it).
// The ring keeps a back margin (64 frames, at most a quarter of the space a refill leaves) below the
// lowest stream frame (past the head) read since the last refill, so readers that look a few frames back
// (interpolation) stay in RAM; reads inside the head never hold the ring back. When every read since the
// last refill stayed inside the head (a retrigger, a loop back) and the ring has moved on, the refill
// re-arms it at the head's end (as StreamVoice::trigger) while the head plays. A reader that reads the
// head and the stream together (two grains, a loop crossfade) keeps its stream position.
// Portable: the Mac tests it; the Teensy wires rd to the pool file and the card index.
//
// Inherent without PSRAM (one 16 KB ring a voice): two positions both in the stream more than one ring
// apart thrash (each restarts the stream at the other, mostly silence); two positions in the stream
// further apart than ring_cap - back starve the higher one; reading backwards through the stream seeks
// (and goes silent) every block. Two stream positions read in alternating blocks thrash even within one
// ring of each other (4000 frames apart: the near position fully short, about one card read a request):
// the ring is anchored to the lowest position read and every refill period sees both. A granular plugin
// should read each grain's frames in one block, or keep its grains within the head; serving two windows
// is a possible later improvement. Positions inside the head are always served.
//
// Not thread-safe: rmp_pool_read (render, called from the sketch's request handling) and the job calls
// (prepare, refill, clear) must run in the same context (the sketch's loop), never from an ISR.
//
// PCM: 16/24-bit and 32-bit INTEGER (two's complement). A 32-bit float WAV (format tag 3) is not
// recognised here: the caller must reject it before rmp_pool_prepare (it would decode as noise).
#pragma once
#include <stdint.h>
#include "rmp_api.h"
#define RMP_POOL_NONE 0xFFFFFFFFu
struct rmp_pool_src { uint32_t data_offset, data_len; float rate; uint8_t channels, bits; };   // a WAV's PCM
typedef bool (*rmp_pool_rd)(void *ctx, uint32_t off, uint8_t *buf, uint32_t n);   // file bytes at off
struct rmp_pool_reader {
  uint8_t *head, *ring;
  uint32_t head_cap, ring_cap, chunk;
  int slot;                // the pool slot prepared (1..127), 0 none
  int want;                // a slot asked for and not prepared: the caller's job prepares it; 0 none
  int bad;                 // a slot that could not be prepared: EMPTY until cleared; 0 none
  rmp_pool_src src;
  uint32_t bpf, hlen;      // bytes a frame; head bytes held (whole frames)
  uint32_t back;           // bytes kept in the ring below the lowest frame read (whole frames)
  uint32_t rs, fill;       // the ring holds data bytes [rs, rs + fill), byte b at ring[b % ring_cap]
  uint32_t next;           // the frame after the last one delivered
  uint32_t snext;          // the frame after the last stream frame (past the head) delivered: the stream's position
  uint32_t low;            // the lowest stream frame (>= the head's frames) read since the last refill:
                           // what the ring must keep
  bool low_reset;          // a refill ran: the next stream read starts low afresh
  uint32_t hi;             // the highest frame asked for since the last refill, NONE if no read: below the
                           // head's frames, the refill re-arms a ring that has moved on
  uint32_t wait;           // the last stream read's frame missing ahead of the ring within its reach, else NONE
  uint32_t seek;           // a frame asked for outside head and ring: the next refill restarts there
  rmp_pool_rd rd;
  void *ctx;
  uint32_t misses;         // frames asked for that were not in RAM (diagnostics)
};
void rmp_pool_attach(rmp_pool_reader *r, uint8_t *head, uint32_t head_cap, uint8_t *ring, uint32_t ring_cap, uint32_t chunk);
void rmp_pool_clear(rmp_pool_reader *r);   // nothing prepared, wanted or bad (the voice left, a new project)
// pool_info for this voice: RMP_POOL_OK with *info when slot is prepared; RMP_POOL_EMPTY for a slot outside
// 1..127 or one that could not be read; else its prepare is asked for (want) and RMP_POOL_NOT_READY.
// info->where is always RMP_POOL_HEAD; a sample that fits the head whole has head_frames == frames (all of
// it random access).
int32_t rmp_pool_info_get(rmp_pool_reader *r, int slot, rmp_pool_info *info);
// The caller's job: slot's PCM is src, read through rd: the head is read (one call); false (and slot bad)
// for a format other than 16/24/32-bit (integer) 1+ channels, or a failed read.
bool rmp_pool_prepare(rmp_pool_reader *r, int slot, const rmp_pool_src *src, rmp_pool_rd rd, void *ctx);
// pool_read: n frames from frame pos, min(channels, 2) floats a frame interleaved; the frames delivered
// (fewer: not in RAM yet, or the end). Never reads the file.
int32_t rmp_pool_read(rmp_pool_reader *r, int slot, uint32_t pos, float *buf, int32_t n);
// The caller's job between requests: one chunk into the ring; first a pending seek restarts it, or a
// re-arm (reads only in the head since the last refill) puts it back at the head's end, then what lies
// below the lowest stream frame read (less the back margin) is dropped; bytes read.
uint32_t rmp_pool_refill(rmp_pool_reader *r);
// The refill's priority, in frames: the stream position's lead (ring data after the last stream frame
// delivered; 0xFFFFFFFF when the rest of the sample is in RAM), or the head frames left after the last
// read when fewer and a re-arm is pending (reads since the last refill, the ring moved on, all reads in the
// head or the last ending there); 0 with a seek pending or a frame waited for. A stopped voice (no reads
// since the last refill) reports its lead only, so it does not look starving.
uint32_t rmp_pool_ahead(const rmp_pool_reader *r);
// A sample whole in RAM (PSRAM-resident: pool_read's other path): n frames from pos of pcm (frames frames of
// `channels` stored channels at `bits` 16/24/32 integer), the first two channels interleaved; the frames delivered.
int32_t rmp_pool_decode(const uint8_t *pcm, uint32_t frames, uint32_t channels, uint32_t bits, uint32_t pos, float *buf, int32_t n);
