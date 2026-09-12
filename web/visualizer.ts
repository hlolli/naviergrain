/** Read-only observation layout v1. This is not an external-field provider. */
export const viewSize = 3344;
export const fieldOffset = 16;
export const grainOffset = fieldOffset + 16 * 16 * 8;
export const emitterOffset = grainOffset + 128 * 8;
type Point = [number, number];
export type Segment = [Point, Point];

/** Split sampled unwrapped paths at torus boundaries, including corner wraps.
 * Very large jumps are omitted instead of drawing ambiguous/expensive trails. */
export function periodicSegments(a: Point, b: Point): Segment[] {
  if (![...a, ...b].every(Number.isFinite)) return [];
  const dx = b[0] - a[0], dy = b[1] - a[1];
  if (Math.abs(dx) + Math.abs(dy) > 8) return [];
  // Translate near zero before enumerating boundaries. At large unwrapped
  // coordinates adding one may no longer change a double.
  a = [a[0] - Math.floor(a[0]), a[1] - Math.floor(a[1])];
  b = [a[0] + dx, a[1] + dy];
  const times = new Set([0, 1]);
  for (const axis of [0, 1] as const) {
    const delta = b[axis] - a[axis];
    if (!delta) continue;
    for (let boundary = Math.floor(Math.min(a[axis], b[axis])) + 1;
      boundary < Math.max(a[axis], b[axis]); boundary++) {
      const t = (boundary - a[axis]) / delta;
      if (t > 0 && t < 1) times.add(t);
    }
  }
  const sorted = [...times].sort((x, y) => x - y);
  return sorted.slice(1).map((end, i) => {
    const start = sorted[i]!, midpoint = (start + end) / 2;
    const ox = Math.floor(a[0] + midpoint * dx), oy = Math.floor(a[1] + midpoint * dy);
    return [[a[0] + start * dx - ox, a[1] + start * dy - oy],
      [a[0] + end * dx - ox, a[1] + end * dy - oy]];
  });
}
export function validateView(v: Float64Array): void {
  if (v.length !== viewSize || v[0] !== 1 || v[1] !== viewSize ||
      v[7] !== 16 || !v.every(Number.isFinite) ||
      ![v[8], v[9]].every(n => Number.isInteger(n) && n! >= 0 && n! <= 128))
    throw new Error("Invalid visual snapshot");
}
export class Visualizer {
  private readonly context: CanvasRenderingContext2D;
  private paths = new Map<string, Point[]>();
  private epoch = "";
  private lastFrame = "";
  private snapshot: Float64Array | undefined;
  constructor(private readonly canvas: HTMLCanvasElement, private readonly detail: HTMLElement) {
    const context = canvas.getContext("2d");
    if (!context) throw new Error("Canvas drawing is unavailable");
    this.context = context;
  }
  clear(): void {
    this.paths.clear(); this.epoch = ""; this.lastFrame = ""; this.snapshot = undefined;
    this.context.clearRect(0, 0, this.canvas.width, this.canvas.height);
    this.detail.textContent = "Waiting for a field snapshot.";
  }
  accept(v: Float64Array): void {
    validateView(v);
    const epoch = v[3] + ":" + v[2], frame = v[5] + ":" + v[4];
    if (epoch !== this.epoch || !v[11]) { this.paths.clear(); this.lastFrame = ""; }
    this.epoch = epoch; this.snapshot = v;
    if (frame === this.lastFrame) return;
    this.lastFrame = frame;
    const alive = new Set<string>();
    for (let i = 0; i < v[8]!; i++) {
      const offset = grainOffset + i * 8, id = v[offset + 1] + ":" + v[offset];
      alive.add(id);
      const points = this.paths.get(id) ?? [];
      points.push([v[offset + 4]!, v[offset + 5]!]);
      if (points.length > 12) points.shift();
      this.paths.set(id, points);
    }
    for (const id of this.paths.keys()) if (!alive.has(id)) this.paths.delete(id);
  }
  draw(trails: number, mode: string): void {
    const v = this.snapshot;
    if (!v) return;
    const ctx = this.context, size = this.canvas.width;
    ctx.fillStyle = "#101e26"; ctx.fillRect(0, 0, size, size);
    let maximum = .001;
    for (let cell = 0; cell < 256; cell++) {
      const k = fieldOffset + cell * 8;
      maximum = Math.max(maximum, mode === "velocity" ? Math.hypot(v[k]!, v[k + 1]!) :
        Math.abs(v[k + (mode === "vorticity" ? 2 : 3)]!));
    }
    for (let cell = 0; cell < 256; cell++) {
      const k = fieldOffset + cell * 8, x = (cell % 16 + .5) * size / 16;
      const y = (Math.floor(cell / 16) + .5) * size / 16;
      if (mode !== "velocity") {
        const value = v[k + (mode === "vorticity" ? 2 : 3)]! / maximum;
        ctx.fillStyle = value < 0 ? `rgba(91,156,255,${Math.abs(value) * .8})` : `rgba(220,96,164,${value * .8})`;
        ctx.fillRect(x - size / 32, y - size / 32, size / 16, size / 16);
      } else {
        const dx = v[k]! / maximum * size / 20, dy = v[k + 1]! / maximum * size / 20;
        ctx.strokeStyle = "#71c6b1"; ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(x - dx / 2, y - dy / 2); ctx.lineTo(x + dx / 2, y + dy / 2); ctx.stroke();
        if (Math.hypot(dx, dy) > .2) {
          const angle = Math.atan2(dy, dx);
          ctx.beginPath(); ctx.moveTo(x + dx / 2 - 3 * Math.cos(angle - .5), y + dy / 2 - 3 * Math.sin(angle - .5));
          ctx.lineTo(x + dx / 2, y + dy / 2);
          ctx.lineTo(x + dx / 2 - 3 * Math.cos(angle + .5), y + dy / 2 - 3 * Math.sin(angle + .5)); ctx.stroke();
        }
      }
    }
    ctx.fillStyle = "#8e9aa4";
    for (let i = 0; i < v[9]!; i++) {
      ctx.beginPath(); ctx.arc(v[emitterOffset + 2 * i]! * size, v[emitterOffset + 2 * i + 1]! * size, 1.5, 0, 2 * Math.PI); ctx.fill();
    }
    ctx.strokeStyle = "#ffc77a"; ctx.lineWidth = 1.5;
    for (const points of [...this.paths.values()].slice(0, Math.max(0, Math.min(128, trails)))) {
      for (let i = 1; i < points.length; i++) {
        for (const [a, b] of periodicSegments(points[i - 1]!, points[i]!)) {
          ctx.beginPath(); ctx.moveTo(a[0] * size, a[1] * size); ctx.lineTo(b[0] * size, b[1] * size); ctx.stroke();
        }
      }
    }
    for (let i = 0; i < v[8]!; i++) {
      const k = grainOffset + i * 8;
      ctx.fillStyle = `rgba(255,201,121,${1 - .6 * v[k + 6]!})`;
      ctx.beginPath(); ctx.arc(v[k + 2]! * size, v[k + 3]! * size, 3, 0, 2 * Math.PI); ctx.fill();
    }
    const k = fieldOffset + (8 * 16 + 8) * 8;
    const shear = (v[k + 5]! + v[k + 6]!) / 2;
    const spin = (v[k + 5]! - v[k + 6]!) / 2;
    const fmt = (n: number) => n.toFixed(3);
    this.detail.textContent = `CPU · simulation ${v[6]!.toFixed(3)} s · ${v[8]} grains / ${v[9]} emitters shown (cap 128 each). ` +
      (v[11] ? `At (0.531, 0.531), strain S = [${fmt(v[k + 4]!)}, ${fmt(shear)}; ${fmt(shear)}, ${fmt(v[k + 7]!)}], rotation W = [0, ${fmt(spin)}; ${fmt(-spin)}, 0]. ` : "Field not yet published. ") +
      `Field display scale: ${maximum.toFixed(3)} (auto).`;
  }
}
