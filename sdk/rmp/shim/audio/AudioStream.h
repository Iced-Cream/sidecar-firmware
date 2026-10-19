/* Derived from the core's AudioStream.h (Teensyduino 1.62); PJRC's notice follows, as the licence requires. */
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
// sdk/rmp/shim/audio/AudioStream.h - the Teensy Audio Library's AudioStream for a
// plugin voice. The core's AudioStream (cores/teensy4, Teensyduino 1.62) keeps one
// global update list and one global block pool and runs from a software interrupt;
// here both belong to an AudioGraph inside the voice's state, and the voice runs
// one update pass per AUDIO_BLOCK_SAMPLES samples from its render().
//
// Same interface and semantics as the core where the Audio objects depend on them:
// objects update in construction order, only active ones (an AudioConnection makes
// both ends active); transmit() queues a block on each connected input that is
// empty and takes a reference; receiveWritable() copies a shared block; allocate()
// returns NULL when the pool is empty (the objects then skip their output);
// release() drops one reference. Not provided: AudioMemory() (the pool is the
// graph's), processor-usage counters (always 0), the interrupt scheduling.
//
// The current graph is a plugin global (audio_current_graph): set it (AudioGraph::
// begin) before constructing a voice's objects - their constructors register there
// - and update_all() sets it before each pass. Plugin calls are never concurrent.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// A package picks its block size (NP_BLOCK in a Noise Plethora Makefile): 16 is the
// smallest every object in Audio.h supports (synth_dc and synth_pinknoise fill 16
// samples per loop iteration, so 12 would write past the block); 128 is the Teensy's
// own, needed where a graph has a feedback loop (its one-block delay shapes the
// sound). The host renders 12 samples per call: a call runs at most one pass.
#ifndef AUDIO_BLOCK_SAMPLES
#define AUDIO_BLOCK_SAMPLES 16
#endif
#if AUDIO_BLOCK_SAMPLES % 16 != 0 || AUDIO_BLOCK_SAMPLES < 16 || AUDIO_BLOCK_SAMPLES > 128
#error "AUDIO_BLOCK_SAMPLES must be a multiple of 16 from 16 to 128 (synth_dc, synth_pinknoise)"
#endif
#ifndef AUDIO_SAMPLE_RATE_EXACT
#define AUDIO_SAMPLE_RATE_EXACT 48000.0f
#endif
#define AUDIO_SAMPLE_RATE AUDIO_SAMPLE_RATE_EXACT
#ifndef AUDIO_GRAPH_BLOCKS
#define AUDIO_GRAPH_BLOCKS 24   /* the block pool per voice graph (4 + 2 x AUDIO_BLOCK_SAMPLES B each): NP clusters peak at 16 */
#endif

typedef struct audio_block_struct {
  uint8_t ref_count;            /* 0 = free in the graph's pool */
  uint8_t reserved1;
  uint16_t memory_pool_index;
  int16_t data[AUDIO_BLOCK_SAMPLES];
} audio_block_t;

class AudioStream;
class AudioConnection;

struct AudioGraph {             // lives in the voice state (zeroed before voice_init)
  audio_block_t blocks[AUDIO_GRAPH_BLOCKS];
  AudioStream *first_update;    // the update list, in construction order
  uint32_t rng;                 // random() (Arduino.h shim): xorshift32, never 0
  uint16_t memory_used, memory_used_max, alloc_failed;
  void begin(uint32_t seed);    // empty pool and list, made current: before constructing the objects
  void update_all();            // one AUDIO_BLOCK_SAMPLES pass (made current first)
};
extern AudioGraph *audio_current_graph;

class AudioConnection {
public:
  AudioConnection();
  AudioConnection(AudioStream &source, AudioStream &destination) : AudioConnection() { connect(source, destination); }
  AudioConnection(AudioStream &source, unsigned char sourceOutput, AudioStream &destination, unsigned char destinationInput)
    : AudioConnection() { connect(source, sourceOutput, destination, destinationInput); }
  ~AudioConnection() { disconnect(); }
  friend class AudioStream;
  int disconnect(void);
  int connect(void);
  int connect(AudioStream &source, AudioStream &destination) { return connect(source, 0, destination, 0); }
  int connect(AudioStream &source, unsigned char sourceOutput, AudioStream &destination, unsigned char destinationInput);
protected:
  AudioStream *src;
  AudioStream *dst;
  unsigned char src_index;
  unsigned char dest_index;
  AudioConnection *next_dest;   // the source's list of connections
  bool isConnected;
};

#define AudioMemoryUsage() (audio_current_graph ? audio_current_graph->memory_used : 0)
#define AudioMemoryUsageMax() (audio_current_graph ? audio_current_graph->memory_used_max : 0)
#define AudioProcessorUsage() (0.0f)
#define AudioProcessorUsageMax() (0.0f)
#define AudioNoInterrupts() do {} while (0)
#define AudioInterrupts() do {} while (0)

class AudioStream {
public:
  AudioStream(unsigned char ninput, audio_block_t **iqueue);
  float processorUsage(void) { return 0.0f; }
  float processorUsageMax(void) { return 0.0f; }
  void processorUsageMaxReset(void) {}
  bool isActive(void) { return active; }
  uint16_t cpu_cycles;
  uint16_t cpu_cycles_max;
  static uint16_t cpu_cycles_total;
  static uint16_t cpu_cycles_total_max;
protected:
  bool active;
  unsigned char num_inputs;
  static audio_block_t *allocate(void);
  static void release(audio_block_t *block);
  void transmit(audio_block_t *block, unsigned char index = 0);
  audio_block_t *receiveReadOnly(unsigned int index = 0);
  audio_block_t *receiveWritable(unsigned int index = 0);
  static bool update_setup(void) { return false; }
  static void update_stop(void) {}
  friend class AudioConnection;
  friend struct AudioGraph;
  uint8_t numConnections;
private:
  AudioConnection *destination_list;
  audio_block_t **inputQueue;
  virtual void update(void) = 0;
  AudioStream *next_update;
};
