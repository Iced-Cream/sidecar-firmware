// A C interface to the DAISYSP voice (daisy_engine.h, on DaisySP: Electrosmith,
// MIT) for the Teensy simulator: one voice per handle, any number of frames per
// call.
#include <new>

#include "daisy_engine.h"
#include "sample_voice.h"

extern "C" {

void* daisy_new(float sample_rate) {
  rytm::DaisyEngine* e = new (std::nothrow) rytm::DaisyEngine();
  if (e) e->Init(sample_rate);
  return e;
}

void daisy_free(void* p) { delete static_cast<rytm::DaisyEngine*>(p); }

// `in`: n samples of an analog voice exciting it, or null (the built-in strike).
void daisy_render(void* p, int model, float note, float structure, float brightness,
                  float damping, float accent, float exciter, int trigger, float level,
                  float* out, int n, const float* in) {
  static_cast<rytm::DaisyEngine*>(p)->Render(model, note, structure, brightness, damping,
                                             accent, exciter, trigger > 0, level, out, n, in);
}

// The SD-card sample voice (sampler/sample_voice.h): the caller keeps `pcm`.
void* smp_new() { return new (std::nothrow) rytm::SampleVoice(); }
void smp_free(void* p) { delete static_cast<rytm::SampleVoice*>(p); }
void smp_set(void* p, const int16_t* pcm, unsigned frames, int channels, float rate) {
  static_cast<rytm::SampleVoice*>(p)->Set(pcm, frames, channels, rate);
}
void smp_trigger(void* p) { static_cast<rytm::SampleVoice*>(p)->Trigger(); }
void smp_render(void* p, float semitones, float start, float end, int loop, float level,
                float* out, int n) {
  static_cast<rytm::SampleVoice*>(p)->Render(semitones, start, end, loop != 0, level, 48000.0f,
                                             out, n);
}
}
