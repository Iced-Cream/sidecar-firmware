"""The note-route messages (sim/protocol.py, 51 ops 28-2B / 68-6B, generic-two-box S8l; core/noteroute): layout,
round trips, and no clash with the other 51 ops in use or reserved (S8h's 18-1C / 58-5C) or the top-level commands.

    python3 -m unittest sim/tests/test_protocol_noteroute.py
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402


class NoteRoutes(unittest.TestCase):
    def test_layout(self):
        m = P.note_route(5, 9, 2, 3, 7)
        self.assertEqual(list(m), [0xF0, 0x7D, 0x52, 0x54, 0x51, 1, 0x28, 5, 9, 2, 3, 7, 0xF7])
        self.assertEqual(list(P.note_reroute(1, 2, 3, P.NOTE_USBC, P.NOTE_ANY_CHAN, 15))[6:-1], [0x29, 1, 2, 3, 0x10, 0x7F, 15])
        self.assertEqual(list(P.note_unroute(4))[6:-1], [0x2A, 4, 0x7F])
        self.assertEqual(list(P.note_devices_ask(8))[6:-1], [0x2B, 8])
        self.assertEqual(list(P.note_noted(5, 9, 0))[6:-1], [0x68, 5, 9, 0])
        self.assertEqual(list(P.note_refused(5, 9, P.NOTE_FULL))[6:-1], [0x69, 5, 9, 3])

    def test_roundtrip(self):
        for m, want in (
            (P.note_route(5, 9, P.NOTE_USBA_ANY, 0, 1), {"op": "NOTES", "seq": 5, "owner": 9, "src": 15, "chan": 0, "slot": 1}),
            (P.note_reroute(1, 2, 3, 0, 4, 5), {"op": "RENOTE", "seq": 1, "owner": 2, "route": 3, "src": 0, "chan": 4, "slot": 5}),
            (P.note_unroute(4, 2), {"op": "UNNOTE", "owner": 4, "route": 2}),
            (P.note_devices_ask(8), {"op": "DEVICES", "seq": 8}),
            (P.note_noted(5, 9, 7), {"op": "NOTED", "seq": 5, "owner": 9, "route": 7}),
            (P.note_refused(5, 9, P.NOTE_BAD), {"op": "REFUSED", "seq": 5, "owner": 9, "code": 5}),
        ):
            cmd, body = P.parse(m)
            self.assertEqual(cmd, P.STORE)
            self.assertEqual(P.decode_note_route(body), want)

    def test_device_list(self):
        m = P.note_device_list(P.NOTE_UNASKED, [(0, "KEYSTEP"), (2, "A VERY LONG KEYBOARD NAME THAT IS CUT"), (3, "Bé")])
        self.assertTrue(all(b < 0x80 for b in m[1:-1]))
        cmd, body = P.parse(m)
        d = P.decode_note_route(body)
        self.assertEqual(d["seq"], 0x7F)
        self.assertEqual(d["devices"], [(0, "KEYSTEP"), (2, "A VERY LONG KEYBOARD NAM"), (3, "B_")])
        # four full names fit one message (the core's 128-byte answer)
        self.assertLessEqual(len(P.note_device_list(1, [(i, "X" * 40) for i in range(4)])), 128)
        self.assertEqual(P.decode_note_route(P.parse(P.note_device_list(1, []))[1])["devices"], [])
        self.assertIsNone(P.decode_note_route(bytes([1, 0x6A, 1, 1, 0, 9, 0x41])))   # a name past the end

    def test_not_ours(self):
        self.assertIsNone(P.decode_note_route(bytes([1, 0x20, 1, 1, 2, 4, 0, 0])))   # a route (audio) op
        self.assertIsNone(P.decode_note_route(bytes([2, 0x28, 1, 1, 0, 0, 0])))      # another version
        self.assertIsNone(P.decode_note_route(bytes([1, 0x28, 1, 1, 0, 0])))         # short

    def test_no_clash(self):
        seen = {}
        for name, ops in P.STORE_OPS.items():
            for op in ops:
                self.assertLess(op, 0x80)
                self.assertNotIn(op, seen, "51 op %02X: %s and %s" % (op, seen.get(op), name))
                seen[op] = name
        mine = [P.NOTES, P.RENOTE, P.UNNOTE, P.NOTE_DEVICES, P.NOTED, P.NOTE_REFUSED, P.NOTE_DEVICE_LIST]
        self.assertEqual(set(mine) - set(P.STORE_OPS["note routes"]), set())
        # S8h's reserved block (design-s8h-screen-stack-status-2026-11-28, section 4) stays clear, written out here too
        s8h = set(range(0x18, 0x1D)) | set(range(0x58, 0x5D))
        self.assertFalse(s8h & set(P.STORE_OPS["note routes"]))
        # the route's own ops (core/route) and the STORE ops sit beside it, not on it
        self.assertFalse({P.ROUTE, P.MOVE, P.UNROUTE, P.ROUTE_GAIN, P.ROUTED, P.ROUTE_REFUSED} & set(mine))
        # a 51 sub-op, not a top-level command: the 51..53 STORE range keeps its commands
        self.assertEqual(P.STORE, 0x51)


if __name__ == "__main__":
    unittest.main()
