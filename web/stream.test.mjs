// node web/stream.test.mjs - the parser on realistic lines (docs/teensy-host-rytm.md, voiceloop.ino formats).
import assert from "node:assert/strict";
import { StreamParser, parseStatus } from "./stream.js";

const enc = (s) => new TextEncoder().encode(s);
const cat = (...a) => Uint8Array.from(a.flatMap((x) => [...x]));
const L = (s) => cat([0x4c, s.length], enc(s));

const lines = [
  "cpu 21.9 % (8 voices, real time), peak block 26 %, with card reads 27 %, stream underruns 0",
  "usb host: ob streaming, in 48000 Hz out 48000 Hz, fifo 256 (min 240 max 268 target 256), xruns 0 blocks 0 frames, bad 0 gaps 0 unanswered 0 rearms 0/0 queue fails 0; midi on, clock 120 BPM, sysex in 0 out 0 dropped 0",
  "Loading something with a long tail of text that ends here",
  "MIDI notes on",
];

// plain, split across chunks, with \r and junk bytes
{
  const p = new StreamParser();
  const all = enc(lines.map((l) => l + "\r\n").join(""));
  const got = [];
  for (let i = 0; i < all.length; i += 7) got.push(...p.push(all.subarray(i, i + 7)));
  assert.deepEqual(got, lines);
  assert.equal(p.framed, false);
  assert.deepEqual(p.push(cat([1, 2, 0xff]), true), []);
}

// framed: L lines, X SysEx and A audio skipped, one byte at a time
{
  const p = new StreamParser();
  const audio = cat([0x41, 2, 0], new Uint8Array(32).fill(0x0a));
  const all = cat(L(lines[0]), cat([0x58, 4, 0xf0, 0x00, 0x20, 0xf7]), audio, L(lines[1]), L("pong"), L(lines[3]));
  const got = [];
  for (const b of all) got.push(...p.push(Uint8Array.of(b)));
  got.push(...p.push(new Uint8Array(0), true));
  assert.deepEqual(got, [lines[0], lines[1], "pong", lines[3]]);
  assert.equal(p.framed, true);
}

// status
{
  const s = { ...parseStatus(lines[0]), ...parseStatus(lines[1]) };
  assert.equal(s.cpu, "21.9"); assert.equal(s.peak, "26"); assert.equal(s.underruns, "0");
  assert.equal(s.ob, "streaming"); assert.equal(s.fifoMin, "240"); assert.equal(s.fifoMax, "268");
  assert.equal(s.target, "256"); assert.equal(s.xruns, "0");
  assert.deepEqual(parseStatus("midi notes 12, channels 3, MIDI notes on"), { notes: "12", chans: "3", notesOn: "on" });
  assert.deepEqual(parseStatus("usb host: FIFO target 96 frames"), { target: "96" });
  assert.deepEqual(parseStatus("output: main"), { out: "main" });
  assert.deepEqual(parseStatus("usb host: ob idle, in 0 Hz out 0 Hz"), { ob: "idle" });
  assert.deepEqual(parseStatus("garbage cpu"), {});
}
console.log("stream.test: ok");
