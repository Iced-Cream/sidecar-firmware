// sdk/rmp/shim/audio/Audio.h - the Teensy Audio Library objects available to a
// plugin (the generators and the processing Noise Plethora's sound-generating
// plugins use). The headers and .cpp files come from the installed Teensyduino's
// libraries/Audio (MIT); a plugin's Makefile lists the .cpp files it links.
// Not here: I/O objects, and the effects that need large buffers or an external
// input (freeverb, granular, flange, delay), and effect_combine, effect_bitcrusher,
// amplifier (no kept Noise Plethora plugin uses them).
#pragma once
#include <math.h>
#include "Arduino.h"
#include "AudioStream.h"
#include "synth_waveform.h"
#include "synth_sine.h"
#include "synth_whitenoise.h"
#include "synth_pinknoise.h"
#include "synth_pwm.h"
#include "synth_dc.h"
#include "mixer.h"
#include "filter_variable.h"
#include "effect_multiply.h"
#include "effect_wavefolder.h"
