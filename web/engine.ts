import {PreparedChannels, reserveHostScratch, type ChannelExports} from "./channels";
import {Mailbox, SharedRing, commandBytes, packetBytes, type BridgeSetup} from "./bridge";
import schema from "../schema/naviergrain-v1.json";
import {config, control, stat, NG_STATUS_READY} from "./naviergrain-schema";

export const sampleRate = 48000;
export const blockSize = 64;
export const maxSourceFrames = sampleRate * 30;
export const maxRenderSeconds = 30;

export interface CsoundApi {
  wasm?: {exports: ChannelExports};
  csoundCreate(): number;
  csoundCompileCSD(handle: number, csd: string): number;
  csoundStart(handle: number): number;
  csoundPerformKsmps(handle: number): number;
  csoundGetSpout(handle: number): number;
  csoundSetControlChannel(handle: number, name: string, value: number): void;
  csoundGetControlChannel(handle: number, name: string): number;
  csoundTableCopyIn(handle: number, table: number, samples: Float64Array): void;
  csoundTableCopyOut(handle: number, table: number): Float64Array | undefined;
  csoundTableLength(handle: number, table: number): number;
  csoundDestroy(handle: number): void;
  getMemory(): WebAssembly.Memory;
}

export interface Settings {
  controls: number[];
  source: Float64Array;
  sourceRate: number;
}
export type Stats = Record<keyof typeof stat, number>;

export function bellSource(): Float64Array {
  return Float64Array.from({length: 48000}, (_, i) =>
    (Math.sin(2 * Math.PI * 220 * i / sampleRate) +
      .35 * Math.sin(2 * Math.PI * 557 * i / sampleRate) +
      .18 * Math.sin(2 * Math.PI * 911 * i / sampleRate)) *
    Math.exp(-3 * i / sampleRate) * .45);
}

function validate(settings: Settings): void {
  if (settings.controls.length !== schema.control.length)
    throw new Error("Control array does not match the schema");
  schema.control.forEach((item, index) => {
    const value = settings.controls[index]!;
    if (!Number.isFinite(value) || value < item.min || value > item.max ||
        (item.integer && !Number.isInteger(value)))
      throw new Error("Invalid control: " + item.name);
  });
  if (!(settings.source instanceof Float64Array) || settings.source.length < 4 ||
      settings.source.length > maxSourceFrames ||
      !settings.source.every(Number.isFinite))
    throw new Error("Use a finite mono source of 4 samples to 30 seconds at 48 kHz");
  if (!Number.isFinite(settings.sourceRate) || settings.sourceRate < 8000 ||
      settings.sourceRate > 192000)
    throw new Error("Source rate must be 8–192 kHz");
}

export function browserConfig(external = false, grid = 32): number[] {
  const cfg = schema.config.map(item => item.default);
  cfg[config.source_loop] = 1;
  cfg[config.backend] = Number(external);
  cfg[config.instance_id] = external ? 1 : 0;
  cfg[config.grid_size] = grid;
  return cfg;
}

