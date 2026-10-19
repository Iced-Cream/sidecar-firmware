"""JUNO voices through juno/build/libjuno.dylib (build with juno/build.sh): a
Juno-106 voice on AMY's factory patches and curves (juno/juno_voice.h)."""
import ctypes
import os

import numpy as np

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "juno", "build", "libjuno.dylib"))
_lib.juno_new.restype = ctypes.c_void_p
_lib.juno_new.argtypes = [ctypes.c_float]
_lib.juno_free.argtypes = [ctypes.c_void_p]
_lib.juno_knobs.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]
_lib.juno_note.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float]
_lib.juno_note_off.argtypes = [ctypes.c_void_p]
_lib.juno_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
_lib.juno_name.restype = ctypes.c_char_p
_lib.juno_name.argtypes = [ctypes.c_void_p]
_lib.juno_use.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
_lib.juno_factory.argtypes = [ctypes.c_int, ctypes.c_char_p]


def factory(p):
    """Factory patch p's 18 bytes (juno_patches.h)."""
    out = ctypes.create_string_buffer(18)
    _lib.juno_factory(p, out)
    return out.raw


class JunoVoice:
    """One JUNO voice: knobs P1..P8 (PAT CUT RES ENV ATK DEC REL LEN, 0..127)."""

    def __init__(self):
        self.h = _lib.juno_new(48000.0)

    def knobs(self, k):
        a = (ctypes.c_float * 8)(*[float(x) for x in k[:8]])
        _lib.juno_knobs(self.h, a)

    def use(self, b, name=""):
        """A patch from the project's JUNO list (18 bytes) instead of PAT's
        factory one; None: the factory patches again. knobs() after it."""
        _lib.juno_use(self.h, bytes(b) if b is not None else None, name.encode() if b is not None else None)

    @property
    def name(self):
        return _lib.juno_name(self.h).decode()

    def trigger(self, note=60, velocity=1.0):
        _lib.juno_note(self.h, float(note), float(velocity))

    def note_off(self):
        """The Rytm's note-off (cmd 0x11): the release (LEN 127 holds until it)."""
        _lib.juno_note_off(self.h)

    def render(self, n):
        out = np.zeros(n, dtype=np.float32)
        _lib.juno_render(self.h, out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), n)
        return out

    def __del__(self):
        if getattr(self, "h", None):
            _lib.juno_free(self.h)
