"""Plaits voices through plaits/build/libplaits.dylib (build with plaits/build.sh)."""
import ctypes
import os

import numpy as np

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "plaits", "build", "libplaits.dylib"))
_lib.plaits_new.restype = ctypes.c_void_p
_lib.plaits_free.argtypes = [ctypes.c_void_p]
_lib.plaits_render2.argtypes = [ctypes.c_void_p, ctypes.c_int] + [ctypes.c_float] * 6 + [
    ctypes.c_int] + [ctypes.c_float] * 5 + [ctypes.POINTER(ctypes.c_float), ctypes.c_int]

ENGINES = 24


class Voice:
    """One Plaits voice. Parameters 0..1 except engine (0..23) and note."""

    def __init__(self):
        self.h = _lib.plaits_new()
        self.engine, self.note = 8, 48.0
        self.harmonics = self.timbre = self.morph = 0.5
        self.decay, self.lpg_colour, self.level = 0.5, 0.5, 0.8
        # The internal envelope's amounts (fm -1..1, timbre/morph 0..1), the
        # OUT -> AUX crossfade, glide (0 = none, 0..1 per block of the gap) and
        # the output gain.
        self.fm_amount = self.timbre_amount = self.morph_amount = 0.0
        self.aux_mix, self.glide, self.gain = 0.0, 0.0, 1.0
        self.target_note = self.note
        self._trig = False

    def trigger(self, note=None, velocity=1.0):
        if note is not None:
            self.target_note = float(note)
            if self.glide <= 0.0:
                self.note = self.target_note
        self.level = max(0.05, min(1.0, velocity))
        self._trig = True

    def note_off(self):
        """The Rytm's note-off (cmd 0x11): nothing - Plaits' trigger has no gate here (its LPG decays on its own)."""

    def render(self, n):
        out = np.zeros(n, dtype=np.float32)
        if self.glide > 0.0:
            self.note += (self.target_note - self.note) * min(1.0, n / 48000.0 / (0.002 + self.glide))
        else:
            self.note = self.target_note
        _lib.plaits_render2(self.h, int(self.engine), self.note, self.harmonics, self.timbre,
                            self.morph, self.decay, self.lpg_colour, int(self._trig),
                            self.level, self.fm_amount, self.timbre_amount, self.morph_amount,
                            self.aux_mix, out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), n)
        out *= self.gain
        self._trig = False
        return out

    def __del__(self):
        if getattr(self, "h", None):
            _lib.plaits_free(self.h)
