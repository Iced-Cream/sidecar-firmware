// web-next/src/lib/catalog.test.ts
import { createHash } from "node:crypto";
import { describe, expect, it } from "vitest";
import { CAT_BASE, boardOf, catAsk, catList, catShown, catState, fetchPackage, loadIndex, safeUrl, sha256Hex, sha256Js } from "./catalog";

const bytes = new TextEncoder().encode("fake rmp bytes");
const sha = createHash("sha256").update(bytes).digest("hex");
const buf = (u: Uint8Array) => u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength) as ArrayBuffer;
const ent = (o: Record<string, unknown> = {}) => ({ id: "x.one", kind: "machine", version: "1.0.0", compatible_from: "1.0.0", name: "ONE", short: "ONE", author: "someone",
  licence: "MIT", board: "pi4", api: 3, source: "https://example.com/src", sha256: sha, description: "plain", file: "machines/x.one/1.0.0/x_one.pi4.rmp", size: bytes.length, readme: "r", ...o });
const idx = (...e: unknown[]) => ({ schema: 1, entries: e, latest: {} });
const ok = (o: object) => ({ ok: true, ...o }) as unknown as Response;

describe("catalog list", () => {
  it("keeps the newest version of each id and board, drops bad entries, caps the size", () => {
    const L = catList(idx(ent(), ent({ version: "1.2.0" }), ent({ version: "1.10.0" }), ent({ id: "x.two", board: "pi5" }), ent({ id: "x.t", board: "teensy" }),
      ent({ id: "bad.hash", sha256: "XYZ" }), ent({ id: "bad.kind", kind: "plugin" }), ent({ id: "bad.ver", version: "1.0" }), ent({ id: "big", size: 1 << 30 })), ["pi4"]);
    expect(L.map((e) => `${e.id}@${e.version}`)).toEqual(["x.one@1.10.0"]);
    expect(catList(idx(...Array.from({ length: 500 }, (_, i) => ent({ id: `m${i}` })))).length).toBe(200);
    expect(catList(idx(ent({ description: "d".repeat(5000) })))[0].description.length).toBe(600);
    expect(() => catList({ schema: 2, entries: [] })).toThrow(/unknown catalog format/);
  });
  it("filters by this board, the kind and a search", () => {
    const L = catList(idx(ent(), ent({ board: "pi5" }), ent({ id: "fx.e", kind: "effect", name: "ECHO", short: "ECH", description: "delay" })));
    expect(catShown(L, "", "", "pi4").length).toBe(2);
    expect(catShown(L, "", "", "").length).toBe(3); // the board not known yet
    expect(catShown(L, "effect", "", "pi4").map((e) => e.id)).toEqual(["fx.e"]);
    expect(catShown(L, "", "ech", "pi5").length).toBe(0);
    expect(catShown(L, "", "DELAY", "pi4")[0].id).toBe("fx.e");
    expect(boardOf(4)).toBe("pi4"); expect(boardOf(5)).toBe("pi5"); expect(boardOf(undefined)).toBe("");
  });
  it("links only https: URLs", () => {
    expect(safeUrl("https://a.example/x")).toBe("https://a.example/x");
    for (const u of ["javascript:alert(1)", "http://a.example", "data:text/html,x", "//a.example", "", null]) expect(safeUrl(u)).toBe("");
    const e = catList(idx(ent({ source: "javascript:alert(1)", demo: "http://x.example/a.wav" })))[0];
    expect(e.source).toBe(""); expect(e.demo).toBe("");
  });
});