/** Offline only. One owned host per render; no audio deadline or shared heap. */
export class OfflineEngine {
  private handle: number;
  private buffer: ArrayBuffer;
  readonly bridge?: Mailbox;
  private channels?: PreparedChannels;
  constructor(private readonly api: CsoundApi, settings: Settings, includes: string, private readonly observed = false, external?: BridgeSetup) {
    validate(settings);
    this.handle = api.csoundCreate();
    if (!this.handle) throw new Error("Csound creation failed");
    try {
      const cfg = browserConfig(Boolean(external), external?.grid);
      const assignments = settings.controls.map((value, i) =>
        "kControl[" + i + "] init " + value).join("\n");
      const channels = schema.stat.map((item, i) =>
        'chnset kStats[' + i + '], "ng.' + item.name + '"').join("\n");
      const csd = `<CsoundSynthesizer>
<CsOptions>
-n -d -m0
</CsOptions>
<CsInstruments>
sr = ${sampleRate}
ksmps = ${blockSize}
nchnls = 2
0dbfs = 1
${includes}
giSource ftgen 1, 0, -${settings.source.length}, -2, 0
${observed ? "giView ftgen 2, 0, -3344, -2, 0" : ""}
instr 1
iConfig[] fillarray ${cfg.join(",")}
${external ? 'kAddress naviergrain_browser iConfig, 0\nchnset kAddress, "ng.bridge"' : ""}
kControl[] fillarray ${settings.controls.join(",")}
${assignments}
kControl[${control.reset}] chnget "ng.reset"
kRun chnget "ng.run"
${observed ? 'kObserve chnget "ng.observe"' : ""}
aL, aR, kStats[] ${observed ? "NaviergrainVisualPrepared" : "NaviergrainPrepared"} giSource, ${settings.sourceRate}, iConfig, kControl, kRun${observed ? ", giView, kObserve" : ""}
${channels}
outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 0 -1
f 0 z
</CsScore>
</CsoundSynthesizer>`;
      if (api.csoundCompileCSD(this.handle, csd) !== 0)
        throw new Error("Instrument compilation failed");
      if (api.csoundStart(this.handle) !== 0) throw new Error("Csound start failed");
      if (api.csoundTableLength(this.handle, 1) !== settings.source.length)
        throw new Error("Source table has the wrong size");
      api.csoundTableCopyIn(this.handle, 1, settings.source);
      this.perform();
      const prepared = this.stats();
      if (!(prepared.status & NG_STATUS_READY) || prepared.live_grains !== 0 ||
          prepared.snapshot_sequence !== 0 || prepared.source_length !== settings.source.length)
        throw new Error("Silent preparation did not reach the ready state");
      if (this.output().some(value => value !== 0))
        throw new Error("Preparation leaked audio");
      // Exercise the pinned host's allocating table-copy helper offline before
      // recording the heap identity. No callback-safety claim is made here.
      if (observed && api.csoundTableCopyOut(this.handle, 2)?.length !== 3344)
        throw new Error("Observation table is unavailable");
      // Both internal and external paths need host scratch/channel preparation.
      // Otherwise even a three-second internal render can detach its heap.
      {
        if (!api.wasm) throw new Error("Pinned host raw API is unavailable");
        reserveHostScratch(api.wasm.exports);
        this.channels = new PreparedChannels(api.getMemory(), api.wasm.exports, this.handle,
          ["ng.run", "ng.observe", "ng.reset", "ng.bridge", ...schema.stat.map(item => "ng." + item.name)]);
      }
      this.buffer = api.getMemory().buffer;
      if (external) this.bridge = new Mailbox(api.getMemory(),
        this.channels!.get("ng.bridge"), 0, external.grid,
        new SharedRing(packetBytes(external.grid), external.fields),
        new SharedRing(commandBytes, external.commands));
    } catch (error) {
      this.destroy();
      throw error;
    }
  }
  private perform(): void {
    if (!this.handle) throw new Error("Engine is closed");
    if (this.api.csoundPerformKsmps(this.handle) !== 0)
      throw new Error("Render ended unexpectedly");
  }
  private output(): Float64Array {
    return new Float64Array(this.api.getMemory().buffer,
      this.api.csoundGetSpout(this.handle), blockSize * 2);
  }
  private setChannel(name: string, value: number): void {
    if (this.channels) this.channels.set(name, value);
    else this.api.csoundSetControlChannel(this.handle, name, value);
  }
  stats(): Stats {
    return Object.fromEntries(schema.stat.map(item => [item.name,
      (this.channels ? this.channels.get("ng." + item.name) :
        this.api.csoundGetControlChannel(this.handle, "ng." + item.name))])) as Stats;
  }
  /** Gate blocks are for lifecycle verification, not missed-wall-time catchup. */
  silence(blocks = 1): void {
    this.setChannel("ng.run", 0);
    for (let i = 0; i < blocks; i++) {
      this.perform();
      if (this.output().some(value => value !== 0)) throw new Error("Pause leaked audio");
    }
  }
  resumeExternal(): void { this.bridge?.resume(); }
  reset(high: boolean): void {
    this.setChannel("ng.reset", Number(high));
  }
  render(blocks: number, observe = false): Float64Array {
    if (!Number.isInteger(blocks) || blocks < 1 || blocks > 64)
      throw new Error("Render chunks must contain 1–64 blocks");
    if (observe && !this.observed) throw new Error("Observation was not prepared");
    this.setChannel("ng.observe", Number(observe));
    const result = new Float64Array(blocks * blockSize * 2);
    this.setChannel("ng.run", 1);
    for (let i = 0; i < blocks; i++) {
      this.bridge?.before();
      this.perform();
      this.bridge?.after();
      if (this.api.getMemory().buffer !== this.buffer)
        throw new Error("WASM memory grew after preparation");
      const samples = this.output();
      if (!samples.every(Number.isFinite)) throw new Error("Non-finite rendered audio");
      result.set(samples, i * blockSize * 2);
    }
    return result;
  }
  view(): Float64Array {
    if (!this.handle || !this.observed) throw new Error("Observation is unavailable");
    const snapshot = this.api.csoundTableCopyOut(this.handle, 2);
    if (!snapshot || snapshot.length !== 3344 || snapshot[0] !== 1 ||
        snapshot[1] !== 3344 || !snapshot.every(Number.isFinite))
      throw new Error("Invalid observation snapshot");
    // The supported host helper returns an owned JS copy but may grow WASM
    // scratch storage. This offline operation is between performance calls;
    // no spout/table view survives it. Re-anchor only here, never in render().
    this.buffer = this.api.getMemory().buffer;
    this.bridge?.reanchorOffline();
    this.channels?.reanchorOffline();
    return snapshot;
  }
  destroy(): void {
    if (this.handle) this.api.csoundDestroy(this.handle);
    this.handle = 0;
  }
}

/** Float WAV preserves the raw signal, including peaks above full scale. */
export function encodeWav(samples: Float64Array): ArrayBuffer {
  if (samples.length % 2 !== 0 ||
      !samples.every(value => Number.isFinite(Math.fround(value))))
    throw new Error("Audio cannot be represented as finite stereo float32 WAV");
  const buffer = new ArrayBuffer(44 + samples.length * 4);
  const view = new DataView(buffer);
  const text = (offset: number, value: string) =>
    [...value].forEach((letter, i) => view.setUint8(offset + i, letter.charCodeAt(0)));
  text(0, "RIFF"); view.setUint32(4, buffer.byteLength - 8, true); text(8, "WAVE");
  text(12, "fmt "); view.setUint32(16, 16, true); view.setUint16(20, 3, true);
  view.setUint16(22, 2, true); view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 8, true); view.setUint16(32, 8, true);
  view.setUint16(34, 32, true); text(36, "data");
  view.setUint32(40, samples.length * 4, true);
  samples.forEach((value, i) => view.setFloat32(44 + i * 4, value, true));
  return buffer;
}
