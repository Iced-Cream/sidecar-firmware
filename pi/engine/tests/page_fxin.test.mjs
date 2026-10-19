// pi/engine/tests/page_fxin.test.mjs - the Sidecar page's IN L/R source switch (Sound tab): fxinShow shows the Rytm's
// confirmed state from /status "fxin" (core/host_link.cpp), not the setting. node --test pi/engine/tests/page_fxin.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("function fxinShow"), b = page.indexOf('// ---- record (/status "rec"');
assert.ok(a > 0 && b > a, "fxinShow in index.html");
assert.match(page, /data-key="fxsrc"><button[^>]*data-cmd="fxin=0" data-val="in"/, "the shared choice group (pending / confirmed)");
assert.match(page, /data-cmd="fxin=1" data-val="fx"/);
assert.match(page, /k === "fxsrc"\) return st\.fxin && st\.fxin\.state === "on" \? "fx"/, "chosen(): the Rytm's confirmed state");
assert.match(page, /Never send channels 10\/11 back into the Rytm/);

function dom() {
  const mk = (ds) => { const cl = new Set(); return { dataset: ds, classList: { toggle: (c, on) => (on ? cl.add(c) : cl.delete(c)), has: (c) => cl.has(c) }, hidden: true, textContent: "" }; };
  const lab = [11, 12].map((id) => { const l = mk({ nm: id === 11 ? "AUDIO IN L" : "AUDIO IN R", fx: "FX RETURN " + (id === 11 ? "L" : "R") }); l.lastChild = { textContent: " " + l.dataset.nm }; return l; });
  const els = { "#fxin-warn": mk({}), "#fxin-note": mk({}) };
  const document = { querySelectorAll: (q) => (q === "[data-nm]" ? lab : []) };
  const show = new Function("$", "document", page.slice(a, b) + "; return fxinShow;")((q) => els[q], document);
  return { show, lab, els };
}

test("the confirmed state is what shows", () => {
  const d = dom();
  d.show({ want: 1, state: "on", rytm: 1 });
  assert.equal(d.els["#fxin-warn"].hidden, false, "the feedback warning shows while on");
  assert.deepEqual(d.lab.map((l) => l.lastChild.textContent), [" FX RETURN L", " FX RETURN R"]);
  d.show({ want: 0, state: "off", rytm: 0 });
  assert.equal(d.els["#fxin-warn"].hidden, true);
  assert.deepEqual(d.lab.map((l) => l.lastChild.textContent), [" AUDIO IN L", " AUDIO IN R"]);
});

test("wanted but not confirmed, and an old Rytm", () => {
  const d = dom();
  d.show({ want: 1, state: "unknown", rytm: -1 });
  assert.match(d.els["#fxin-note"].textContent, /waiting/);
  d.show({ want: 1, state: "unsupported", rytm: -1 });
  assert.match(d.els["#fxin-note"].textContent, /not supported by this Rytm build/);
  assert.equal(d.els["#fxin-warn"].hidden, true);
});
