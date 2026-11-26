// web-next/src/lib/lag.ts - "Lag now": a tester presses it when the Rytm feels slow, and the Sidecar writes one marker line
// (and a snapshot of its per-second counters) into the black box, so the log shows when. The command is the fixed word
// `lagnow` (pi/engine/lagdiag.h): nothing the page or the user types reaches the log. The Sidecar takes one a second;
// the page keeps the same rule so a held or double-tapped button is one marker, not an error.
export const LAG_CMD = "lagnow";
export const LAG_MIN_MS = 1000;

export type LagResult = "sent" | "too-soon";

export class LagMarker {
  count = 0;
  private last = -Infinity;
  private busy = false;
  constructor(
    private cmd: (c: string) => Promise<void>,
    private now: () => number = () => Date.now(),
  ) {}

  /** "sent" when the Sidecar took it, "too-soon" when one went less than a second ago (nothing sent); a refused command rejects. */
  async press(): Promise<LagResult> {
    const t = this.now();
    if (this.busy || t - this.last < LAG_MIN_MS) return "too-soon";
    this.busy = true;
    try {
      await this.cmd(LAG_CMD);
    } finally {
      this.busy = false;
    }
    this.last = t;
    this.count++;
    return "sent";
  }
}

let shared: { cmd: (c: string) => Promise<void>; marker: LagMarker } | null = null;
/** The one marker of the page: every button shares its second (a press in one and then the other is not a second toast). */
export function sharedMarker(cmd: (c: string) => Promise<void>, now?: () => number): LagMarker {
  if (!shared || shared.cmd !== cmd) shared = { cmd, marker: new LagMarker(cmd, now) };
  return shared.marker;
}

/** Why the button cannot be pressed, in a word for its label: "" when it can. */
export function lagBlocked(state: string, paired: boolean): "" | "offline" | "pair first" {
  return state !== "open" ? "offline" : paired ? "" : "pair first";
}
