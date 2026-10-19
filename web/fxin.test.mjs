// node web/fxin.test.mjs - the Sound tab's IN L/R source switch (fxin.js): the model, and the mock device's :fxin
// answers (mockdev.js) as the page parses them.
import assert from "node:assert/strict";
import { fxinModel, FX_NAMES } from "./fxin.js";
import { MockDevice } from "./mockdev.js";
import { parseReply } from "./proto.js";

// the confirmed state shows, not the setting
assert.deepEqual(fxinModel(null).sel, "");
let m = fxinModel({ t: "fxin", fxin: { want: 1, state: "on", rytm: 1 } });
assert.equal(m.sel, "fx"); assert.equal(m.warn, true); assert.deepEqual(m.names, FX_NAMES); assert.match(m.note, /channels 10\/11/);
m = fxinModel({ t: "fxin", fxin: { want: 0, state: "off", rytm: 0 } });
assert.equal(m.sel, "in"); assert.equal(m.warn, false); assert.deepEqual(m.names, ["IN L", "IN R"]);
m = fxinModel({ t: "fxin", fxin: { want: 1, state: "unknown", rytm: -1 } });
assert.equal(m.sel, ""); assert.match(m.note, /waiting/);
m = fxinModel({ t: "fxin", fxin: { want: 1, state: "unsupported", rytm: -1 } });
assert.equal(m.sel, ""); assert.match(m.note, /not supported by this Rytm build/); assert.equal(m.warn, false);

// the mock device: a Rytm with 0030 follows the setting after its answer; an older one never answers
function dev(opts) {
  const got = [];
  const d = new MockDevice((l) => { const o = parseReply(l); if (o) got.push(o); }, { rate: 0, ...opts });
  return { d, last: () => got.filter((o) => o.t === "fxin").pop() };
}
{
  const { d, last } = dev({});
  d.console("fxin");
  assert.equal(last().fxin.state, "off");
  d.console("fxin=1");
  assert.equal(last().fxin.want, 1); assert.equal(last().fxin.state, "off", "not confirmed yet");
  await new Promise((r) => setTimeout(r, 300));
  d.console("fxin");
  assert.equal(last().fxin.state, "on");
  d.console("fxin=0");
  await new Promise((r) => setTimeout(r, 300));
  d.console("fxin");
  assert.equal(last().fxin.state, "off");
}
{
  const { d, last } = dev({ oldRytm: true });
  d.console("fxin=1");
  await new Promise((r) => setTimeout(r, 300));
  d.console("fxin");
  assert.equal(last().fxin.state, "unsupported");
}
console.log("fxin.test: ok");
{
  const { readFileSync } = await import("node:fs");
  const html = readFileSync(new URL("./index.html", import.meta.url), "utf8"), app = readFileSync(new URL("./app.js", import.meta.url), "utf8");
  assert.match(html, /data-key="fxsrc"><button[^>]*data-cmd="fxin=0" data-val="in"/, "the shared choice group");
  assert.match(app, /pend\.report\("fxsrc", v\.sel\)/, "confirmed through the shared Pending");
}
