"""DAISYSP voices through daisysp/build/libdaisy.dylib (build with daisysp/build.sh)."""
import ctypes
import os

import numpy as np

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "daisysp", "build", "libdaisy.dylib"))
_lib.daisy_new.restype = ctypes.c_void_p
_lib.daisy_new.argtypes = [ctypes.c_float]
_lib.daisy_free.argtypes = [ctypes.c_void_p]
_lib.daisy_render.argtypes = [ctypes.c_void_p, ctypes.c_int] + [ctypes.c_float] * 6 + [
    ctypes.c_int, ctypes.c_float, ctypes.POINTER(ctypes.c_float), ctypes.c_int,
    ctypes.POINTER(ctypes.c_float)]

MODELS = ["MODAL", "STRING", "RESON"]   # rytm-mods 0021's curated list (DRIP dropped)
# INP: what strikes the voice. INT the built-in strike; NAT (the track's native
# machine) and NOISE (UT NOISE), played by the Rytm on this very voice, come in
# on the voice's own Overbridge output; BD..CY/CB on those voices' (channels
# 2..9, track order).
INPUTS = ["INT", "NAT", "NOISE", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB"]


class DaisyVoice:
    """One DAISYSP voice: model 0..2, note in semitones, the rest 0..1."""

    def __init__(self):
        self.h = _lib.daisy_new(48000.0)
        self.model, self.note = 0, 48.0
        self.structure = self.brightness = 0.5
        self.damping, self.accent, self.exciter = 0.3, 0.8, 0.25
        self.input = 0          # INPUTS index
        self.level, self.gain = 0.8, 1.0
        self._trig = False

    def trigger(self, note=None, velocity=1.0):
        if note is not None:
            self.note = float(note)
        self.level = max(0.05, min(1.0, velocity))
        self._trig = True

    def note_off(self):
        """The Rytm's note-off (cmd 0x11): nothing - the resonator rings out on its own."""

    def render(self, n, inp=None):
        """inp: n float32 samples of the analog voice exciting it, or None."""
        out = np.zeros(n, dtype=np.float32)
        ip = None
        if inp is not None:
            inp = np.ascontiguousarray(inp, dtype=np.float32)
            ip = inp.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        _lib.daisy_render(self.h, int(self.model), self.note, self.structure, self.brightness,
                          self.damping, self.accent, self.exciter, int(self._trig), self.level,
                          out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), n, ip)
        out *= self.gain
        self._trig = False
        return out

    def __del__(self):
        if getattr(self, "h", None):
            _lib.daisy_free(self.h)
