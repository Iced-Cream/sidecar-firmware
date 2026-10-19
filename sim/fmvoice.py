"""6-OP FM voices (rytm-mods 0021 machine 40, flag 0x48) through
plaits/build/libplaits.dylib (build with plaits/build.sh): Plaits' six-op
engine playing one DX7 voice each (plaits/fm_machine.h), and Plaits' ROM
voices (its three 32-voice DX7 banks)."""
import ctypes
import hashlib
import os

import numpy as np

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "plaits", "build", "libplaits.dylib"))
_lib.fm_new.restype = ctypes.c_void_p
_lib.fm_free.argtypes = [ctypes.c_void_p]
_lib.fm_voice.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.fm_knobs.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]
_lib.fm_note.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float]
_lib.fm_note_off.argtypes = [ctypes.c_void_p]
_lib.fm_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
_lib.fm_rom_voice.argtypes = [ctypes.c_int, ctypes.c_char_p]

ROM_VOICES = 96


def rom_voice(i):
    """Plaits' ROM voice i (0..95), 128 packed bytes."""
    out = ctypes.create_string_buffer(128)
    _lib.fm_rom_voice(i, out)
    return out.raw


def voice_name(v):
    """A packed voice's 10-character name (bytes 118..127), trailing spaces cut;
    ASCII 0x20..0x7e, else '_'."""
    return "".join(chr(c) if 0x20 <= c <= 0x7E else "_" for c in (b & 0x7F for b in v[118:128])).rstrip()


def rom_banks():
    """Plaits' three ROM banks: [(MD5 hex of the 4096 bytes, bytes)]."""
    out = []
    for k in range(3):
        b = b"".join(rom_voice(32 * k + i) for i in range(32))
        out.append((hashlib.md5(b).hexdigest(), b))
    return out


class FmVoice:
    """One 6-OP FM voice: knobs P1..P8 (PAT TIM MOR DEC LPG TUN LEV LEN, 0..127);
    use() the DX7 voice PAT picks (128 bytes, None: silent)."""

    def __init__(self):
        self.h = _lib.fm_new()
        self.name = ""

    def knobs(self, k):
        a = (ctypes.c_float * 8)(*[float(x) for x in k[:8]])
        _lib.fm_knobs(self.h, a)

    def use(self, v, name=""):
        _lib.fm_voice(self.h, bytes(v) if v is not None else None)
        self.name = name if v is not None else f"{name} (bank missing)" if name else "(empty)"

    def trigger(self, note=60, velocity=1.0):
        _lib.fm_note(self.h, float(note), float(velocity))

    def note_off(self):
        """The Rytm's note-off (cmd 0x11): LEN 127 holds until it."""
        _lib.fm_note_off(self.h)

    def render(self, n):
        out = np.zeros(n, dtype=np.float32)
        _lib.fm_render(self.h, out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), n)
        return out

    def __del__(self):
        if getattr(self, "h", None):
            _lib.fm_free(self.h)
