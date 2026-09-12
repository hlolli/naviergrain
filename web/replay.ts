/** Bounded OFFLINE capture. The audio-owned C acceptance clocks are authoritative.
 * Replay reproduces ingress timing as well as acceptance; it never runs a solver.
 * No real-time allocation or cross-build reproducibility claim. */
import {OfflineEngine, browserConfig, blockSize, sampleRate, maxRenderSeconds,
  maxSourceFrames, type CsoundApi, type Settings} from "./engine";
import {SharedRing, commandBytes, packetBytes, type BridgeSetup} from "./bridge";

const maxBytes = 64 * 1024 * 1024;
const maxBlocks = sampleRate * maxRenderSeconds / blockSize;
type Acceptance = {frame: string; epoch: string; sequence: string};
type Block = {frame: number; packet?: string; recovery?: boolean; reset?: boolean;
  resume?: boolean; command?: string; accepted?: Acceptance};
interface Tape {
  version: 1; schema: 1; packetVersion: 1; mailboxVersion: 2; build: string;
  frames: number; outputFrames: number; grid: number; config: number[]; controls: number[];
  sourceRate: number; source: string; sourceSha256: string; pcmSha256: string; blocks: Block[];
}
export async function sha256(bytes: Uint8Array): Promise<string> {
  return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes.slice().buffer)),
    value => value.toString(16).padStart(2, "0")).join("");
}
function base64(bytes: Uint8Array): string {
  let text = "";
  for (let i = 0; i < bytes.length; i += 8192)
    text += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(text);
}
function unbase64(text: string, limit: number): Uint8Array {
  if (typeof text !== "string" || text.length > Math.ceil(limit / 3) * 4 ||
      text.length % 4 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(text))
    throw new Error("Invalid or oversized replay bytes");
  const decoded = Uint8Array.from(atob(text), letter => letter.charCodeAt(0));
  if (decoded.length > limit || base64(decoded) !== text) throw new Error("Invalid replay encoding");
  return decoded;
}
async function hashPCM(chunks: Float64Array[]): Promise<string> {
  const samples = new Float64Array(chunks.reduce((sum, chunk) => sum + chunk.length, 0));
  let offset = 0;
  for (const chunk of chunks) { samples.set(chunk, offset); offset += chunk.length; }
  return sha256(sourceBytes(samples));
}
function sourceBytes(source: Float64Array): Uint8Array {
  const bytes = new Uint8Array(source.length * 8), view = new DataView(bytes.buffer);
  source.forEach((sample, i) => view.setFloat64(i * 8, sample, true));
  return bytes;
}
function setup(grid: number): BridgeSetup {
  return {grid, commands: new SharedRing(commandBytes).buffer,
    fields: new SharedRing(packetBytes(grid)).buffer};
}
function key(acceptance: Acceptance): string { return acceptance.epoch + ":" + acceptance.sequence; }
function packetKey(packet: Uint8Array): string {
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  return view.getBigUint64(24, true) + ":" + view.getBigUint64(32, true);
}
function checkBuild(build: string): void {
  if (!/^[0-9a-f]{64}$/.test(build)) throw new Error("Replay needs the exact build manifest SHA-256");
}
/** Own the engine from its prepared state until capture ends. Use this wrapper
 * for ALL rendering/reset/resume calls; view()/stats() on engine are read-only. */
