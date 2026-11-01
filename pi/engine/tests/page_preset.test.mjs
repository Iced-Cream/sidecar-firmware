// pi/engine/tests/page_preset.test.mjs - the Pool tab's JUNO / 6-OP FM lists on the page (pi/engine/www/index.html): the
// two extra tabs, the list's rows and the index's rows as HTML (a folder opens, a preset adds, or sets the slot chosen), the
// escaping of names, and against mock_web.py's /presets and preset=... lines: the list, a folder, an add into the first empty
// slot, a set into a slot, the stale gen refused. node --test pi/engine/tests/page_preset.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(HERE, "..", "www", "index.html"), "utf8");
const cut = (from, to) => {
  const a = page.indexOf(from), b = page.indexOf(to, a);
  assert.ok(a > 0 && b > a, "in index.html: " + from);
  return page.slice(a, b);
};
const F = new Function(cut("const esc =", "const join =") +
  cut("const preListHtml", "let prIx = null") + "; return { preListHtml, preIxHtml };")();

test("the Pool tab has JUNO and 6-OP FM, the bank view's parts are marked to hide, the notes say what they do", () => {
  assert.match(page, /data-pk="juno"[^>]*>JUNO<\/button><button data-pk="fm"[^>]*>6-OP FM<\/button>/);
  for (const id of ["pl-pre", "pl-pre-rows", "pl-pre-ix", "pl-pre-list", "pl-pre-add", "pl-pre-up", "pl-pre-prev", "pl-pre-next"])
    assert.ok(page.includes(`id="${id}"`), id);
  assert.equal((page.match(/<[a-z]+ [^>]*data-nopre/g) || []).length, 4);   // the banks, the buttons, the table, the file picker
  assert.match(page, /#pl \[hidden\] \{ display:none !important; \}/);
  assert.match(page, /Add presets browses the RAM index of \/JUNO/);
});

test("the list's rows: PAT, the preset, an empty slot, Replace; the built-ins say so; names are escaped", () => {
  const h = F.preListHtml({ o: 0, builtin: false, rows: [["Brass", 2], ["", 0], ["a<b>\"x", 2]] });
  assert.match(h, /<td class="r">0<\/td><td>Brass<\/td>/);
  assert.match(h, /<td class="r">1<\/td><td><span class="mut">-<\/span><\/td>/);
  assert.match(h, /a&#60;b&#62;&#34;x/);
  assert.equal((h.match(/data-pre="rep"/g) || []).length, 3);
  assert.match(h, /data-at="2"/);
  assert.match(F.preListHtml({ o: 0, builtin: true, rows: [["Brass", 2]] }), /built-in/);
  assert.match(F.preListHtml({ o: 40, rows: [["x", 2]] }), /<td class="r">40<\/td>/);
  assert.match(F.preListHtml({ rows: [] }), /colspan="3"/);
});

test("the index's rows: a folder opens (its child), a preset adds, or sets the chosen slot; paged rows carry their place", () => {
  const d = { o: 50, ready: true, rows: [["FACTORY", 1], ["lead", 0]], kids: [2, -1] };
  const h = F.preIxHtml(d, -1);
  assert.match(h, /data-pre="dir" data-kid="2">FACTORY\/</);
  assert.match(h, /data-pre="add" data-row="51">Add</);
  assert.match(F.preIxHtml(d, 7), /data-row="51">Set slot 7</);
  assert.match(F.preIxHtml({ ready: false, rows: [] }, -1), /reading the card/);
  assert.match(F.preIxHtml({ ready: true, rows: [] }, -1), /empty folder/);
});

test("against mock_web.py: the list, a folder, add into the first empty slot, set a slot, a stale gen refused", async () => {
  const port = 19500 + Math.floor(Math.random() * 400), base = `http://127.0.0.1:${port}`;
  const srv = spawn("python3", [join(HERE, "mock_web.py"), String(port)], { stdio: "ignore", env: { ...process.env, MOCK_PAIR: "0" } });
  try {
    let up = false;
    for (let i = 0; i < 50 && !up; i++) { try { up = (await fetch(base + "/status")).ok; } catch (e) { await new Promise((r) => setTimeout(r, 100)); } }
    assert.ok(up, "mock_web answers");
    const pre = async (q) => (await fetch(base + "/presets?" + q)).json();
    const cmd = (c) => fetch(base + "/cmd", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: "c=" + encodeURIComponent(c) });
    let l = await pre("k=juno&f=-1&o=0&n=200");
    assert.equal(l.ready, true);
    assert.deepEqual(l.rows.slice(0, 2), [["Brass", 2], ["Strings", 2]]);
    const root = await pre("k=juno&f=0");
    assert.equal(root.title, "JUNO");
    assert.deepEqual(root.rows[0], ["FACTORY", 1]);
    const fac = await pre(`k=juno&f=${root.kids[0]}`);
    assert.equal(fac.up, 0);
    assert.equal(fac.rows[1][0], "A12 Brass 2");
    // add: the first empty slot (2)
    const seq0 = l.msg.seq;
    await cmd(`preset=juno:add:${root.gen}:${root.kids[0]}:1`);
    l = await pre("k=juno&f=-1&o=0&n=200");
    assert.equal(l.msg.seq, seq0 + 1);
    assert.equal(l.msg.text, "JUNO 002 ADDED");
    assert.deepEqual(l.rows[2], ["A12 Brass 2", 2]);
    // set: slot 0
    await cmd(`preset=juno:set:${root.gen}:${root.kids[0]}:2:0`);
    l = await pre("k=juno&f=-1&o=0&n=200");
    assert.equal(l.msg.text, "JUNO 000 SET");
    assert.equal(l.rows[0][0], "A13 Strings");
    // a stale gen: refused, nothing changes
    await cmd(`preset=juno:add:${root.gen + 1}:${root.kids[0]}:0`);
    l = await pre("k=juno&f=-1&o=0&n=200");
    assert.equal(l.msg.text, "NOT ADDED");
    assert.equal(l.rows.length, 3);
    // FM: the built-ins, then a voice into slot 5 (the built-ins are the rows)
    const fm = await pre("k=fm&f=-1&n=200");
    assert.equal(fm.builtin, true);
    assert.equal(fm.total, 96);
  } finally { srv.kill(); }
});
