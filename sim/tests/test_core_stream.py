"""The core's streamed sample voice (core/engines/stream_voice.h, the Teensy's DSP SAMPLE source) against the
simulator's sample voice (sim/sampler/sample_voice.h through libdaisy): forward playback, the same sound. At
transpositions off the octave the simulator's float position drifts, so there the core meets the exact interpolation.

    make -C core lib && sim/daisysp/build.sh && uv run python -m unittest sim/tests/test_core_stream.py
"""
import ctypes
import os
import sys
import tempfile
import unittest
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from samplevoice import SampleVoice  # noqa: E402

LIB = ctypes.CDLL(os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib"))
LIB.core_c_stream_render.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float,
                                     ctypes.c_float, ctypes.c_int, ctypes.c_float,
                                     ctypes.POINTER(ctypes.c_float), ctypes.c_int]
LIB.core_c_stream_render.restype = ctypes.c_int
MIN_SNR_DB = 60.0


def snr(ref, got):
    e = np.sqrt(np.mean((ref - got) ** 2))
    return np.inf if e == 0 else 20 * np.log10(np.sqrt(np.mean(ref ** 2)) / e)


class TestCoreStream(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="core-stream-")
        os.makedirs(os.path.join(self.card, "Samples", "S"))
        t = np.arange(48000 * 2)   # 2 s: longer than the 8 KB head and the 16 KB ring
        self.pcm = (np.sin(2 * np.pi * 220 * t / 48000) * 12000 + np.sin(2 * np.pi * 1234 * t / 48000) * 3000
                    ).astype(np.int16)
        with wave.open(os.path.join(self.card, "Samples", "S", "tone.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(self.pcm.tobytes())

    def core(self, semis, n):
        out = (ctypes.c_float * n)()
        got = LIB.core_c_stream_render(self.card.encode(), b"/Samples/S/tone.wav", semis, 0.0, 1.0, 0, 1.0, out, n)
        self.assertEqual(got, n)
        return np.array(out[:], dtype=np.float64)

    def sim(self, semis, n):
        v = SampleVoice()
        v.set((self.pcm.reshape(-1, 1), 48000, "tone"))
        v.trigger()
        return v.render(n, semis, 0.0, 1.0, False, 1.0).astype(np.float64)

    def exact(self, semis, n):
        """The same linear interpolation with the position kept exactly (float64), at the voice's float32 step."""
        pcm = self.pcm.astype(np.float64)
        step = float(np.float32(2.0) ** np.float32(semis / 12.0))
        pos = np.arange(n) * step
        k = pos.astype(np.int64)
        k1 = np.minimum(k + 1, len(pcm) - 1)
        k = np.minimum(k, len(pcm) - 1)
        out = (pcm[k] + (pcm[k1] - pcm[k]) * (pos - np.floor(pos))) / 32768.0
        out[pos > len(pcm) - 1] = 0.0
        return out

    def test_forward_same_sound(self):
        # Octaves: the step is exact, so the simulator's single-float position stays exact too.
        for semis in (0.0, 12.0, -12.0):
            n = 48000
            a, b = self.core(semis, n), self.sim(semis, n)
            self.assertGreater(np.abs(a).max(), 0.2)
            self.assertGreaterEqual(snr(b, a), MIN_SNR_DB, f"semitones {semis}")

    def test_forward_transposed(self):
        # Other steps: the simulator keeps the position in one float, which drifts by up to 1/512 frame a sample
        # past 32768 frames (measured at +7: 5.9 dB from the exact position, the core 128.9 dB). StreamVoice
        # keeps frame + fraction, so it is held to the exact interpolation instead.
        for semis in (7.0, -5.0, 0.37):
            n = 48000
            a = self.core(semis, n)
            self.assertGreater(np.abs(a).max(), 0.2)
            self.assertGreaterEqual(snr(self.exact(semis, n), a), MIN_SNR_DB, f"semitones {semis}")


if __name__ == "__main__":
    unittest.main()