export class ReplayCapture {
  readonly engine: OfflineEngine;
  private readonly source: Uint8Array;
  private readonly settings: Settings;
  private readonly blocks: Block[] = [];
  private readonly pcm: Float64Array[] = [];
  private current?: Block;
  private pending: Pick<Block, "reset" | "resume"> = {};
  private frames = 0;
  private bytes: number;
  private previous = "0:0";
  private closed = false;
  constructor(api: CsoundApi, settings: Settings, includes: string,
      private readonly queues: BridgeSetup, private readonly build: string, observed = false) {
    checkBuild(build);
    this.settings = {...settings, source: settings.source.slice(), controls: [...settings.controls]};
    this.source = sourceBytes(this.settings.source);
    this.bytes = Math.ceil(this.source.length / 3) * 4 + 4096;
    this.engine = new OfflineEngine(api, this.settings, includes, observed, queues);
    this.engine.bridge!.onIngress = (packet, recovery) => {
      if (!this.current) throw new Error("Recording bypassed its render boundary");
      if (packet) this.current.packet = base64(packet);
      if (recovery) this.current.recovery = true;
    };
    this.engine.bridge!.onCommand = command => {
      if (!this.current) throw new Error("Recording bypassed its command boundary");
      this.current.command = base64(command);
    };
  }
  reset(high: boolean): void {
    if (this.closed) throw new Error("Recording is closed");
    this.pending.reset = high;
  }
  resume(): void {
    if (this.closed) throw new Error("Recording is closed");
    this.pending.resume = true;
  }
  render(observe = false): Float64Array {
    if (this.closed || this.frames >= maxBlocks * blockSize) throw new Error("Recording is closed or full");
    this.current = {frame: this.frames, ...this.pending}; this.pending = {};
    try {
      if (this.current.reset !== undefined) this.engine.reset(this.current.reset);
      if (this.current.resume) this.engine.resumeExternal();
      const pcm = this.engine.render(1, observe);
      const accepted = this.engine.bridge!.acceptance();
      if (key(accepted) !== this.previous) {
        // Fixed browser profile: 240-Hz particles / 48 kHz, at most one
        // acceptance in a 64-sample block. Refuse incompatible clock telemetry.
        const frame = BigInt(accepted.frame);
        if (frame < BigInt(this.frames) || frame >= BigInt(this.frames + blockSize))
          throw new Error("Acceptance is outside its recorded audio block");
        this.current.accepted = accepted; this.previous = key(accepted);
      }
      if (Object.keys(this.current).length > 1) {
        const size = JSON.stringify(this.current).length + 1;
        if (this.bytes + size > maxBytes - 4096) throw new Error("Replay capture exceeds 64 MiB");
        this.bytes += size; this.blocks.push(this.current);
      }
      this.pcm.push(pcm.slice());
      this.frames += blockSize;
      return pcm;
    } catch (error) { this.closed = true; throw error; }
    finally { this.current = undefined; }
  }
  async finish(outputFrames = this.frames): Promise<Uint8Array> {
    if (this.closed || !this.frames) throw new Error("No complete recording");
    if (!Number.isSafeInteger(outputFrames) || outputFrames > this.frames || outputFrames <= this.frames - blockSize)
      throw new Error("Invalid final replay sample count");
    this.closed = true;
    const tape: Tape = {version: 1, schema: 1, packetVersion: 1, mailboxVersion: 2,
      build: this.build, frames: this.frames, outputFrames, grid: this.queues.grid,
      config: browserConfig(true, this.queues.grid), controls: this.settings.controls,
      sourceRate: this.settings.sourceRate, source: base64(this.source),
      sourceSha256: await sha256(this.source), pcmSha256: await hashPCM(this.pcm), blocks: this.blocks};
    const bytes = new TextEncoder().encode(JSON.stringify(tape));
    if (bytes.length > maxBytes) throw new Error("Replay capture exceeds 64 MiB");
    return bytes;
  }
  destroy(): void { this.closed = true; this.engine.destroy(); }
}

