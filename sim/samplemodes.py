"""DSP SAMPLE's playback modes and the sample RAM's allocator in the simulator,
through sampler/build/libsampler.dylib (build with sampler/build.sh): the C++
the Teensy runs (sampler/sample_modes.h, sampler/sample_ram.h; docs/psram.md).

ModeVoice plays a sample held whole in RAM - as the Teensy will from PSRAM -
in one of seven modes (MODES), the mode picked by MOD (SD SAMPLE page word 20,
was FLD) at a trig. sp, the SD SAMPLE page, is P9..P16 as the report sends
them: TUN FIN SMP MOD STA END LOP LEV (0..127.99)."""
import ctypes
import os

import numpy as np

_F = ctypes.POINTER(ctypes.c_float)
_U = ctypes.POINTER(ctypes.c_uint)


def _load(name):
    lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "sampler", "build", name))
    lib.sm_new.restype = ctypes.c_void_p
    lib.sm_new.argtypes = [ctypes.c_uint]
    lib.sm_free.argtypes = [ctypes.c_void_p]
    lib.sm_attach_pluck.argtypes = [ctypes.c_void_p, _F]
    lib.sm_set.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_uint, ctypes.c_float]
    lib.sm_trigger.argtypes = [ctypes.c_void_p, ctypes.c_float, _F]
    lib.sm_note_off.argtypes = [ctypes.c_void_p]
    lib.sm_stop.argtypes = [ctypes.c_void_p]
    lib.sm_render.argtypes = [ctypes.c_void_p, _F, _F, ctypes.c_int]
    lib.sm_playing.argtypes = [ctypes.c_void_p]
    lib.sm_mode.argtypes = [ctypes.c_void_p]
    lib.sm_ops.argtypes = [ctypes.c_void_p, _U]
    lib.sm_path.argtypes = [ctypes.c_int] * 3
    lib.sr_new.restype = ctypes.c_void_p
    lib.sr_new.argtypes = [ctypes.c_uint, ctypes.c_uint]
    lib.sr_free.argtypes = [ctypes.c_void_p]
    lib.sr_alloc.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, _U, ctypes.c_int,
                             ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    lib.sr_find.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    for f in ("sr_release", "sr_pin", "sr_unpin"):
        getattr(lib, f).argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.sr_touch.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint]
    lib.sr_block.argtypes = [ctypes.c_void_p, ctypes.c_int, _U, _U]
    for f in ("sr_used", "sr_largest_hole", "sr_evictions"):
        getattr(lib, f).argtypes = [ctypes.c_void_p]
        getattr(lib, f).restype = ctypes.c_uint
    lib.sr_count.argtypes = [ctypes.c_void_p]
    if hasattr(lib, "sm_cache_stats"):
        lib.sm_cache_stats.argtypes = [ctypes.POINTER(ctypes.c_ulonglong)]
    return lib


_lib = _load("libsampler.dylib")   # with read counts and the cache model
_fast = None


def fast_lib():
    """The build without counts or cache model (timing): ModeVoice(lib=fast_lib())."""
    global _fast
    if _fast is None:
        _fast = _load("libsampler_fast.dylib")
    return _fast


MODES = ["ONE", "LOOP", "SLICE", "GRAIN", "STRCH", "PLUCK", "WAVE"]
PATH_STREAM, PATH_RAM, PATH_SILENT = 0, 1, 2


def mode_value(mode):
    """A MOD knob value (0..127) in mode's zone (its middle)."""
    return int((mode + 0.5) * 128 / len(MODES))


def choose_path(psram, resident, mode):
    """sample_modes.h's ChoosePath: PATH_STREAM, PATH_RAM or PATH_SILENT."""
    return _lib.sm_path(int(bool(psram)), int(bool(resident)), int(mode))


def cache_reset():
    """The cache model (all voices' sample reads; sampler_c.cc): cleared."""
    _lib.sm_cache_reset()


def cache_stats():
    """(sample reads, cache misses) since cache_reset: a miss is a 32-byte
    PSRAM read on the Teensy."""
    o = (ctypes.c_ulonglong * 2)()
    _lib.sm_cache_stats(o)
    return tuple(o)


