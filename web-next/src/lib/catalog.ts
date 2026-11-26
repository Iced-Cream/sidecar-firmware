// web-next/src/lib/catalog.ts - the community catalog (Voices › User machines › Catalog): the registry's index.json
// (https://trigcondition.github.io/sidecar-machines/, its docs/index-format.md), read and checked by THIS BROWSER: the Pi
// never goes to the internet. A package is downloaded here, its size and SHA-256 checked (crypto.subtle where the page is
// a secure context; the Pi's page is plain http, so a pure-JS SHA-256 otherwise), then sent through the ordinary upload
// (upload.ts, /fs/put into /Plugins), where the Pi's loader checks it again. Everything from the index is data: the card
// renders it as text, and only https: URLs become links. Framework-free (catalog.test.ts).
export const CAT_BASE = "https://trigcondition.github.io/sidecar-machines/"; // the one registry: no settings field
export const CAT_MAX_ENTRIES = 200;
export const CAT_MAX_FILE = 96 + (1 << 20) + (64 << 10); // the Pi's loader limit (core/user/um.cpp kPkgMax); the upload check allows 2 MB but the loader would refuse more than this
const CAT_MAX_INDEX = 4 << 20;
// the registry's preview rules (its tools/previews.py): at most 8 files of 256 B to 2 MiB, "<name>.mp3" with a name of up to 40 characters
export const CAT_MAX_PREVIEWS = 8;
const PV_NAME = /^[A-Za-z0-9](?:(?:[A-Za-z0-9_.()+#-]| (?! ))*[A-Za-z0-9_()+#-])?$/;
const PV_DIR = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;
const PV_RESERVED = /^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$/;
const CAT_FILE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.rmp$/;

export type CatBoard = "pi4" | "pi5";
export interface CatEntry {
  id: string; kind: "machine" | "effect"; version: string; v: [number, number, number]; board: string;
  name: string; short: string; author: string; licence: string; sha256: string; file: string; size: number;
  description: string; source: string; demo: string; previews: CatPreview[];
}
/** One audio preview of an entry (the registry's optional `previews`): a display name and an https: URL under CAT_BASE. */
export interface CatPreview { name: string; url: string; bytes: number }
export interface CatState { k: "install" | "update" | "installed" | "newer"; text: string; from?: string; file?: string }

const clip = (s: unknown, n: number) => String(s == null ? "" : s).slice(0, n);
/** an https: URL or "" (no javascript:, data:, http:) */
export function safeUrl(u: unknown): string {
  try { const x = new URL(String(u)); return x.protocol === "https:" ? x.href : ""; } catch { return ""; }
}
export function parseVer(v: unknown): [number, number, number] | null {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(String(v));
  return m ? [+m[1], +m[2], +m[3]] : null;
}
export function cmpVer(a: number[], b: number[]): number {
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
/** The https: URL of a preview file under CAT_BASE ("machines/<dirs>/<name>.mp3", the registry's file name rule), or "". */
export function previewUrl(f: unknown): string {
  if (typeof f !== "string" || !f.startsWith("machines/") || !f.endsWith(".mp3")) return "";
  const seg = f.split("/"), disp = (seg.pop() as string).slice(0, -4);
  if (!disp || disp.length > 40 || !PV_NAME.test(disp) || disp.includes("..") || PV_RESERVED.test(disp.split(".")[0].trimEnd().toUpperCase())) return "";
  if (!seg.every((d) => PV_DIR.test(d))) return "";
  try {
    const u = new URL([...seg, disp + ".mp3"].map(encodeURIComponent).join("/"), CAT_BASE);
    return u.protocol === "https:" && u.href.startsWith(CAT_BASE) ? u.href : "";
  } catch { return ""; }
}
/** The index's previews: at most 8, bad items dropped (the entry stays). */
function catPreviews(p: unknown): CatPreview[] {
  const out: CatPreview[] = [], seen = new Set<string>();
  if (!Array.isArray(p)) return out;
  for (const x of p) {
    if (out.length >= CAT_MAX_PREVIEWS) break;
    if (!x || typeof x !== "object" || typeof x.name !== "string" || !Number.isInteger(x.bytes) || x.bytes < 256 || x.bytes > 2 << 20) continue;
    const url = previewUrl(x.file), name = clip(x.name, 40).trim();
    if (!url || !name || seen.has(url)) continue; // a blank name or a repeated file is dropped
    seen.add(url);
    out.push({ name, url, bytes: x.bytes });
  }
  return out;
}
/** The index's entries, cleaned and capped: the newest version of each id and board; bad entries dropped. */
export function catList(idx: any, boards: string[] = ["pi4", "pi5"]): CatEntry[] {
  if (!idx || idx.schema !== 1 || !Array.isArray(idx.entries)) throw new Error("unknown catalog format");
  const best = new Map<string, CatEntry>();
  for (const x of idx.entries) {
    if (!x || typeof x !== "object" || typeof x.id !== "string" || typeof x.board !== "string" || !boards.includes(x.board)) continue;
    const v = parseVer(x.version);
    if (!v || (x.kind !== "machine" && x.kind !== "effect") || !/^[0-9a-f]{64}$/.test(String(x.sha256)) || typeof x.file !== "string") continue;
    if (!Number.isInteger(x.size) || x.size < 1 || x.size > CAT_MAX_FILE) continue;
    const e: CatEntry = { id: clip(x.id, 40), kind: x.kind, version: String(x.version), v, board: x.board, name: clip(x.name, 10), short: clip(x.short, 4),
      author: clip(x.author, 40), licence: clip(x.licence, 60), sha256: x.sha256, file: x.file, size: x.size, description: clip(x.description, 600),
      source: safeUrl(x.source), demo: safeUrl(x.demo), previews: catPreviews(x.previews) };
    const k = `${e.id}/${e.board}`, o = best.get(k);
    if (!o || cmpVer(e.v, o.v) > 0) best.set(k, e);
  }
  return [...best.values()].sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id)).slice(0, CAT_MAX_ENTRIES);
}
/** "pi4" / "pi5" from the status's usbMode.board (4, 5); "" while it is not known. */
export const boardOf = (n: unknown): CatBoard | "" => (n === 5 ? "pi5" : n === 4 ? "pi4" : "");
export function catShown(list: CatEntry[], kind: string, q: string, board: string): CatEntry[] {
  const w = q.trim().toLowerCase();
  return list.filter((e) => (!board || e.board === board) && (!kind || e.kind === kind)
    && (!w || [e.name, e.short, e.id, e.author, e.description].some((t) => t.toLowerCase().includes(w))));
}
/** Installed (the /um "plugin" rows) against an entry: install, update, installed, or newer (never a downgrade).
 * Only loadable rows (fit) count as installed; the newest non-older one wins. `file` names the card's existing
 * package for this id, so an update or a reinstall replaces that file instead of adding a second one. */
export function catState(e: CatEntry, installed: { [k: string]: any }[]): CatState {
  const mine = installed.filter((x) => x.id === e.id);
  const base = (f: unknown) => (typeof f === "string" ? f.split("/").pop() ?? "" : "");
  const okName = (f: string) => (/^[A-Za-z0-9._-]{1,64}\.rmp$/i.test(f) ? f : undefined);
  const good = mine.filter((x) => x.fit !== false && !x.older)
    .sort((a, b) => cmpVer(parseVer(b.ver) ?? [0, 0, 0], parseVer(a.ver) ?? [0, 0, 0]));
  const p = good[0];
  if (!p) {
    const broken = mine.find((x) => x.fit === false);
    return broken ? { k: "install", text: "Reinstall", file: okName(base(broken.file)) } : { k: "install", text: "Install" };
  }
  const file = okName(base(p.file));
  const iv = parseVer(p.ver);
  if (!iv) return { k: "installed", text: "Installed" };
  const c = cmpVer(e.v, iv);
  return c > 0 ? { k: "update", text: `Update to ${e.version}`, from: String(p.ver), file } : c === 0 ? { k: "installed", text: "Installed" }
    : { k: "newer", text: `Installed ${String(p.ver)} (newer than the catalog)` };
}
/** The trust text of the one confirm. */
export const catAsk = (e: CatEntry) => `${e.name || e.id} ${e.version} by ${e.author}: a community machine, native code, no sandbox, not tested by Sidecar.${e.source ? "" : " No source is published for it."}`
  + " It is downloaded in this browser, checked against its SHA-256, then sent to the Pi.";
export const catFileName = (e: CatEntry) => e.file.split("/").pop() ?? "";

/** FIPS 180-4 SHA-256 (hex), for a page served over plain http, which has no crypto.subtle. */
export function sha256Js(m: Uint8Array): string {
  const K = new Uint32Array(64), H = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  for (let i = 0, n = 2; i < 64; n++) {
    let pr = true;
    for (let d = 2; d * d <= n; d++) if (n % d === 0) { pr = false; break; }
    if (pr) K[i++] = Math.floor((Math.cbrt(n) % 1) * 4294967296);
  }
  const L = m.length, T = new Uint8Array(((L + 9 + 63) >> 6) << 6), dv = new DataView(T.buffer), W = new Uint32Array(64);
  T.set(m); T[L] = 0x80;
  dv.setUint32(T.length - 8, Math.floor(L / 536870912)); dv.setUint32(T.length - 4, (L << 3) >>> 0);
  const rr = (x: number, n: number) => (x >>> n) | (x << (32 - n));
  for (let o = 0; o < T.length; o += 64) {
    for (let i = 0; i < 16; i++) W[i] = dv.getUint32(o + 4 * i);
    for (let i = 16; i < 64; i++) {
      const a = W[i - 15], b = W[i - 2];
      W[i] = (W[i - 16] + (rr(a, 7) ^ rr(a, 18) ^ (a >>> 3)) + W[i - 7] + (rr(b, 17) ^ rr(b, 19) ^ (b >>> 10))) >>> 0;
    }
    let [a, b, c, d, e, f, g, h] = H;
    for (let i = 0; i < 64; i++) {
      const t1 = (h + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + W[i]) >>> 0;
      const t2 = ((rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
      h = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    [a, b, c, d, e, f, g, h].forEach((v, i) => { H[i] = (H[i] + v) >>> 0; });
  }
  return [...H].map((v) => v.toString(16).padStart(8, "0")).join("");
}
export async function sha256Hex(buf: ArrayBuffer): Promise<string> {
  const s = typeof crypto !== "undefined" ? crypto.subtle : undefined;
  if (s) {
    try { return [...new Uint8Array(await s.digest("SHA-256", buf))].map((b) => b.toString(16).padStart(2, "0")).join(""); } catch { /* fall back */ }
  }
  return sha256Js(new Uint8Array(buf));
}

/** The registry's list for these boards; throws when it cannot be read (no internet: the card says "not reachable"). */
export async function loadIndex(fetchFn: typeof fetch = (...a) => fetch(...a), boards?: string[]): Promise<CatEntry[]> {
  const r = await fetchFn(CAT_BASE + "index.json", { cache: "no-store", signal: AbortSignal.timeout(8000) });
  if (!r.ok) throw new Error(`answered ${r.status}`);
  const t = await r.text();
  if (t.length > CAT_MAX_INDEX) throw new Error("too large");
  return catList(JSON.parse(t), boards);
}
/** Download an entry's .rmp and check it (size, SHA-256): the bytes, or a thrown refusal (nothing is sent then). */
export async function fetchPackage(e: CatEntry, fetchFn: typeof fetch = (...a) => fetch(...a)): Promise<ArrayBuffer> {
  if (!CAT_FILE.test(catFileName(e))) throw new Error("the catalog names this file oddly: refused");
  const url = new URL(e.file, CAT_BASE).href;
  if (!url.startsWith(CAT_BASE)) throw new Error("the catalog points outside the registry: refused");
  const r = await fetchFn(url, { cache: "no-store", signal: AbortSignal.timeout(60000) });
  if (!r.ok) throw new Error(`download failed: ${r.status}`);
  const buf = await r.arrayBuffer();
  if (buf.byteLength !== e.size) throw new Error(`refused: the download is ${buf.byteLength} bytes, the catalog says ${e.size}`);
  const h = await sha256Hex(buf);
  if (h !== e.sha256) throw new Error(`refused: the SHA-256 of the download does not match the catalog (${h.slice(0, 12)} instead of ${e.sha256.slice(0, 12)}); nothing was sent to the Pi`);
  return buf;
}
