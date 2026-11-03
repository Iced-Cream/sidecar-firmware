// pi/engine/tests/page_tiles.test.mjs - fix1049 (hardware pass 2026-10-23: the STATUS cards cut "8.1 / 1.7 / 10....",
// "block max 22 us · pas..." and the FIFO card at a phone's width): a card's value and its detail wrap, never cut, and
// the detail is one part a line (pi/engine/www/index.html tileLines). Checked by eye too: mock_web.py at 390 px and
// 1280 px, no card's text wider than its card. node --test pi/engine/tests/page_tiles.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const a = page.indexOf("function tileLines("), b = page.indexOf("function tile(");
assert.ok(a > 0 && b > a, "tileLines in index.html");
const F = new Function(page.slice(a, b) + "; return { tileLines };")();
const rule = (sel) => {
  const m = page.match(new RegExp(sel.replace(/\./g, "\\.").replace(/ /g, "\\s+") + "\\s*\\{([^}]*)\\}"));
  assert.ok(m, sel);
  return m[1];
};

test("a card's detail: one part a line", () => {
  assert.equal(F.tileLines("block max 22 us · pass max 1234 us"), "block max 22 us\npass max 1234 us");
  assert.equal(F.tileLines("IN 1500/s · gap 0 · lost 0 · link usb"), "IN 1500/s\ngap 0\nlost 0\nlink usb");
  assert.equal(F.tileLines("target 96 frames"), "target 96 frames");
  assert.equal(F.tileLines(""), "\u00a0");
  assert.equal(F.tileLines(undefined), "\u00a0");
});

test("the value and the detail wrap, nothing cut with an ellipsis", () => {
  for (const sel of [".tile .v", ".tile .d"]) {
    const r = rule(sel);
    assert.ok(!/nowrap/.test(r) && !/ellipsis/.test(r) && !/overflow:\s*hidden/.test(r), sel + ": " + r);
    assert.ok(/overflow-wrap:\s*anywhere/.test(r), sel + " breaks a long word");
  }
  assert.ok(/pre-line/.test(rule(".tile .d")), "the detail's lines kept");
  const px = +rule(".tile .v").match(/font-size:\s*(\d+)px/)[1];
  assert.ok(px <= 18, "the value at most 18 px (the CPU card's three figures in a 179 px card at 390 px)");
});

// 0.3.0: the temperature card (pi/engine/thermal.h: warned from 80 °C, off below 75; clockLow, the clock Circle's
// throttle lowered at socmaxtemp 78): honest about the clock, quiet under the warning
test("the temperature card says the clock was lowered", () => {
  const i = page.indexOf("function tempTile("), j = page.indexOf("function tileLines(");
  assert.ok(i > 0 && j > i, "tempTile in index.html");
  const T = new Function(page.slice(i, j) + "; return tempTile;")();
  assert.deepEqual(T({ temp: 60, tempWarn: false, clockLow: 0 }), ["SoC", ""]);
  assert.deepEqual(T({ temp: 79, tempWarn: false, clockLow: 600 }), ["SoC · clock lowered to 600 MHz (hot)", ""]);
  assert.deepEqual(T({ temp: 80, tempWarn: true, clockLow: 600 }), ["SoC · clock lowered to 600 MHz · warning until below 75 °C", "bad"]);
  assert.deepEqual(T({ temp: 80, tempWarn: true, clockLow: 0 }), ["SoC · running hot (warning until below 75 °C)", "bad"]);
  assert.deepEqual(T({ temp: 50 }), ["SoC", ""]);   // an older engine: no clockLow
});

// 0.4.0: the USB-C card, the Pi 5's computer audio on as on the Pi 4 (/status "usbHost": a computer has configured the
// device; false on a plain power supply, where no USB reset ever comes)
test("the USB-C card says whether a computer is there", () => {
  const i = page.indexOf("function gadgetTile("), j = page.indexOf("function tempTile(");
  assert.ok(i > 0 && j > i, "gadgetTile in index.html");
  const G = new Function(page.slice(i, j) + "; return gadgetTile;")();
  const sc = "sidecar: timer alt 1/1 mac on pk 0 | out ... | late 0/0/0 incompl 0/0/0";
  assert.deepEqual(G({ gadget: "sidecar", mac: true, usbHost: false }, sc), ["sidecar", "no computer on USB-C · computer audio on"]);
  assert.deepEqual(G({ gadget: "sidecar", mac: false, usbHost: true }, sc), ["sidecar", "alt 1/1 · computer audio off · late 0/0/0"]);
  assert.deepEqual(G({ gadget: "sidecar", mac: true, usbHost: true }, undefined), ["sidecar", "UAC2 12/12 + MIDI"]);
  assert.deepEqual(G({ gadget: "sidecar", mac: true }, sc), ["sidecar", "alt 1/1 · computer audio on · late 0/0/0"]);   // an older engine: no usbHost
  assert.deepEqual(G({ gadget: "0", mac: null, usbHost: null }, undefined), ["off", "off"]);
  assert.deepEqual(G({ gadget: "midi", mac: null, usbHost: null }, undefined), ["midi", "MIDI echo"]);
});
