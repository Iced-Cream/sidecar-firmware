"""26 FXIN / 27 FXIN_STATE (sim/protocol.py, rytm-mods 0030-fx-in): the bytes, the decoders, no clash with other ids.

    python3 -m unittest sim/tests/test_protocol_fxin.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import protocol as P  # noqa: E402


class FxIn(unittest.TestCase):
    def test_encode(self):
        self.assertEqual(P.fxin(1), bytes.fromhex("f07d52542601f7"))
        self.assertEqual(P.fxin(True), P.fxin(1))
        self.assertEqual(P.fxin(False), bytes.fromhex("f07d52542600f7"))
        self.assertEqual(P.fxin(P.FXIN_ASK), bytes.fromhex("f07d5254267ff7"))
        with self.assertRaises(ValueError):
            P.fxin(2)

    def test_decode(self):
        for v in (0, 1, 0x7F):
            cmd, payload = P.parse(P.fxin(v))
            self.assertEqual((cmd, P.decode_fxin(payload)), (0x26, v))
        for on in (False, True):
            cmd, payload = P.parse(P.fxin_state(on))
            self.assertEqual((cmd, P.decode_fxin_state(payload)), (0x27, on))
        self.assertIsNone(P.decode_fxin_state(b"\x02"))
        self.assertIsNone(P.decode_fxin_state(b""))
        self.assertIsNone(P.decode_fxin(b"\x05"))

    def test_ids_free(self):
        used = [v for k, v in vars(P).items() if k in ("SENDFX", "SENDFX_NAMES", "SENDFX_STATE", "SENDFX_ASK", "SLOT_PRINTS",
                                                       "NOTE_OFF", "POOL_ASSIGN", "POOL_ASK", "POOL_TABLE", "FM_NAMES", "FM_ASK")]
        self.assertNotIn(P.FXIN, used)
        self.assertNotIn(P.FXIN_STATE, used)
        self.assertEqual(len({P.FXIN, P.FXIN_STATE}), 2)


if __name__ == "__main__":
    unittest.main()
