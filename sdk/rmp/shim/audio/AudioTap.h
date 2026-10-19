// sdk/rmp/shim/audio/AudioTap.h - the end of a plugin voice's graph: copies the
// block it receives into a buffer render() reads from (silence when none came).
#pragma once
#include <string.h>
#include "AudioStream.h"
class AudioTap : public AudioStream {
public:
  AudioTap() : AudioStream(1, q) {}
  int16_t buf[AUDIO_BLOCK_SAMPLES];
  void update() override {
    audio_block_t *b = receiveReadOnly(0);
    if (b) { memcpy(buf, b->data, sizeof buf); release(b); } else memset(buf, 0, sizeof buf);
  }
private:
  audio_block_t *q[1];
};
