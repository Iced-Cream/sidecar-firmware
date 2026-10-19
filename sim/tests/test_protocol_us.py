# sim/tests/test_protocol_us.py - the user-slot messages 19..1E in sim/protocol.py: round trips, and the
# same bytes as the Teensy's C++ builders (teensy/libraries/RytmPlugins/extras/proto_test.cpp -DPROTO_DUMP).
# run: cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_protocol_us
import os, subprocess, tempfile, unittest
import protocol as P

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
LIB = os.path.join(ROOT, "teensy", "libraries", "RytmPlugins")
KNOBS = [dict(p=1, look="list", kind="enum", role="preset", min=0, max=127, default=0, enum_count=4, long="Algorithm", short="ALG"),
         dict(p=2, look="dial", kind="preset", role="free", min=0, max=127, default=0, long="Preset", short="PRE"),
         dict(p=3, look="fader", kind="number", role="offset", min=10, max=120, default=100, long="Level", short="LEV"),
         dict(p=5, look="dial", kind="pool", role="free", min=0, max=127, default=0, long="Sample", short="SMP"),
         dict(p=9, look="centred", kind="bipolar", role="free", min=0, max=127, default=64, long="Tune\x01", short="TUN")]


class CrossCheck(unittest.TestCase):
    def test_same_bytes_as_the_teensy(self):
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "dump")
            src = os.path.join(LIB, "src")
            subprocess.run(["c++", "-std=c++17", "-O1", "-DPROTO_DUMP", "-I", src, os.path.join(LIB, "extras", "proto_test.cpp"),
                            os.path.join(src, "rmp_proto.cpp"), os.path.join(src, "rmp_manifest.cpp"),
                            os.path.join(src, "rmp_loader.cpp"), "-o", exe], check=True)
            got = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split()
        # the Teensy always sends the 43-byte part 0 (spec 2.6): the READY slot's fp, 0 for the EMPTY one
        meta = P.meta_messages(1, 3, P.ST_READY, P.FLAG_PARAMS | P.FLAG_PRESETS, "NP CLUST1", "NPC1", "PAGE TWO", KNOBS,
                               fp=P.plugin_fp("rytm.v2_test"))
        want = [P.meta_unchanged(2, 5), meta[0], meta[1], meta[2], P.meta_messages(4, 9, P.ST_EMPTY, fp=0)[0],
                P.list_messages(1, P.LIST_PRESETS, 3, [f"PRE{i}" for i in range(8)], total=10)[0],
                P.knobs(3, 2, [(1, 64), (2, 100)]), P.slots([1, 2, 3, 4, 5], 7), P.slots([1, 2, 3, 4, 5], 7, 2, P.RES_NO_ROOM)]
        self.assertEqual(got, [m.hex() for m in want])


