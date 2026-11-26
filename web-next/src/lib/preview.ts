// web-next/src/lib/preview.ts - the catalog's audio previews: ONE audio element shared by every track, so only one track
// plays at a time. preload "none", no autoplay: nothing is fetched until the user presses play, and the audio streams
// from the registry's site to this browser (the Pi is never contacted for it). Framework-free (preview.test.ts).
// A track is named by a caller-chosen `key` (the card uses entry + URL: the pi4 and pi5 rows of one id share URLs) and
// has the `url` to stream.
export interface AudioLike {
  preload: string; autoplay: boolean; src: string; currentTime: number; readonly duration: number; readonly paused: boolean; readonly ended: boolean;
  addEventListener(t: string, f: () => void): void;
  play(): Promise<void> | void; pause(): void; removeAttribute(n: string): void; load(): void;
}
/** key: the loaded track; loading: pressed but no audio yet; error/errKey: the last failure and the track it belongs to */
export interface PreviewState { key: string; url: string; playing: boolean; loading: boolean; pos: number; time: string; error: string; errKey: string }

export const clock = (s: number) => { const n = Number.isFinite(s) ? Math.floor(s) : 0; return `${Math.floor(n / 60)}:${n % 60 < 10 ? "0" : ""}${n % 60}`; };
export const idleState = (): PreviewState => ({ key: "", url: "", playing: false, loading: false, pos: 0, time: "0:00", error: "", errKey: "" });

export class PreviewPlayer {
  state: PreviewState = idleState();
  private a: AudioLike | null = null;
  /** `onChange` gets the new state after every change (the card copies it into a ref). */
  constructor(private make: () => AudioLike = () => new Audio(), private onChange: (s: PreviewState) => void = () => {}) {}
  private set(s: Partial<PreviewState>) { this.state = { ...this.state, ...s }; this.onChange(this.state); }
  private clear(error = "") { const k = this.state.key; this.state = idleState(); this.set(error ? { error, errKey: k } : {}); }
  private audio(): AudioLike {
    if (this.a) return this.a;
    const a = (this.a = this.make());
    a.preload = "none";
    a.autoplay = false;
    a.addEventListener("timeupdate", () => {
      if (!this.state.url || !a.duration) return;
      this.set({ pos: Math.round((1000 * a.currentTime) / a.duration), time: `${clock(a.currentTime)} / ${clock(a.duration)}` });
    });
    a.addEventListener("play", () => { if (this.state.url) this.set({ playing: true }); });
    a.addEventListener("playing", () => { if (this.state.url) this.set({ loading: false }); });
    a.addEventListener("waiting", () => { if (this.state.url) this.set({ loading: true }); });
    a.addEventListener("pause", () => { if (this.state.url && !a.ended) this.set({ playing: false, loading: false }); });
    a.addEventListener("ended", () => this.clear());
    a.addEventListener("error", () => { if (this.state.url) this.clear("could not play"); });
    return a;
  }
  isLoaded(key: string) { return this.state.key === key; }
  isPlaying(key: string) { return this.state.key === key && this.state.playing; }
  /** Play, pause or resume this track; another track that is loaded is stopped first. */
  toggle(key: string, url: string) {
    const a = this.audio();
    if (this.state.key === key) {
      if (a.paused) void a.play(); else a.pause();
      return;
    }
    this.stop();
    this.set({ key, url, loading: true, error: "", errKey: "" });
    a.src = url;
    // a refusal (autoplay policy, no network) arrives as the error event or this rejection; an AbortError is only a
    // Pause pressed while the first buffering was under way
    void Promise.resolve(a.play()).catch((e: { name?: string }) => {
      if (e?.name !== "AbortError" && this.state.key === key) this.clear("could not play");
    });
  }
  /** Move the loaded track to `pos` (0..1000); nothing moves for a track that is not the loaded one. */
  seek(key: string, pos: number) {
    const a = this.a;
    if (a && this.state.key === key && a.duration) a.currentTime = (a.duration * pos) / 1000;
  }
  /** Silence and unload (the list closed, the filters changed, the page left). */
  stop() {
    const a = this.a;
    if (!a) return;
    const had = this.state.key || this.state.error;
    this.state = { ...this.state, key: "", url: "" }; // so the pause event below changes nothing else
    a.pause();
    a.removeAttribute("src");
    a.load();
    if (had) this.clear();
  }
}