def active_grains():
    """Grains sounding now, all voices (at most 32: kGrainBudget)."""
    return _lib.sm_active_grains()


def to_mono16(pcm):
    """(frames x channels) int16 -> the sample RAM's format: int16 mono (the
    first two channels averaged, as the streamed voice), plus the guard frame."""
    a = pcm.astype(np.int32)
    m = a[:, 0] if a.shape[1] == 1 else (a[:, 0] + a[:, 1]) // 2
    m = m.astype(np.int16)
    return np.ascontiguousarray(np.concatenate([m, m[-1:]]))


class ModeVoice:
    def __init__(self, seed=1, lib=None):
        self.lib = lib or _lib
        self.h = self.lib.sm_new(seed)
        self.pcm = None
        self.line = np.zeros(self.lib.sm_pluck_len(), np.float32)   # PLUCK's delay line
        self.lib.sm_attach_pluck(self.h, self.line.ctypes.data_as(_F))

    def set(self, mono, rate):
        """mono: to_mono16's array (with its guard), or None for no sample."""
        self.pcm = mono                     # kept alive: the C side points at it
        if mono is None:
            self.lib.sm_set(self.h, None, 0, 48000.0)
        else:
            self.lib.sm_set(self.h, mono.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), len(mono) - 1, float(rate))

    @staticmethod
    def _sp(sp):
        return np.ascontiguousarray(sp, np.float32)

    def trigger(self, note, sp):
        s = self._sp(sp)
        self.lib.sm_trigger(self.h, float(note), s.ctypes.data_as(_F))

    def note_off(self):
        self.lib.sm_note_off(self.h)

    def stop(self):
        """Silent at once, its grains back to the budget (the voice streams or
        plays another machine)."""
        self.lib.sm_stop(self.h)

    def render(self, n, sp):
        s = self._sp(sp)
        out = np.zeros(n, np.float32)
        self.lib.sm_render(self.h, s.ctypes.data_as(_F), out.ctypes.data_as(_F), n)
        return out

    @property
    def playing(self):
        return bool(self.lib.sm_playing(self.h))

    @property
    def mode(self):
        return self.lib.sm_mode(self.h)

    def ops(self):
        """(sample values read, grains started) so far."""
        o = (ctypes.c_uint * 2)()
        self.lib.sm_ops(self.h, o)
        return tuple(o)

    def __del__(self):
        if getattr(self, "h", None):
            self.lib.sm_free(self.h)


class SampleRam:
    """The allocator alone (offsets, no memory): bytes of arena, align-byte blocks."""

    def __init__(self, size, align=4096):
        self.h = _lib.sr_new(size, align)

    def alloc(self, key, nbytes, now, evict=True):
        """-> (id or -1, [evicted keys]); evict False: a free hole or nothing."""
        ev = (ctypes.c_uint * 160)()
        n = ctypes.c_int(0)
        i = _lib.sr_alloc(self.h, key, nbytes, now, ev, 160, ctypes.byref(n), int(evict))
        return i, list(ev[:n.value])

    def find(self, key):
        return _lib.sr_find(self.h, key)

    def free(self, i):
        _lib.sr_release(self.h, i)

    def touch(self, i, now):
        _lib.sr_touch(self.h, i, now)

    def pin(self, i):
        _lib.sr_pin(self.h, i)

    def unpin(self, i):
        _lib.sr_unpin(self.h, i)

    def block(self, i):
        """(offset, bytes) or None."""
        o, b = ctypes.c_uint(), ctypes.c_uint()
        return (o.value, b.value) if _lib.sr_block(self.h, i, ctypes.byref(o), ctypes.byref(b)) else None

    used = property(lambda s: _lib.sr_used(s.h))
    largest_hole = property(lambda s: _lib.sr_largest_hole(s.h))
    evictions = property(lambda s: _lib.sr_evictions(s.h))
    count = property(lambda s: _lib.sr_count(s.h))

    def __del__(self):
        if getattr(self, "h", None):
            _lib.sr_free(self.h)