class RoundTrip(unittest.TestCase):
    def test_ask_and_assign(self):
        m = P.ask(3, 7, P.ASK_ENUM, 2)
        self.assertEqual(len(m), 11)
        cmd, pl = P.parse(m)
        self.assertEqual((cmd, P.decode_ask(pl)), (P.ASK, dict(slot=3, gen=7, what=P.ASK_ENUM, arg=2)))
        cmd, pl = P.parse(P.assign(5, P.OP_INSTALL, 0, 9))
        self.assertEqual((cmd, P.decode_assign(pl)), (P.ASSIGN, dict(slot=5, op=P.OP_INSTALL, arg=0, installed_gen=9)))

    def test_meta(self):
        msgs = P.meta_messages(1, 3, P.ST_READY, P.FLAG_PARAMS, "NP CLUST1", "NPC1", "PAGE TWO", KNOBS)
        self.assertEqual([len(m) for m in msgs], [41, 106, 34])
        heads = [P.decode_meta(P.parse(m)[1]) for m in msgs]
        self.assertEqual((heads[0]["part"], heads[0]["state"], heads[0]["nknobs"], heads[0]["nparts"], heads[0]["name"],
                          heads[0]["short"], heads[0]["p2_title"]), (0, P.ST_READY, 5, 3, "NP CLUST1", "NPC1", "PAGE TWO"))
        k = heads[1]["knobs"] + heads[2]["knobs"]
        self.assertEqual([x["p"] for x in k], [1, 2, 3, 5, 9])
        self.assertEqual((k[2]["look"], k[2]["kind"], k[2]["role"], k[2]["min"], k[2]["max"], k[2]["default"]),
                         ("fader", "number", "offset", 10, 120, 100))
        self.assertEqual((k[4]["long"], k[0]["enum_count"]), ("Tune_", 4))
        u = P.decode_meta(P.parse(P.meta_unchanged(2, 5))[1])
        self.assertEqual((u["slot"], u["gen"], u["unchanged"]), (2, 5, True))

    def test_list_knobs_slots(self):
        names = [f"N{i}" for i in range(10)]
        msgs = P.list_messages(1, P.LIST_PRESETS, 3, names)
        self.assertEqual(len(msgs), 2)
        a, b = (P.decode_list(P.parse(m)[1]) for m in msgs)
        self.assertEqual((a["replace"], a["total"], a["entries"][0], b["replace"], b["entries"][-1]),
                         (True, 10, (0, "N0"), False, (9, "N9")))
        empty = P.list_messages(0, P.LIST_INSTALLED, 2, [])
        self.assertEqual((len(empty), P.decode_list(P.parse(empty[0])[1])["total"]), (1, 0))
        self.assertEqual(P.decode_knobs(P.parse(P.knobs(3, 2, [(1, 64), (15, 127)]))[1]),
                         dict(voice=3, slot=2, knobs=[(1, 64), (15, 127)]))
        self.assertEqual(P.decode_slots(P.parse(P.slots([1, 2, 3, 4, 5], 7))[1]), dict(gens=[1, 2, 3, 4, 5], installed=7, answer=None))
        self.assertEqual(P.decode_slots(P.parse(P.slots([1, 2, 3, 4, 5], 7, 2, P.RES_NO_ROOM))[1])["answer"], (2, P.RES_NO_ROOM))

    def test_unknown_version_is_none(self):
        m = bytearray(P.ask(1, 1, P.ASK_META))
        m[5] = 2
        self.assertIsNone(P.decode_ask(P.parse(bytes(m))[1]))
        m = bytearray(P.slots([1] * 5, 1))
        m[5] = 9
        self.assertIsNone(P.decode_slots(P.parse(bytes(m))[1]))


class SoundTag(unittest.TestCase):
    """The plugin fingerprint (2026-10-10, rytm-mods 0021 sound-tag-research.md): 1A part 0's two optional bytes and
    the tag the Rytm appends to 0x04 / 0x03; both directions backward compatible."""

    def test_plugin_fp(self):
        # FNV-1a 32 of the id ("a": 0xe40c292c), mod 16383, + 1: 1..16383 (0 = untagged), 14 bits
        self.assertEqual(P.plugin_fp("a"), 0xe40c292c % 16383 + 1)
        self.assertEqual(P.plugin_fp(""), 0x811c9dc5 % 16383 + 1)
        for i in range(500):
            self.assertTrue(1 <= P.plugin_fp(f"rytm.plugin{i}") <= 0x3fff)
        self.assertNotEqual(P.plugin_fp("rytm.v2_test"), P.plugin_fp("rytm.sine_test"))

    # Shared with the Teensy's C++ plugin_fp (spec 2.6): the same ids must give the same values there. The id
    # bytes exactly as the package stores them (case kept, no version).
    FP_VECTORS = (("", 8768), ("a", 14699), ("rytm.v2_test", 12437), ("befaco.np.atari", 3424),
                  ("befaco.np.a", 11572), ("RYTM.V2_TEST", 8663))

    def test_plugin_fp_vectors(self):
        for pkg_id, fp in self.FP_VECTORS:
            self.assertEqual(P.plugin_fp(pkg_id), fp, pkg_id)

    def test_meta_fp(self):
        old = P.meta_messages(1, 3, P.ST_READY, 0, "NP", "NP", "", KNOBS)
        new = P.meta_messages(1, 3, P.ST_READY, 0, "NP", "NP", "", KNOBS, fp=0x2abc)
        self.assertEqual((len(old[0]), len(new[0])), (41, 43))
        self.assertEqual(new[0][-3:], bytes([0x2abc >> 7, 0x2abc & 0x7f, 0xf7]))
        self.assertEqual(new[0][:40], old[0][:40])
        self.assertEqual(new[1:], old[1:])
        self.assertEqual(P.decode_meta(P.parse(new[0])[1])["fp"], 0x2abc)
        self.assertEqual(P.decode_meta(P.parse(old[0])[1])["fp"], 0)   # an old Teensy: untagged
        self.assertEqual(P.decode_meta(P.parse(new[0])[1])["name"], "NP")
        self.assertIsNone(P.decode_meta(P.parse(new[0])[1][:-1]))      # 36: neither form
        self.assertTrue(all(b < 0x80 for b in P.meta_messages(1, 1, 1, fp=0xffff)[0][1:-1]))

    def test_reports_tag(self):
        ps = [64 << 7 | 3] * 16
        for tag in (None, 0, 0x1234):
            m = P.trig_params(0x50 | 3, 60, 100, ps, tag)
            self.assertEqual(len(m), 41 if tag is None else 43)
            self.assertEqual(P.decode_trig_params(P.parse(m)[1]),
                             dict(voice=3, machine=0x50, note=60, vel=100, params=ps, tag=tag or 0))
            m = P.params_report(0x58 | 1, ps, tag)
            self.assertEqual(len(m), 39 if tag is None else 41)
            self.assertEqual(P.decode_params(P.parse(m)[1]), dict(voice=1, machine=0x58, params=ps, tag=tag or 0))
        # the Rytm's own bytes (ext_render): tag hi, lo before F7
        m = P.HDR + bytes([P.TRIG_PARAMS, 0x50, 60, 100] + [64, 0] * 16 + [0x21, 0x05, 0xf7])
        self.assertEqual(P.decode_trig_params(P.parse(m)[1])["tag"], 0x21 << 7 | 5)
        # P1..P8 only (older firmware) still decodes, tag 0; other lengths are None
        m = P.HDR + bytes([P.TRIG_PARAMS, 0, 60, 100] + [1, 0] * 8 + [0xf7])
        self.assertEqual(P.decode_trig_params(P.parse(m)[1])["params"], [128] * 8)
        self.assertIsNone(P.decode_trig_params(P.parse(m)[1][:-1]))
        self.assertIsNone(P.decode_params(bytes(36)))


