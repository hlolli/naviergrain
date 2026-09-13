/** Worker-only GPU publication with a preprepared, warm C/WASM standby.
 * One command/step/readback in flight. The audio worker never calls this code.
 */
import {SolverEngine, SharedRing, commandBytes, packetBytes, type BridgeSetup} from "./bridge";
import {GPUFieldSolver, type GPUField, type GPUProfile} from "./gpu-solver";
import {config as I, control as C} from "./naviergrain-schema";
import schema from "../schema/naviergrain-v1.json";
import type {CsoundApi} from "./engine";

export interface FieldCommand {
  key: bigint; epoch: bigint; reset: bigint; sequence: bigint; frame: bigint;
  controls: number[];
}
export function decodeFieldCommand(bytes: Uint8Array, key: bigint): FieldCommand {
  if (bytes.length !== commandBytes) throw new Error("Incomplete field command");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) !== 0x4d43474e || view.getUint32(4, true) !== 1 ||
      view.getBigUint64(8, true) !== key || bytes.subarray(48,64).some(v => v !== 0))
    throw new Error("Invalid field command header");
  const command = {key, epoch:view.getBigUint64(16,true), reset:view.getBigUint64(24,true),
    sequence:view.getBigUint64(32,true), frame:view.getBigUint64(40,true), controls:[] as number[]};
  if (!command.epoch || !command.sequence) throw new Error("Invalid command clock");
  for (let i = 0; i < schema.control.length; i++) {
    const value = view.getFloat64(64+8*i, true), spec = schema.control[i]!;
    const pitch = i === C.pitch_ratio;
    if (!Number.isFinite(value) || value < (pitch ? Math.log2(spec.min) : spec.min) ||
        value > (pitch ? Math.log2(spec.max) : spec.max) || (spec.integer && !Number.isInteger(value)))
      throw new Error("Invalid command control: " + spec.name);
    command.controls.push(pitch ? Math.min(spec.max, Math.max(spec.min, 2**value)) : value);
  }
  return command;
}
/** Wire-identical to ng_packet_encode; no JS-number conversion of uint64 clocks. */
export function encodeGPUField(command: FieldCommand, field: GPUField, grid: number): Uint8Array {
  const bytes = new Uint8Array(packetBytes(grid)), view = new DataView(bytes.buffer);
  const diagnostics = [4,5,6,7,8,1].map(i => field.diagnostics[i]!);
  if (field.planes.length !== 4*grid*grid || !field.planes.every(Number.isFinite) ||
      field.planes.subarray(3*grid*grid).some(v => v < 0) ||
      diagnostics.some(v => !Number.isFinite(v) || v < 0) ||
      !Number.isFinite(field.time) || field.time < 0 ||
      !Number.isSafeInteger(field.interventions) || field.interventions < 0)
    throw new Error("Invalid GPU field packet");
  view.setUint32(0,0x4c46474e,true); view.setUint32(4,1,true);
  view.setUint32(8,128,true); view.setUint32(12,bytes.length-128,true);
  view.setBigUint64(16,command.key,true); view.setBigUint64(24,command.epoch,true);
  view.setBigUint64(32,command.sequence,true); view.setBigUint64(40,command.frame,true);
  view.setFloat64(48,field.time,true); view.setUint32(56,grid,true); view.setUint32(60,grid,true);
  view.setUint32(64,1,true); view.setUint32(68,1,true);
  diagnostics.forEach((value,i) => view.setFloat32(72+4*i,value,true));
  view.setBigUint64(96,BigInt(field.interventions),true);
  field.planes.forEach((value,i) => view.setFloat32(128+4*i,value,true));
  return bytes;
}
export interface GPUBackend {
  readonly adapterInfo: Readonly<Record<string,string>>;
  readonly failure: string | undefined;
  reset(): void;
  step(controls: readonly number[]): Promise<GPUField>;
  destroy(): void;
}
export interface ProviderReport {
  backend: "gpu" | "cpu-fallback"; reason?: string;
  gpuFields: number; cpuFields: number; rejected: number; waitingEpoch: string | null;
  meanServiceMs: number; maxServiceMs: number;
  adapterInfo?: Readonly<Record<string,string>>;
}
// A timeout closes the owner; a late completion is never published or reused.
async function deadline<T>(pending: Promise<T>, ms: number): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try { return await Promise.race([pending, new Promise<never>((_,reject) => {
    timer = setTimeout(() => reject(new Error("GPU operation timed out")), ms);
  })]); } finally { clearTimeout(timer); }
}
export class GPUProvider {
  private readonly commands: SharedRing;
  private readonly fields: SharedRing;
  private readonly cpuCommands = new SharedRing(commandBytes);
  private readonly cpuFields: SharedRing;
  private readonly cpu: SolverEngine;
  private gpu?: GPUBackend;
  private readonly input = new Uint8Array(commandBytes);
  private readonly cpuPacket: Uint8Array;
  private previous?: FieldCommand;
  private waitingEpoch?: bigint;
  private closed = false;
  private working = false;
  private backend: "gpu" | "cpu-fallback" = "cpu-fallback";
  private reason?: string;
  private adapterInfo?: Readonly<Record<string,string>>;
  private gpuFields = 0;
  private cpuFieldCount = 0;
  private rejected = 0;
  private serviceSum = 0;
  private serviceMax = 0;
  private services = 0;

