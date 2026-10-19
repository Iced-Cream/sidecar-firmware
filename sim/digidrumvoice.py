"""DIGIDRUM voices through digidrum/build/libdigidrum.dylib (build with
digidrum/build.sh): soejrd/digidrum's TRX and EFM machines, one engine number
(digidrum/dd_voice.h)."""
import ctypes
import os

import numpy as np

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "digidrum", "build", "libdigidrum.dylib"))
_lib.ddv_size.restype = ctypes.c_uint32
_lib.ddv_init.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.ddv_render.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint8), ctypes.c_float,
                            ctypes.c_int, ctypes.POINTER(ctypes.c_float), ctypes.c_uint32]
_lib.ddv_name.restype = ctypes.c_char_p
_lib.ddv_label.restype = ctypes.c_char_p
ENGINES = 11   # dd_voice.h's curated list: EFM BD SD XT CP RS CB HH CY, TRX RS CB CL
NAMES = [_lib.ddv_name(i).decode() for i in range(ENGINES)]


def label(engine, i):
    return _lib.ddv_label(engine, i).decode()


def default(engine, i):
    return _lib.ddv_default(engine, i)


class DigidrumVoice:
    """One DIGIDRUM voice: engine 0..10, controls 0..127 (8), level 0..1."""

    def __init__(self):
        self._mem = ctypes.create_string_buffer(_lib.ddv_size())
        self.engine = 0
        _lib.ddv_init(self._mem, 0)
        self.control = [default(0, i) for i in range(8)]
        self.level = 1.0
        self._trig = False

    def trigger(self, note=None, velocity=1.0):
        """note is ignored: the engine's PTCH control sets the pitch."""
        self.level = max(0.05, min(1.0, velocity))
        self._trig = True

    def note_off(self):
        """The Rytm's note-off (cmd 0x11): nothing - a drum: it plays out."""

    def render(self, n):
        out = np.zeros(n, dtype=np.float32)
        c = (ctypes.c_uint8 * 8)(*[max(0, min(127, int(x))) for x in self.control])
        for k in range(0, n, 32):
            m = min(32, n - k)
            _lib.ddv_render(self._mem, int(self.engine), c, float(self.level), int(self._trig),
                            out[k:].ctypes.data_as(ctypes.POINTER(ctypes.c_float)), m)
            self._trig = False
        return out