describe("installed state", () => {
  const e = catList(idx(ent({ version: "1.2.0" })))[0];
  it("install, update, installed, and never a silent downgrade", () => {
    expect(catState(e, []).k).toBe("install");
    expect(catState(e, [{ id: "x.one", ver: "1.0.0" }])).toMatchObject({ k: "update", text: "Update to 1.2.0", from: "1.0.0" });
    expect(catState(e, [{ id: "x.one", ver: "1.2.0" }]).k).toBe("installed");
    const d = catState(e, [{ id: "x.one", ver: "2.0.0" }]);
    expect(d.k).toBe("newer");
    expect(d.text).toMatch(/newer than the catalog/);
  });
  it("compares against the newest installed copy", () => {
    expect(catState(e, [{ id: "x.one", ver: "1.0.0", older: true }, { id: "x.one", ver: "1.2.0" }]).k).toBe("installed");
  });
  it("an update or reinstall replaces the card's existing file, never adds a second one", () => {
    expect(catState(e, [{ id: "x.one", ver: "1.0.0", fit: true, file: "/Plugins/xone.a64.rmp" }])).toMatchObject({ k: "update", file: "xone.a64.rmp" });
    expect(catState(e, []).file).toBeUndefined();
    expect(catState(e, [{ id: "x.one", ver: "1.0.0", file: "../evil name.rmp" }]).file).toBeUndefined();
  });
  it("a copy that does not load is not 'installed': it offers a reinstall over that file", () => {
    expect(catState(e, [{ id: "x.one", ver: "1.2.0", fit: false, file: "xone.a64.rmp" }])).toMatchObject({ k: "install", text: "Reinstall", file: "xone.a64.rmp" });
    expect(catState(e, [{ id: "x.one", ver: "1.2.0", fit: false, file: "dup.rmp" }, { id: "x.one", ver: "1.2.0", fit: true, file: "xone.a64.rmp" }]).k).toBe("installed");
  });
  it("the confirm says community, native code, no sandbox, not tested; and no source when there is none", () => {
    expect(catAsk(e)).toMatch(/community machine, native code, no sandbox, not tested by Sidecar/);
    expect(catAsk(e)).not.toMatch(/No source/);
    expect(catAsk(catList(idx(ent({ source: undefined })))[0])).toMatch(/No source is published/);
  });
});

describe("download and check", () => {
  const e = catList(idx(ent()))[0];
  it("returns the bytes when size and SHA-256 match, from the registry's URL", async () => {
    let url = "";
    const b = await fetchPackage(e, async (u) => { url = String(u); return ok({ arrayBuffer: async () => buf(bytes) }); });
    expect(url).toBe(`${CAT_BASE}machines/x.one/1.0.0/x_one.pi4.rmp`);
    expect(new Uint8Array(b)).toEqual(bytes);
  });
  it("refuses a hash mismatch and a wrong size", async () => {
    const bad = new TextEncoder().encode("tampered bytes!");
    await expect(fetchPackage({ ...e, size: bad.length }, async () => ok({ arrayBuffer: async () => buf(bad) }))).rejects.toThrow(/SHA-256 of the download does not match/);
    await expect(fetchPackage(e, async () => ok({ arrayBuffer: async () => buf(bad) }))).rejects.toThrow(/bytes, the catalog says/);
    await expect(fetchPackage(e, async () => ({ ok: false, status: 404 }) as Response)).rejects.toThrow(/404/);
  });
  it("refuses odd names and paths outside the registry", async () => {
    const f = async () => ok({ arrayBuffer: async () => buf(bytes) });
    await expect(fetchPackage({ ...e, file: "machines/x y.rmp" }, f)).rejects.toThrow(/oddly/);
    await expect(fetchPackage({ ...e, file: "https://evil.example/a.rmp" }, f)).rejects.toThrow(/outside the registry/);
  });
  it("reads the index, and fails (the card says not reachable) without internet", async () => {
    const L = await loadIndex(async () => ok({ text: async () => JSON.stringify(idx(ent())) }));
    expect(L.length).toBe(1);
    await expect(loadIndex(async () => { throw new TypeError("Failed to fetch"); })).rejects.toThrow(/Failed to fetch/);
    await expect(loadIndex(async () => ({ ok: false, status: 500 }) as Response)).rejects.toThrow(/500/);
  });
  it("SHA-256: the pure-JS fallback (plain http has no crypto.subtle) agrees with the platform's", async () => {
    for (const n of [0, 1, 3, 55, 56, 63, 64, 65, 119, 120, 1000, 70000]) {
      const u = new Uint8Array(n).map((_, i) => (i * 7 + 3) & 255);
      expect(sha256Js(u)).toBe(createHash("sha256").update(u).digest("hex"));
    }
    expect(await sha256Hex(buf(bytes))).toBe(sha);
  });
});

