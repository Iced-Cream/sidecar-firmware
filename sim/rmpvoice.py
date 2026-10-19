# sim/rmpvoice.py - a machine plugin's native build as a simulator voice (sim/rmp/rmp_native.cpp), with
# plugin API 2: voice_enter/leave counts and the call log, load_entry, the per-voice preset state, pool
# samples, and presets through voiceloop's glue (a package's description, a preset folder, rmp_report).
import ctypes, os, subprocess
import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIBSRC = os.path.join(_HERE, "..", "teensy", "libraries", "RytmPlugins", "src")
_SOURCES = [os.path.join(_HERE, "rmp", "rmp_native.cpp"), os.path.join(_HERE, "..", "sdk", "rmp", "rmp_api.h")] + \
           [os.path.join(_LIBSRC, f) for f in ("rmp_pool.cpp", "rmp_pool.h", "rmp_presets.cpp", "rmp_presets.h",
                                                "rmp_manifest.cpp", "rmp_manifest.h", "rmp_loader.cpp", "rmp_loader.h",
                                                "rmp_format.h")] + [os.path.join(_HERE, "rmp", "build.sh")]
_lib = None
F, I16, U8 = ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_int16), ctypes.POINTER(ctypes.c_uint8)

def _load():
    global _lib
    if _lib is None:
        path = os.path.join(_HERE, "rmp", "librmp_native.dylib")
        if not os.path.exists(path) or os.path.getmtime(path) < max(os.path.getmtime(s) for s in _SOURCES):
            subprocess.run([os.path.join(_HERE, "rmp", "build.sh")], check=True)
        lib = ctypes.CDLL(path)
        vp, c_int, c_u32 = ctypes.c_void_p, ctypes.c_int, ctypes.c_uint32
        lib.rmp_open.restype = vp
        lib.rmp_open.argtypes = [ctypes.c_char_p]
        for f in ("rmp_note_off", "rmp_close"):
            getattr(lib, f).argtypes = [vp]
        lib.rmp_note_on.argtypes = [vp, ctypes.c_float, ctypes.c_float]
        lib.rmp_params.argtypes = [vp, F]
        lib.rmp_render.argtypes = [vp, F, c_int]
        lib.rmp_api_of.argtypes = [vp]
        lib.rmp_calls.argtypes = [vp, c_int]
        lib.rmp_load_entry.argtypes = [vp, ctypes.c_char_p, c_int, I16]
        lib.rmp_native_pool_set.argtypes = [c_int, ctypes.c_char_p, c_u32, c_int, c_int, ctypes.c_float, c_int]
        lib.rmp_native_pool_set.restype = c_int
        lib.rmp_call_log.argtypes = [ctypes.c_char_p, c_int]
        lib.rmp_set_package.argtypes = [vp, ctypes.c_char_p]
        lib.rmp_preset_file.argtypes = [vp, ctypes.c_char_p, ctypes.c_char_p, c_int]
        lib.rmp_preset_name.argtypes = [vp, c_int, ctypes.c_char_p]
        lib.rmp_report.argtypes = [vp, c_int, c_int, F, c_u32, U8, U8, ctypes.POINTER(c_int)]
        lib.rmp_native_pool_clear.argtypes = []
        lib.rmp_native_pool_job.argtypes = []
        lib.rmp_native_preset_parse.argtypes = [ctypes.c_char_p, c_int, I16]
        lib.rmp_pv_decide_v.argtypes = [vp, c_int, c_int, c_int, c_int, F, ctypes.POINTER(c_int)]
        lib.rmp_pv_loaded_v.argtypes = [vp, c_int, c_int, I16, U8, c_u32, F, U8, U8]
        lib.rmp_pv_apply_v.argtypes = [vp, F, c_int, c_u32]
        _lib = lib
    return _lib

def _f16(p):
    return (ctypes.c_float * 16)(*[float(x) for x in list(p)[:16]])

def preset_parse(text):
    """A generic preset file's values: P1..P16 as a list, -1 where the file names none."""
    b = text.encode()
    v = (ctypes.c_int16 * 16)()
    _load().rmp_native_preset_parse(b, len(b), v)
    return list(v)

def pool_set(slot, pcm_bytes, channels, bits, rate, psram=False):
    """Pool slot `slot` holds this PCM (interleaved, little endian); psram: resident, read whole."""
    if _load().rmp_native_pool_set(slot, pcm_bytes, len(pcm_bytes), channels, bits, float(rate), 1 if psram else 0):
        raise ValueError(f"pool slot {slot}: {channels} channels, {bits} bits: not 1+ channels of 16/24/32-bit PCM")

