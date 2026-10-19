"""The Teensy's SD-card samples in the simulator: a folder on the Mac stands in for
the card (--samples DIR, default sim/samples). Every folder holding .wav files,
at any depth, in path order, is FLD 0, 1, ...; its .wav files sorted by name are
SMP 0, 1, ... Playback is sampler/sample_voice.h, the C++
the Teensy sketch shares, through libdaisy."""
import ctypes
import os
import wave

import numpy as np

from daisyvoice import _lib

_lib.smp_new.restype = ctypes.c_void_p
_lib.smp_free.argtypes = [ctypes.c_void_p]
_lib.smp_set.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_uint,
                         ctypes.c_int, ctypes.c_float]
_lib.smp_trigger.argtypes = [ctypes.c_void_p]
_lib.smp_render.argtypes = [ctypes.c_void_p] + [ctypes.c_float] * 3 + [
    ctypes.c_int, ctypes.c_float, ctypes.POINTER(ctypes.c_float), ctypes.c_int]


def read_wav(path):
    """16-bit (or 24/32-bit, cut to 16) PCM WAV -> (int16 frames x channels, rate)."""
    with wave.open(path, "rb") as w:
        ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(w.getnframes())
    if width == 2:
        a = np.frombuffer(raw, "<i2")
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        a = (b[:, 2].astype(np.int8).astype(np.int16) << 8) | b[:, 1]
    elif width == 4:
        a = (np.frombuffer(raw, "<i4") >> 16).astype(np.int16)
    else:
        a = ((np.frombuffer(raw, np.uint8).astype(np.int16) - 128) << 8)
    return np.ascontiguousarray(a.astype(np.int16).reshape(-1, ch)), rate


class Library:
    def __init__(self, root):
        self.root = root
        self.cache = {}
        self.scan()

    def scan(self):
        """Every folder (at any depth) holding .wav files is one FLD, in path order."""
        self.folders = []
        if not os.path.isdir(self.root):
            return
        for d, dirs, files in sorted(os.walk(self.root)):
            dirs.sort()
            wavs = sorted(f for f in files if f.lower().endswith(".wav") and not f.startswith("."))
            if wavs:
                self.folders.append((os.path.relpath(d, self.root), wavs))

    # The card as a tree (the knob tree walk): a place is a tuple of folder names
    # under the root.
    def subdirs(self, place):
        """Subfolders of `place` that hold .wav files somewhere below."""
        pre = os.path.join(*place) if place else ""
        out = set()
        for d, _ in self.folders:
            if d == ".":
                continue
            parts = d.split(os.sep)
            if parts[:len(place)] == list(place) and len(parts) > len(place):
                out.add(parts[len(place)])
        return sorted(out, key=str.lower)

    def wavs(self, place):
        rel = os.path.join(*place) if place else "."
        for d, files in self.folders:
            if d == rel:
                return files
        return []

    def get_in(self, place, smp):
        """(pcm, rate, name) of file smp (clamped) in `place`, or None."""
        files = self.wavs(place)
        if not files:
            return None
        rel = os.path.join(*(list(place) + [files[min(smp, len(files) - 1)]]))
        if rel not in self.cache:
            try:
                pcm, rate = read_wav(os.path.join(self.root, rel))
            except (wave.Error, EOFError, ValueError) as e:
                print(f"sampler: {rel}: {e}")
                pcm, rate = np.zeros((2, 1), np.int16), 48000
            self.cache[rel] = (pcm, rate, rel)
        return self.cache[rel]

    # Numbered folders, as the Teensy's card index numbers them (for 0023's
    # TEENSY folder in the Rytm's file browser): breadth first from the root (0),
    # only folders with .wav files somewhere below.
    def numbered(self):
        if getattr(self, "_num", None) is None:
            num, parent, kids = [()], [0], {}
            i = 0
            while i < len(num):
                place = num[i]
                kids[i] = []
                for d in self.subdirs(place):
                    num.append(place + (d,))
                    parent.append(i)
                    kids[i].append(len(num) - 1)
                i += 1
            self._num, self._parent, self._kids = num, parent, kids
        return self._num, self._parent, self._kids

    def listing(self, f):
        """Folder f's (parent, [(kind, id, size, name)]): subfolders (kind 1, id
        = their number), then files (kind 0, id = position, size in bytes)."""
        num, parent, kids = self.numbered()
        if not 0 <= f < len(num):
            return 0, []
        out = [(1, k, 0, num[k][-1]) for k in kids[f]]
        place = num[f]
        rel = os.path.join(*place) if place else ""
        for i, fn in enumerate(self.wavs(place)):
            size = os.path.getsize(os.path.join(self.root, rel, fn))
            out.append((0, i, size, os.path.splitext(fn)[0]))
        return parent[f], out

    def name(self, fld, smp):
        if not self.folders:
            return None
        d, files = self.folders[min(fld, len(self.folders) - 1)]
        return os.path.join(d, files[min(smp, len(files) - 1)])

    def get(self, fld, smp):
        """(pcm, rate, name) of folder fld's file smp (clamped), or None."""
        n = self.name(fld, smp)
        if n is None:
            return None
        if n not in self.cache:
            try:
                pcm, rate = read_wav(os.path.join(self.root, n))
            except (wave.Error, EOFError, ValueError) as e:
                print(f"sampler: {n}: {e}")
                pcm, rate = np.zeros((2, 1), np.int16), 48000
            self.cache[n] = (pcm, rate, n)
        return self.cache[n]


class SampleVoice:
    def __init__(self):
        self.h = _lib.smp_new()
        self.pcm = None
        self.name = None

    def set(self, entry):
        """entry: (pcm, rate, name), or None for no sample (silent)."""
        if entry is None:
            if self.name is None:
                return
            entry = (np.zeros((2, 1), np.int16), 48000, None)
        elif entry[2] == self.name:
            return
        pcm, rate, self.name = entry
        self.pcm = pcm                      # keep it alive: the C side points at it
        _lib.smp_set(self.h, pcm.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), len(pcm),
                     pcm.shape[1], float(rate))

    def trigger(self):
        _lib.smp_trigger(self.h)

    def note_off(self):
        """The Rytm's note-off (cmd 0x11): nothing - the sample plays out (as the Rytm's own one-shots)."""

    def render(self, n, semitones, start, end, loop, level):
        out = np.zeros(n, np.float32)
        _lib.smp_render(self.h, semitones, start, end, int(loop), level,
                        out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), n)
        return out

    def __del__(self):
        if getattr(self, "h", None):
            _lib.smp_free(self.h)