describe("previews", () => {
  const pv = (file: string, o: Record<string, unknown> = {}) => ({ name: "01 Acid line", file, bytes: 193743, ...o });
  const F = "machines/x.one/1.0.0/preview/01 Acid line.mp3";
  const get = (previews: unknown) => catList(idx(ent({ previews })))[0].previews;
  it("keeps good items with the URL resolved against the registry and encoded", () => {
    expect(get([pv(F)])).toEqual([{ name: "01 Acid line", url: CAT_BASE + "machines/x.one/1.0.0/preview/01%20Acid%20line.mp3", bytes: 193743 }]);
  });
  it("encodes names with # and +, and enforces the minimum size", () => {
    const f = "machines/x.one/1.0.0/preview/a#b+c (d).mp3";
    expect(get([pv(f, { name: "a#b+c (d)" })])![0].url).toBe(CAT_BASE + "machines/x.one/1.0.0/preview/a%23b%2Bc%20(d).mp3");
    expect(get([pv(F, { bytes: 255 })])).toEqual([]);
    expect(get([pv(F, { bytes: 256 })])!.length).toBe(1);
    expect(get([pv(F, { bytes: 2 << 20 })])!.length).toBe(1);
    expect(get([pv(F, { bytes: (2 << 20) + 1 })])).toEqual([]);
  });
  it("a blank name is dropped, a repeated file is listed once", () => {
    expect(get([pv(F, { name: "" }), pv(F, { name: "   " })])).toEqual([]);
    expect(get([pv(F), pv(F, { name: "again" })])!.length).toBe(1);
  });
  it("an entry without previews is unchanged", () => {
    expect(get(undefined)).toEqual([]);
    expect(get("nope")).toEqual([]);
    expect(catList(idx(ent()))[0].id).toBe("x.one");
  });
  it("caps at 8, clips names, drops bad items but not the entry", () => {
    const many = Array.from({ length: 12 }, (_, i) => pv(`machines/x.one/1.0.0/preview/t${i}.mp3`, { name: `t${i}` }));
    expect(get(many)!.length).toBe(8);
    expect(get([pv(F, { name: "n".repeat(200) })])![0].name.length).toBe(40);
    const L = catList(idx(ent({ previews: [pv(F), null, 7, pv(F, { bytes: 1.5 }), pv(F, { bytes: -1 }), pv(F, { bytes: 3 << 20 }), pv(F, { name: 5 })] })));
    expect(L.length).toBe(1);
    expect(L[0].previews!.length).toBe(1);
  });
  it("refuses bad file names and escapes from the registry", () => {
    const bad = ["../x.mp3", "machines/../../x.mp3", "machines/a/../b.mp3", "machines/./a.mp3", "http://evil.example/machines/a.mp3", "https://evil.example/machines/a.mp3",
      "//evil.example/machines/a.mp3", "javascript:alert(1)", "/machines/a.mp3", "machines\\a.mp3", "machines/a/b.wav", "machines/a/b.MP3", "machines/a/.mp3",
      "machines/a/CON.mp3", "machines/a/a  b.mp3", "machines/a/a b .mp3", "machines/a/a..b.mp3", "machines/a/a%2e%2e.mp3", "machines/a/a?x=1.mp3", "machines/a/a#x.mp3 ", "x.mp3", 5, null,
      "machines/a/" + "n".repeat(41) + ".mp3"];
    expect(get(bad.map((f) => pv(f as string)))).toEqual([]);
  });
});
