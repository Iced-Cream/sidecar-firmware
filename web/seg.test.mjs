// node web/seg.test.mjs - the Sound tab's choice groups (seg.js): the console's answers as state (against the mock
// device's wording, which is the firmware's), and the pending / confirmed / reverted flow with a fake clock.
import assert from "node:assert/strict";
import { Pending, parseDevState } from "./seg.js";
import { MockDevice } from "./mockdev.js";

// every state-reporting answer of the mock device parses to the state it holds
{
  const lines = [];
  const dev = new MockDevice((l) => lines.push(l), { rate: 0 });
  const ask = (c) => { lines.length = 0; dev.feed(new TextEncoder().encode(":" + c + "\n")); return lines.map(parseDevState).filter(Boolean); };
  assert.deepEqual(ask("notes")[0].state, { notes: false, mach: Object.fromEntries([0, 1, 2, 3, 4, 5, 6, 7].map((v) => [v, "plaits"])) });
  assert.deepEqual(ask("notes=1")[0].state, { notes: true });
  assert.deepEqual(ask("out=main")[0].state, { out: "main" });
  assert.deepEqual(ask("out")[0].state, { out: "main" });
  assert.deepEqual(ask("mac=off")[0].state, { mac: false });
  assert.deepEqual(ask("mac=on")[0].state, { mac: true });
  assert.deepEqual(ask("fifo=128")[0].state, { fifo: 128 });
  assert.deepEqual(ask("fifo=8")[0].state, { fifo: 24 });   // limited by the device: its answer is the state
  assert.deepEqual(ask("notemach=3:juno")[0].state, { mach: { 3: "juno" } });
  assert.deepEqual(ask("notemach=3:nope")[0].reject, ["mach"]);
  assert.deepEqual(ask("notes")[0].state.mach[3], "juno");
}
// the stats lines are marked periodic; the serial build's refusals reject the key
assert.deepEqual(parseDevState("midi notes 4, channels ff, MIDI notes on"), { state: { notes: true }, periodic: true });
assert.equal(parseDevState("cpu 21 % peak block 3 % stream underruns 0 fifo 90 (min 80 max 99 target 96)").periodic, true);
assert.deepEqual(parseDevState("output: the host build's (RYTM_USB_HOST=1); here the serial link carries the 8 voices").reject, ["out"]);
assert.deepEqual(parseDevState('console: unknown command "fifo=96" (notes=1|0)').reject, ["fifo"]);
assert.deepEqual(parseDevState("uac2: not in this build (RMP_CORE=uac2)").reject, ["mac"]);
assert.equal(parseDevState("usb host: ob streaming"), null);

// pending: a click, the confirming answer, a timeout, a refusal, a second click replacing the first
{
  const ev = [], timers = [];
  let id = 0;
  const p = new Pending((k, kind, t) => ev.push([k, kind, t]), {
    ms: 2000, set: (f, ms) => (timers.push({ f, ms, id: ++id }), id), clear: (h) => { const i = timers.findIndex((t) => t.id === h); if (i >= 0) timers.splice(i, 1); },
  });
  p.start("out", "main");
  assert.deepEqual(ev.at(-1), ["out", "pending", ""]); assert.equal(p.want("out"), "main");
  assert.equal(p.report("out", "tracks"), false);   // an older answer: still waiting
  assert.equal(p.has("out"), true);
  assert.equal(p.report("out", "main"), true);
  assert.deepEqual(ev.at(-1), ["out", "ok", "saved ✓"]); assert.equal(timers.length, 0);
  p.start("mac", "false");
  timers.shift().f();   // 2 s with no answer
  assert.equal(ev.at(-1)[1], "err"); assert.match(ev.at(-1)[2], /no answer/); assert.equal(p.has("mac"), false);
  p.start("mach", "3:juno");
  assert.equal(p.fail("mach", "no such machine"), true);
  assert.deepEqual(ev.at(-1), ["mach", "err", "no such machine"]);
  assert.equal(p.fail("mach", "x"), false);   // nothing pending: no message
  p.start("notes", "true"); p.start("notes", "false");   // the second click replaces the first, one timer
  assert.equal(timers.length, 1); assert.equal(p.report("notes", "true"), false); assert.equal(p.report("notes", "false"), true);
  p.start("a", 1); p.start("b", 2); p.failAll("disconnected");
  assert.equal(p.has("a") || p.has("b"), false); assert.equal(timers.length, 0);
}
// the USB audio mode: the Sidecar Mini's stereo build (the mock's default) and the 20 / 12 build, the firmware's wording
{
  for (const multiUsb of [false, true]) {
    const lines = [];
    const dev = new MockDevice((l) => lines.push(l), { rate: 0, multiUsb });
    const ask = (c) => { lines.length = 0; dev.feed(new TextEncoder().encode(":" + c + "\n")); return lines.map(parseDevState).filter(Boolean); };
    const a = ask("usbmode");
    assert.deepEqual(a[0].state, { usbmode: multiUsb ? "multi" : "stereo", usbmulti: multiUsb });
    assert.deepEqual(a[1].state, { uacin: true, uacout: false, uacconf: true, uacpaused: false }); assert.equal(a[1].periodic, true);
    assert.deepEqual(ask("usbmode=off")[0], { state: {}, periodic: false, switching: "off" });
    assert.deepEqual(ask(multiUsb ? "usbmode=multi" : "usbmode=nope")[0].reject ?? [], multiUsb ? [] : ["usbmode"]);
  }
  assert.deepEqual(parseDevState("usb: no mode \"multi\" (stereo, off)").reject, ["usbmode"]);
  assert.deepEqual(parseDevState("usb: mode stereo (unchanged) (Serial + audio 2 in / 2 out; multi not in this build)").state,
    { usbmode: "stereo", usbmulti: false });
  assert.deepEqual(parseDevState("uac2: not configured, alt in/out 0/0, in 0 pk 0 fr").state, { uacin: false, uacout: false, uacconf: false, uacpaused: false });
  assert.equal(parseDevState("uac2: 480M, alt in/out 1/1, in 4000 pk 48000 fr (11/12/13 0/4000/0, ring 0, under 0 over 0), fb 48000.000 Hz base 48000.000 Hz, sof 8000, mac on, paused for the recorder (IN silent, OUT dropped)").state.uacpaused, true);
}
console.log("seg.test.mjs ok");
