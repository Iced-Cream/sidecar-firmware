/* Derived from the core's AudioStream.cpp (Teensyduino 1.62); PJRC's notice follows, as the licence requires. */
/* Teensyduino Core Library
 * http://www.pjrc.com/teensy/
 * Copyright (c) 2017 PJRC.COM, LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * 1. The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * 2. If the Software is incorporated into a build system that allows
 * selection among a list of target devices, then similar target
 * devices manufactured by PJRC.COM must be included in the list of
 * target devices and selectable in the same manner.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
// sdk/rmp/shim/audio/AudioStream.cpp - see AudioStream.h. Follows the core's
// AudioStream.cpp (Teensyduino 1.62) for transmit/receive/release/connect, with the
// pool and the update list taken from the current AudioGraph.
#include "AudioStream.h"
#include <string.h>

AudioGraph *audio_current_graph;
uint16_t AudioStream::cpu_cycles_total, AudioStream::cpu_cycles_total_max;

void AudioGraph::begin(uint32_t seed) {
  memset(blocks, 0, sizeof blocks);
  for (int i = 0; i < AUDIO_GRAPH_BLOCKS; ++i) blocks[i].memory_pool_index = (uint16_t)i;
  first_update = nullptr;
  rng = seed ? seed : 0x9E3779B9u;
  memory_used = memory_used_max = alloc_failed = 0;
  audio_current_graph = this;
}

void AudioGraph::update_all() {
  audio_current_graph = this;
  for (AudioStream *p = first_update; p; p = p->next_update)
    if (p->active) p->update();
}

/* random() of the Arduino.h shim: xorshift32 on the current graph's state. */
extern "C" uint32_t audio_graph_random(void) {
  AudioGraph *g = audio_current_graph;
  if (!g) return 0;
  uint32_t x = g->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return g->rng = x;
}

AudioStream::AudioStream(unsigned char ninput, audio_block_t **iqueue)
  : cpu_cycles(0), cpu_cycles_max(0), active(false), num_inputs(ninput), numConnections(0),
    destination_list(nullptr), inputQueue(iqueue), next_update(nullptr) {
  for (int i = 0; i < num_inputs; ++i) inputQueue[i] = nullptr;
  AudioGraph *g = audio_current_graph;
  if (!g) return;
  AudioStream **pp = &g->first_update;
  while (*pp) pp = &(*pp)->next_update;
  *pp = this;
}

audio_block_t *AudioStream::allocate(void) {
  AudioGraph *g = audio_current_graph;
  if (!g) return nullptr;
  for (int i = 0; i < AUDIO_GRAPH_BLOCKS; ++i) {
    audio_block_t *b = &g->blocks[i];
    if (b->ref_count == 0) {
      b->ref_count = 1;
      if (++g->memory_used > g->memory_used_max) g->memory_used_max = g->memory_used;
      return b;
    }
  }
  if (g->alloc_failed < 0xFFFF) g->alloc_failed++;
  return nullptr;
}

void AudioStream::release(audio_block_t *block) {
  if (!block || !block->ref_count) return;
  if (--block->ref_count == 0 && audio_current_graph) audio_current_graph->memory_used--;
}

void AudioStream::transmit(audio_block_t *block, unsigned char index) {
  for (AudioConnection *c = destination_list; c; c = c->next_dest) {
    if (c->src_index == index && c->dst->inputQueue[c->dest_index] == nullptr) {
      c->dst->inputQueue[c->dest_index] = block;
      block->ref_count++;
    }
  }
}

audio_block_t *AudioStream::receiveReadOnly(unsigned int index) {
  if (index >= num_inputs) return nullptr;
  audio_block_t *in = inputQueue[index];
  inputQueue[index] = nullptr;
  return in;
}

audio_block_t *AudioStream::receiveWritable(unsigned int index) {
  if (index >= num_inputs) return nullptr;
  audio_block_t *in = inputQueue[index];
  inputQueue[index] = nullptr;
  if (in && in->ref_count > 1) {
    audio_block_t *p = allocate();
    if (p) memcpy(p->data, in->data, sizeof(p->data));
    in->ref_count--;
    in = p;
  }
  return in;
}

AudioConnection::AudioConnection()
  : src(nullptr), dst(nullptr), src_index(0), dest_index(0), next_dest(nullptr), isConnected(false) {}

/* Return codes as the core's: 0 ok, 1 already connected, 2 input number too high,
   3 no source/destination, 4 destination input in use, 6 duplicate. */
int AudioConnection::connect(void) {
  if (isConnected) return 1;
  if (!src || !dst) return 3;
  if (dest_index >= dst->num_inputs) return 2;
  if (AudioGraph *g = audio_current_graph)
    for (AudioStream *s = g->first_update; s; s = s->next_update)
      for (AudioConnection *p = s->destination_list; p; p = p->next_dest)
        if (p->dst == dst && p->dest_index == dest_index) return 4;
  AudioConnection **pp = &src->destination_list;
  for (; *pp; pp = &(*pp)->next_dest)
    if ((*pp)->dst == dst && (*pp)->src_index == src_index && (*pp)->dest_index == dest_index) return 6;
  *pp = this;
  next_dest = nullptr;
  src->numConnections++; src->active = true;
  dst->numConnections++; dst->active = true;
  isConnected = true;
  return 0;
}

int AudioConnection::connect(AudioStream &source, unsigned char sourceOutput,
                             AudioStream &destination, unsigned char destinationInput) {
  if (isConnected) return 1;
  src = &source; dst = &destination;
  src_index = sourceOutput; dest_index = destinationInput;
  return connect();
}

int AudioConnection::disconnect(void) {
  if (!isConnected) return 1;
  if (dest_index >= dst->num_inputs) return 2;
  AudioConnection **pp = &src->destination_list;
  while (*pp && *pp != this) pp = &(*pp)->next_dest;
  if (!*pp) return 3;
  *pp = next_dest;
  next_dest = nullptr;
  if (dst->inputQueue[dest_index]) {
    AudioStream::release(dst->inputQueue[dest_index]);
    dst->inputQueue[dest_index] = nullptr;
  }
  if (--src->numConnections == 0) src->active = false;
  if (--dst->numConnections == 0) dst->active = false;
  isConnected = false;
  return 0;
}