function object(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}
function uint64(value: unknown): value is string {
  return typeof value === "string" && /^(0|[1-9][0-9]{0,19})$/.test(value) && BigInt(value) <= 0xffffffffffffffffn;
}
async function decode(bytes: Uint8Array, build: string): Promise<{tape: Tape; settings: Settings}> {
  checkBuild(build);
  if (bytes.length > maxBytes) throw new Error("Replay exceeds 64 MiB");
  const value: unknown = JSON.parse(new TextDecoder("utf-8", {fatal: true}).decode(bytes));
  if (!object(value) || value.version !== 1 || value.schema !== 1 || value.packetVersion !== 1 ||
      value.mailboxVersion !== 2 || value.build !== build || typeof value.grid !== "number" ||
      ![16, 32, 64].includes(value.grid) || typeof value.frames !== "number" ||
      !Number.isSafeInteger(value.frames) || value.frames < blockSize || value.frames % blockSize ||
      value.frames > maxBlocks * blockSize || typeof value.outputFrames !== "number" ||
      !Number.isSafeInteger(value.outputFrames) || value.outputFrames > value.frames || value.outputFrames <= value.frames - blockSize || !Array.isArray(value.blocks) || value.blocks.length > maxBlocks ||
      !Array.isArray(value.controls) || !value.controls.every(v => typeof v === "number") ||
      JSON.stringify(value.config) !== JSON.stringify(browserConfig(true, value.grid)) ||
      typeof value.sourceRate !== "number" || typeof value.source !== "string" || typeof value.sourceSha256 !== "string" || typeof value.pcmSha256 !== "string" || !/^[0-9a-f]{64}$/.test(value.pcmSha256))
    throw new Error("Incompatible replay build, schema, configuration, or duration");
  const raw = unbase64(value.source, maxSourceFrames * 8);
  if (raw.length < 32 || raw.length % 8 || await sha256(raw) !== value.sourceSha256)
    throw new Error("Replay source hash or length mismatch");
  const sourceView = new DataView(raw.buffer);
  const source = Float64Array.from({length: raw.length / 8}, (_, i) => sourceView.getFloat64(i * 8, true));
  let previousFrame = -1;
  const fields = new Map<string, number>();
  const acceptances = new Set<string>();
  for (const block of value.blocks) {
    if (!object(block) || typeof block.frame !== "number" || !Number.isSafeInteger(block.frame) ||
        block.frame <= previousFrame || block.frame % blockSize || block.frame < 0 || block.frame >= value.frames ||
        Object.keys(block).some(name => !["frame", "packet", "command", "recovery", "reset", "resume", "accepted"].includes(name)))
      throw new Error("Invalid replay block order");
    previousFrame = block.frame;
    for (const name of ["reset", "resume", "recovery"])
      if (block[name] !== undefined && typeof block[name] !== "boolean") throw new Error("Invalid replay control event");
    if (block.packet !== undefined) {
      if (typeof block.packet !== "string") throw new Error("Invalid replay packet");
      const packet = unbase64(block.packet, packetBytes(value.grid));
      if (packet.length !== packetBytes(value.grid)) throw new Error("Truncated replay packet");
      // Ingress records deliberately retain rejected packets too. C validates
      // them again; acceptance below must refer to a packet actually seen.
      fields.set(packetKey(packet), block.frame);
    }
    if (block.command !== undefined && (typeof block.command !== "string" ||
        unbase64(block.command, commandBytes).length !== commandBytes)) throw new Error("Invalid replay command");
    if (block.accepted !== undefined) {
      const a = block.accepted;
      if (!object(a) || !uint64(a.frame) || !uint64(a.epoch) || !uint64(a.sequence) ||
          BigInt(a.frame) < BigInt(block.frame) || BigInt(a.frame) >= BigInt(block.frame + blockSize) ||
          !fields.has(a.epoch + ":" + a.sequence) || acceptances.has(a.epoch + ":" + a.sequence))
        throw new Error("Invalid replay acceptance clock or payload reference");
      acceptances.add(a.epoch + ":" + a.sequence);
    }
  }
  return {tape: value as unknown as Tape,
    settings: {source, sourceRate: value.sourceRate, controls: value.controls}};
}
/** Import performs bounds/build/source validation before creating Csound. */
export class ReplayPlayer {
  readonly engine: OfflineEngine;
  readonly frames: number;
  readonly outputFrames: number;
  private frame = 0;
  private cursor = 0;
  private previous = "0:0";
  private readonly producer: SharedRing;
  private readonly commands: SharedRing;
  private readonly command = new Uint8Array(commandBytes);
  private closed = false;
  private readonly pcm: Float64Array[] = [];
  private constructor(api: CsoundApi, includes: string, private readonly tape: Tape, settings: Settings) {
    const queues = setup(tape.grid);
    this.engine = new OfflineEngine(api, settings, includes, false, queues);
    this.frames = tape.frames; this.outputFrames = tape.outputFrames;
    this.producer = new SharedRing(packetBytes(tape.grid), queues.fields);
    this.commands = new SharedRing(commandBytes, queues.commands);
  }
  static async create(api: CsoundApi, includes: string, bytes: Uint8Array, build: string): Promise<ReplayPlayer> {
    const {tape, settings} = await decode(bytes, build);
    return new ReplayPlayer(api, includes, tape, settings);
  }
  render(): Float64Array {
    if (this.closed || this.frame >= this.frames) throw new Error("Replay is closed or complete");
    const next = this.tape.blocks[this.cursor];
    const event = next?.frame === this.frame ? next : undefined;
    if (event) this.cursor++;
    try {
      if (event?.reset !== undefined) this.engine.reset(event.reset);
      if (event?.resume) this.engine.resumeExternal();
      if (event?.recovery) this.commands.requestRecovery();
      if (event?.packet && !this.producer.push(unbase64(event.packet, this.producer.bytes)))
        throw new Error("Replay field queue unexpectedly full");
      const pcm = this.engine.render(1);
      const accepted = this.engine.bridge!.acceptance();
      const changed = key(accepted) !== this.previous;
      if (changed !== Boolean(event?.accepted) || (changed &&
          (key(accepted) !== key(event!.accepted!) || accepted.frame !== event!.accepted!.frame)))
        throw new Error("Replay diverged at field acceptance frame " + this.frame);
      this.previous = key(accepted);
      const exported = this.commands.pop(this.command);
      if (exported !== Boolean(event?.command) || (exported && base64(this.command) !== event!.command))
        throw new Error("Replay diverged at control/epoch frame " + this.frame);
      this.pcm.push(pcm.slice());
      this.frame += blockSize;
      return pcm;
    } catch (error) { this.closed = true; throw error; }
  }
  async verify(): Promise<void> {
    if (this.closed || this.frame !== this.frames || await hashPCM(this.pcm) !== this.tape.pcmSha256)
      throw new Error("Replay PCM hash mismatch or incomplete render");
  }
  destroy(): void { this.closed = true; this.engine.destroy(); }
}