  static async create(api: CsoundApi, cfg: readonly number[], setup: BridgeSetup,
      createGPU: (profile: GPUProfile) => Promise<GPUBackend> = GPUFieldSolver.create): Promise<GPUProvider> {
    const owner = new GPUProvider(api, cfg, setup);
    // C host/buffers/solver exist BEFORE adapter/shader preparation, not on loss.
    const pending = Promise.resolve().then(() => createGPU({grid:setup.grid, seed:cfg[I.seed]!, fluidHz:cfg[I.fluid_hz]!,
      particleHz:cfg[I.particle_hz]!, pressureIterations:cfg[I.pressure_iterations]!,
      viscosityIterations:cfg[I.viscosity_iterations]!}));
    let expired = false;
    void pending.then(gpu => { if (expired) gpu.destroy(); }, () => {});
    try {
      owner.gpu = await deadline(pending, 10000);
      owner.adapterInfo = owner.gpu.adapterInfo;
      owner.backend = "gpu";
    } catch (error) { expired = true; owner.fallback(error, false); }
    return owner;
  }
  private constructor(api: CsoundApi, private readonly cfg: readonly number[], setup: BridgeSetup) {
    this.commands = new SharedRing(commandBytes,setup.commands);
    this.fields = new SharedRing(packetBytes(setup.grid),setup.fields);
    this.cpuFields = new SharedRing(packetBytes(setup.grid));
    this.cpuPacket = new Uint8Array(packetBytes(setup.grid));
    this.cpu = new SolverEngine(api,cfg,{grid:setup.grid,
      commands:this.cpuCommands.buffer,fields:this.cpuFields.buffer});
  }
  private fallback(error: unknown, recovery: boolean): void {
    this.reason = error instanceof Error ? error.message : String(error);
    this.gpu?.destroy(); this.gpu = undefined;
    this.backend = "cpu-fallback";
    if (recovery) {
      this.waitingEpoch = this.previous?.epoch ?? 0n;
      this.commands.requestRecovery();
    }
  }
  get stopped(): boolean { return this.closed || this.commands.stopped; }
  report(): ProviderReport {
    return {backend:this.backend,reason:this.reason,gpuFields:this.gpuFields,cpuFields:this.cpuFieldCount,
      rejected:this.rejected,waitingEpoch:this.waitingEpoch?.toString() ?? null,
      meanServiceMs:this.services ? this.serviceSum/this.services : 0,maxServiceMs:this.serviceMax,
      adapterInfo:this.adapterInfo};
  }
  async work(): Promise<boolean> {
    if (this.stopped) return false;
    if (this.working) throw new Error("GPU provider already has an in-flight command");
    if (this.gpu?.failure) this.fallback(new Error(this.gpu.failure),true);
    if (!this.commands.pop(this.input)) return false;
    let command: FieldCommand;
    try {
      command = decodeFieldCommand(this.input,BigInt(this.cfg[I.instance_id]!));
      const last = this.previous;
      if (last && (command.sequence <= last.sequence || command.epoch < last.epoch ||
          command.reset < last.reset || (command.epoch === last.epoch && command.frame < last.frame)))
        throw new Error("Regressing command clock");
    } catch { this.rejected++; return true; }
    const reset = command.reset !== (this.previous?.reset ?? 0n);
    this.previous = command;
    if (this.waitingEpoch !== undefined) {
      if (command.epoch <= this.waitingEpoch) return true;
      this.waitingEpoch = undefined;
    }
    this.working = true;
    const start = performance.now();
    try {
      // Warm standby follows the identical accepted command schedule. It has
      // private queues: never a second consumer/producer of the audio SABs.
      if (!this.cpuCommands.push(this.input) || !this.cpu.work() || !this.cpuFields.pop(this.cpuPacket))
        throw new Error("Prepared CPU standby failed");
      let packet: Uint8Array;
      if (this.gpu) {
        try {
          if (reset) this.gpu.reset();
          const result = await deadline(this.gpu.step(command.controls),1000);
          if (this.stopped) return true;
          packet = encodeGPUField(command,result,this.cfg[I.grid_size]!);
        } catch (error) {
          if (!this.stopped) this.fallback(error,true);
          return true; // Wait for an AUDIO-owned new epoch, discard failed result.
        }
      } else packet = this.cpuPacket;
      if (!this.stopped && this.fields.push(packet)) {
        if (this.backend === "gpu") this.gpuFields++; else this.cpuFieldCount++;
      }
      return true;
    } finally {
      const elapsed = performance.now()-start;
      this.serviceSum += elapsed; this.serviceMax = Math.max(this.serviceMax,elapsed); this.services++;
      this.working = false;
    }
  }
  destroy(): void {
    if (this.closed) return;
    this.closed = true;
    this.gpu?.destroy(); this.gpu = undefined;
    this.cpu.destroy();
  }
}