def pool_clear():
    _load().rmp_native_pool_clear()

def call_log():
    """The harness's calls into plugins since the last call_log: I voice_init, E voice_enter, L voice_leave,
    C a voice closed (its state freed)."""
    b = ctypes.create_string_buffer(4096)
    _load().rmp_call_log(b, len(b))
    return b.value.decode()

def pool_job():
    """The Teensy's loop jobs once: prepares asked for, one refill for each voice running short."""
    _load().rmp_native_pool_job()

class RmpVoice:
    """One voice of a plugin machine (path to its native .dylib)."""
    def __init__(self, dylib_path):
        self.h = None
        self.lib = _load()
        self.h = self.lib.rmp_open(os.fsencode(dylib_path))
        if not self.h:
            raise RuntimeError(f"cannot load plugin {dylib_path}")
    def note_on(self, note, velocity): self.lib.rmp_note_on(self.h, note, velocity)
    def note_off(self): self.lib.rmp_note_off(self.h)
    def params(self, p): self.lib.rmp_params(self.h, _f16(p))
    def render(self, n):
        out = np.zeros(n, dtype=np.float32)
        self.lib.rmp_render(self.h, out.ctypes.data_as(F), n)
        return out
    def api(self): return self.lib.rmp_api_of(self.h)
    def calls(self):
        """(voice_enter calls, voice_leave calls) on this voice."""
        return self.lib.rmp_calls(self.h, 0), self.lib.rmp_calls(self.h, 1)
    def load_entry(self, data):
        """(the plugin's result, values P1..P16 with -1 untouched); -1000 when it has no load_entry."""
        v = (ctypes.c_int16 * 16)()
        r = self.lib.rmp_load_entry(self.h, data, len(data), v)
        return r, list(v)
    def preset_decide(self, pre, total, is_trig, first, p):
        s = ctypes.c_int(0)
        e = self.lib.rmp_pv_decide_v(self.h, pre, total, int(is_trig), int(first), _f16(p), ctypes.byref(s))
        return e, bool(s.value)
    def preset_loaded(self, entry, sync, values, roles, now_ms, p):
        """The 1C pairs [(knob, value)] a load with write-back sends."""
        ok, ov = (ctypes.c_uint8 * 16)(), (ctypes.c_uint8 * 16)()
        n = self.lib.rmp_pv_loaded_v(self.h, entry, int(sync), (ctypes.c_int16 * 16)(*values), (ctypes.c_uint8 * 16)(*roles),
                                     now_ms, _f16(p), ok, ov)
        return [(ok[i], ov[i]) for i in range(n)]
    def preset_apply(self, p, is_trig, now_ms):
        a = _f16(p)
        self.lib.rmp_pv_apply_v(self.h, a, int(is_trig), now_ms)
        return list(a)
    def loads(self):
        """Preset loads rmp_report attempted on this voice."""
        return self.lib.rmp_calls(self.h, 2)
    def set_package(self, rmp_path):
        """The plugin's package (.rmp): its manifest describes the slot (roles, ranges, PRE, preset_ext)."""
        r = self.lib.rmp_set_package(self.h, os.fsencode(rmp_path))
        if r:
            raise RuntimeError(f"cannot read package {rmp_path} ({r})")
    def preset_files(self, files):
        """The slot's preset folder: {file name: bytes}, in folder order (replaces the folder)."""
        self.lib.rmp_preset_file(self.h, None, None, 0)
        for name, data in files.items():
            self.lib.rmp_preset_file(self.h, name.encode(), data, len(data))
    def preset_names(self):
        b = ctypes.create_string_buffer(14)
        n = self.lib.rmp_preset_name(self.h, -1, b)
        out = []
        for i in range(n):
            self.lib.rmp_preset_name(self.h, i, b)
            out.append(b.value.decode())
        return out
    def report(self, p, is_trig=False, first=False, now_ms=0):
        """A report through the glue: (entry loaded or -1 / -2 failed, the 1C pairs, the values params() got)."""
        a = _f16(p)
        ok, ov, e = (ctypes.c_uint8 * 16)(), (ctypes.c_uint8 * 16)(), ctypes.c_int(0)
        n = self.lib.rmp_report(self.h, int(is_trig), int(first), a, now_ms, ok, ov, ctypes.byref(e))
        return e.value, [(ok[i], ov[i]) for i in range(n)], list(a)
    def __del__(self):
        if getattr(self, "h", None):
            self.lib.rmp_close(self.h); self.h = None