class Malformed(unittest.TestCase):
    def test_truncated_and_bad_payloads_are_none(self):
        meta = [P.parse(m)[1] for m in P.meta_messages(1, 3, P.ST_READY, 0, "A", "B", "C", KNOBS)]
        for pl in (meta[0][:20], meta[1][:30], meta[1][:3], P.parse(P.meta_unchanged(1, 1))[1][:3]):
            self.assertIsNone(P.decode_meta(pl))
        bad = bytearray(meta[1])
        bad[5] = 9   # the first knob's look: no such look
        self.assertIsNone(P.decode_meta(bytes(bad)))
        lst = P.parse(P.list_messages(1, P.LIST_PRESETS, 3, ["A", "B"])[0])[1]
        self.assertIsNone(P.decode_list(lst[:-3]))
        self.assertIsNone(P.decode_list(lst[:4]))
        kn = P.parse(P.knobs(3, 2, [(1, 64), (2, 100)]))[1]
        self.assertIsNone(P.decode_knobs(kn[:-1]))
        self.assertIsNone(P.decode_knobs(kn[:3]))
        self.assertIsNone(P.decode_ask(P.parse(P.ask(1, 1, P.ASK_META))[1][:4]))
        self.assertIsNone(P.decode_assign(P.parse(P.assign(1, 0, 0, 0))[1] + b"\0"))
        self.assertIsNone(P.decode_slots(P.parse(P.slots([1] * 5, 1))[1][:5]))

    def test_builders_stay_7_bit(self):
        msgs = [P.meta_unchanged(0x81, 1), P.meta_messages(0x81, 1, 0x86, 0, knobs=[dict(p=0x89, min=200)])[1],
                P.list_messages(0x81, 0xA0, 1, ["A"], total=200)[0], P.list_messages(1, 0x20, 1, [], total=-1)[0],
                P.knobs(1, 0x82, [(1, 2)]), P.slots([1] * 5, 1, 0x83, 0x85)]
        for m in msgs:
            self.assertTrue(all(b < 0x80 for b in m[1:-1]), m.hex())
        self.assertEqual(P.decode_list(P.parse(msgs[2])[1])["total"], 127)
        self.assertEqual(P.decode_list(P.parse(msgs[3])[1])["total"], 0)
        self.assertEqual(P.decode_slots(P.parse(msgs[5])[1])["answer"], (3, 5))


if __name__ == "__main__":
    unittest.main()
