// web-next/src/lib/preview.test.ts
import { describe, expect, it } from "vitest";
import { PreviewPlayer, clock, type AudioLike } from "./preview";

class FakeAudio implements AudioLike {
  preload = ""; autoplay = true; src = ""; currentTime = 0; duration = NaN; paused = true; ended = false;
  plays: string[] = [];
  rejectWith: Error | null = null; // the next play() returns a promise rejected with this
  private h: { [k: string]: (() => void)[] } = {};
  addEventListener(t: string, f: () => void) { (this.h[t] ??= []).push(f); }
  emit(t: string) { (this.h[t] ?? []).forEach((f) => f()); }
  play() {
    this.paused = false; this.plays.push(this.src); this.emit("play");
    const r = this.rejectWith; this.rejectWith = null;
    return r ? Promise.reject(r) : Promise.resolve();
  }
  pause() { if (!this.paused) { this.paused = true; this.emit("pause"); } }
  removeAttribute() { this.src = ""; }
  load() { /* nothing */ }
}
const mk = () => { const a = new FakeAudio(); const p = new PreviewPlayer(() => a); return { a, p }; };

describe("preview player", () => {
  it("makes no audio element, and fetches nothing, until a track is played; preload none, no autoplay", () => {
    let made = 0;
    const p = new PreviewPlayer(() => { made++; return new FakeAudio(); });
    expect(made).toBe(0);
    p.toggle("k1", "u1");
    expect(made).toBe(1);
    const a = new FakeAudio(); new PreviewPlayer(() => a).toggle("k", "u"); expect(a.preload).toBe("none"); expect(a.autoplay).toBe(false);
  });
  it("one track plays at a time: starting another replaces the first", () => {
    const { a, p } = mk();
    p.toggle("k1", "u1");
    expect(p.state.url).toBe("u1"); expect(p.state.playing).toBe(true);
    p.toggle("k2", "u2");
    expect(p.state.url).toBe("u2"); expect(a.src).toBe("u2"); expect(a.plays).toEqual(["u1", "u2"]);
    expect(p.isPlaying("k1")).toBe(false); expect(p.isPlaying("k2")).toBe(true);
  });
  it("the same track pauses and resumes", () => {
    const { a, p } = mk();
    p.toggle("k1", "u1"); p.toggle("k1", "u1");
    expect(p.state.playing).toBe(false); expect(p.state.url).toBe("u1");
    p.toggle("k1", "u1");
    expect(p.state.playing).toBe(true); expect(a.plays).toEqual(["u1", "u1"]);
  });
  it("progress follows the audio, a seek moves it, and the end resets the track", () => {
    const { a, p } = mk();
    p.toggle("k1", "u1"); a.duration = 100; a.currentTime = 25; a.emit("timeupdate");
    expect(p.state.pos).toBe(250); expect(p.state.time).toBe("0:25 / 1:40");
    p.seek("k1", 500); expect(a.currentTime).toBe(50);
    p.seek("k2", 900); expect(a.currentTime).toBe(50); // not the loaded track: nothing moves
    a.ended = true; a.emit("ended");
    expect(p.state.url).toBe(""); expect(p.state.playing).toBe(false);
  });
  it("stop (list closed, filters changed, page left) silences and clears", () => {
    const { a, p } = mk();
    p.toggle("k1", "u1"); p.stop();
    expect(a.paused).toBe(true); expect(a.src).toBe(""); expect(p.state.url).toBe("");
  });
  it("a playback error is shown and the track is released", () => {
    const { a, p } = mk();
    p.toggle("k1", "u1"); a.emit("error");
    expect(p.state.error).toBe("could not play"); expect(p.state.playing).toBe(false); expect(p.state.url).toBe("");
  });
  it("an AbortError from play() (Pause during the first buffering) is not an error", async () => {
    const { a, p } = mk();
    a.rejectWith = Object.assign(new Error("interrupted by a call to pause()"), { name: "AbortError" });
    p.toggle("k1", "u1"); a.pause();
    await Promise.resolve(); await Promise.resolve();
    expect(p.state.error).toBe(""); expect(p.state.url).toBe("u1"); expect(p.state.playing).toBe(false);
  });
  it("another refusal of play() is shown on that track only, and a stale one touches nothing", async () => {
    const { a, p } = mk();
    a.rejectWith = new Error("NotAllowedError"); p.toggle("k1", "u1");
    await Promise.resolve(); await Promise.resolve();
    expect(p.state.error).toBe("could not play"); expect(p.state.errKey).toBe("k1"); expect(p.state.url).toBe("");
    a.rejectWith = new Error("late"); p.toggle("k2", "u2"); p.toggle("k3", "u3"); // k2's rejection arrives after k3 started
    await Promise.resolve(); await Promise.resolve();
    expect(p.state.url).toBe("u3"); expect(p.state.error).toBe(""); expect(p.state.errKey).toBe("");
  });
  it("the same URL under two keys (pi4 and pi5 rows) is two tracks", () => {
    const { p } = mk();
    p.toggle("a|u", "u");
    expect(p.isPlaying("a|u")).toBe(true); expect(p.isPlaying("b|u")).toBe(false);
    p.seek("b|u", 500); // not loaded under this key
    p.toggle("b|u", "u");
    expect(p.isPlaying("a|u")).toBe(false); expect(p.isPlaying("b|u")).toBe(true);
  });
  it("loading shows from the press until audio flows", () => {
    const { a, p } = mk();
    p.toggle("k1", "u1");
    expect(p.state.loading).toBe(true);
    a.emit("playing"); expect(p.state.loading).toBe(false);
    a.emit("waiting"); expect(p.state.loading).toBe(true);
    a.pause(); expect(p.state.loading).toBe(false);
  });
  it("clock", () => { expect(clock(0)).toBe("0:00"); expect(clock(65.9)).toBe("1:05"); expect(clock(NaN)).toBe("0:00"); });
});
