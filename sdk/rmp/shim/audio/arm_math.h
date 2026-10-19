/* sdk/rmp/shim/audio/arm_math.h - on the Teensy, the CMSIS-DSP header in
   Teensyduino's cores/teensy4 (on the include path after this directory); in the
   native build nothing: the Audio objects in Audio.h call no CMSIS function. */
#pragma once
#if defined(__arm__)
#include_next <arm_math.h>
#endif
