// node web/rec.test.mjs - the Record tab's model (rec.js recModel) and the mock device's recorder (mockdev.js), which
// answers as teensy/voiceloop/rec_mini.h: stereo only without PSRAM, stems with it, start / stop, arming by PLAY / STOP.
import assert from "node:assert/strict";
import { recModel, hms, okName, REC_SOURCES } from "./rec.js";
import { MockDevice } from "./mockdev.js";
import { parseReply } from "./proto.js";

assert.equal(hms(3725), "1:02:05");
assert.ok(okName("take_1") && !okName("a b") && !okName(""));
assert.equal(REC_SOURCES.length, 13);

function rig(psram) {
  let t = 0;
  const replies = [], lines = [];
  const dev = new MockDevice((l) => { lines.push(l); const o = parseReply(l); if (o) replies.push(o); }, { rate: 0, psram, now: () => t });
  const cmd = (c) => { dev.feed(new TextEncoder().encode(":" + c + "\n")); return replies.filter((o) => o.t === "rec").at(-1); };
  return { dev, cmd, lines, tick: (ms) => (t += ms) };
}

{ // a stock Teensy: stereo only
  const { cmd, lines, tick, dev } = rig(false);
  let r = cmd("rec");
  assert.equal(r.state, 0); assert.equal(r.mode, "stereo"); assert.equal(r.psram, 0); assert.equal(r.ringKB, 32);
  let v = recModel(r);
  assert.equal(v.stemsOk, false); assert.equal(v.button, "● Record"); assert.match(v.card, /32 KB buffer .* internal RAM, 4 KB writes/);
  assert.equal(r.usbShared, 1); assert.equal(r.usbPaused, 0); assert.match(v.usb, /pauses the USB audio/);   // the shared pool
  r = cmd("rec=mode:stems");
  assert.equal(r.mode, "stereo");
  assert.ok(lines.some((l) => /stems need PSRAM/.test(l)));
  r = cmd("rec=name:jam");
  r = cmd("rec=start");
  assert.equal(r.state, 2); assert.equal(r.take, "jam_001");
  tick(2500);
  r = cmd("rec");
  v = recModel(r);
  assert.equal(v.time, "0:00:02"); assert.equal(v.button, "■ Stop"); assert.equal(v.buttonClass, "on");
  assert.equal(r.usbPaused, 1); assert.match(v.usb, /is paused while the recorder/);
  assert.ok(v.levels[0].on && v.levels[0].pct > 0 && !v.levels[3].on);
  r = cmd("rec=bits:16");   // refused while recording
  assert.equal(r.bits, 24);
  r = cmd("rec=stop");
  assert.equal(r.state, 0); assert.equal(r.usbPaused, 0);
  assert.ok(dev.fs.has("/Recordings/jam_001/00_MASTER.wav"));
  assert.equal(recModel(r).state, "last take: jam_001");
}

{ // PSRAM: stems; arming by PLAY / STOP
  const { cmd, tick, dev } = rig(true);
  let r = cmd("rec=mode:stems");
  assert.equal(r.mode, "stems"); assert.equal(r.ch.length, 12); assert.equal(recModel(r).stemsOk, true);
  assert.equal(r.usbShared, 0); assert.equal(recModel(r).usb, "");   // PSRAM: the ring is there, USB audio never paused
  r = cmd("rec=arm");
  assert.equal(r.state, 1); assert.equal(recModel(r, "arm").button, "■ Disarm");
  dev.recTransport(true);
  tick(1000);
  r = cmd("rec");
  assert.equal(r.state, 2); assert.equal(r.frames, 48000);
  dev.recTransport(false);
  r = cmd("rec");
  assert.equal(r.state, 1);   // re-armed
  assert.ok(dev.fs.has("/Recordings/take_001/01_MAIN_L.wav") && dev.fs.has("/Recordings/take_001/12_IN_R.wav"));
  r = cmd("rec=stop");
  assert.equal(r.state, 0);
}

{ // warnings: a slow card, an error
  const v = recModel({ state: 2, ready: 1, take: "t_001", frames: 96000, drops: 3, silence: 21, error: "card full", writes: 9, cardKBps: 2048,
    needKBps: 281, worstWriteMs: 40, ringMax: 99, busy: 12, bytes: 500, ringKB: 16, ringMs: 56, blockKB: 4, freeMB: 900, totalMB: 30000, ch: [0], peak: [995] });
  assert.match(v.warn[0], /3 blocks dropped/); assert.equal(v.warn[1], "card full");
  assert.ok(v.levels[0].hot);
  assert.match(v.card, /worst write 40 ms/);
}
console.log("rec.test: PASS");
