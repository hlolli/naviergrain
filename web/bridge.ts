/** Browser bridge v1: four fixed SAB slots, one writer/reader per direction.
 * WASM memories are ordinary ArrayBuffers, never shared with another worker. */
import type {CsoundApi} from "./engine";
import {reserveHostScratch} from "./channels";
export const commandBytes = 256;
export const maxPacketBytes = 65664;
export const mailboxBytes = 64 + 2 * maxPacketBytes;
export const packetBytes = (grid: number) => {
  if (![16, 32, 64].includes(grid)) throw new Error("Unsupported bridge grid");
  return 128 + 16 * grid * grid;
};
export function requireIsolation(): void {
  if (!globalThis.isSecureContext || !globalThis.crossOriginIsolated ||
      typeof SharedArrayBuffer === "undefined")
    throw new Error("External solvers need a secure, cross-origin-isolated page. Internal CPU still works.");
}
export class SharedRing {
  readonly buffer: SharedArrayBuffer;
  private readonly words: Int32Array;
  private readonly slots: Uint8Array[];
  private cursor = 0;
  constructor(readonly bytes: number, buffer?: SharedArrayBuffer) {
    if (!Number.isInteger(bytes) || bytes < 1 || bytes > maxPacketBytes)
      throw new Error("Invalid shared slot size");
    this.buffer = buffer ?? new SharedArrayBuffer(64 + 4 * bytes);
    if (this.buffer.byteLength !== 64 + 4 * bytes)
      throw new Error("Incorrect shared queue length");
    this.words = new Int32Array(this.buffer, 0, 16);
    if (!buffer) {
      this.words[0] = 0x46475131; this.words[1] = 1;
      this.words[2] = bytes; this.words[3] = 4;
    }
    if (this.words[0] !== 0x46475131 || this.words[1] !== 1 ||
        this.words[2] !== bytes || this.words[3] !== 4)
      throw new Error("Incompatible shared queue");
    this.slots = Array.from({length: 4}, (_, i) => new Uint8Array(this.buffer, 64 + i * bytes, bytes));
  }
  /** Only the producer calls push; full means drop NEW, never overwrite. */
  push(source: Uint8Array): boolean {
    if (source.length !== this.bytes) throw new Error("Incomplete publication");
    if (Atomics.load(this.words, 8 + this.cursor)) {
      Atomics.add(this.words, 5, 1); return false;
    }
    this.slots[this.cursor]!.set(source);
    Atomics.store(this.words, 8 + this.cursor, 1);
    Atomics.add(this.words, 4, 1);
    this.cursor = (this.cursor + 1) % 4;
    return true;
  }
  /** Only the consumer calls pop; release only after the entire owned copy. */
  pop(target: Uint8Array): boolean {
    if (target.length !== this.bytes) throw new Error("Incorrect copy destination");
    if (!Atomics.load(this.words, 8 + this.cursor)) return false;
    target.set(this.slots[this.cursor]!);
    Atomics.store(this.words, 8 + this.cursor, 0);
    Atomics.add(this.words, 6, 1);
    this.cursor = (this.cursor + 1) % 4;
    return true;
  }
  // Word 12 is a one-way provider->audio recovery request on COMMAND queues.
  // The audio consumer owns epochs; providers must never invent timestamps.
  requestRecovery(): void { Atomics.store(this.words, 12, 1); }
  takeRecovery(): boolean { return Atomics.exchange(this.words, 12, 0) !== 0; }
  stop(): void { Atomics.store(this.words, 7, 1); }
  get stopped(): boolean { return Atomics.load(this.words, 7) !== 0; }
  stats() {
    return {published: Atomics.load(this.words, 4) >>> 0,
      dropped: Atomics.load(this.words, 5) >>> 0,
      consumed: Atomics.load(this.words, 6) >>> 0};
  }
}
export interface BridgeSetup {
  commands: SharedArrayBuffer;
  fields: SharedArrayBuffer;
  grid: number;
}
export class Mailbox {
  private heap!: ArrayBuffer;
  private header!: Uint32Array;
  private input!: Uint8Array;
  private output!: Uint8Array;
  rejected = 0;
  // Offline recording only. The callback must take its own copy immediately.
  onIngress?: (packet: Uint8Array | undefined, recovery: boolean) => void;
  onCommand?: (command: Uint8Array) => void;
  acceptance(): {frame: string; epoch: string; sequence: string} {
    this.checkHeap();
    const word = (index: number) => (BigInt(this.header[index]!) |
      (BigInt(this.header[index + 1]!) << 32n)).toString();
    return {frame: word(10), epoch: word(12), sequence: word(14)};
  }
  constructor(private readonly memory: WebAssembly.Memory, private readonly offset: number,
              private readonly mode: 0 | 1, private readonly grid: number,
              readonly incoming: SharedRing, readonly outgoing: SharedRing) {
    if (incoming.bytes !== (mode === 0 ? packetBytes(grid) : commandBytes) ||
        outgoing.bytes !== (mode === 0 ? commandBytes : packetBytes(grid)))
      throw new Error("Mailbox queue sizes do not match the prepared mode");
    this.reanchorOffline();
  }
  /** Only after preparation or the explicitly offline observation-copy helper. */
  reanchorOffline(): void {
    const heap = this.memory.buffer;
    if (typeof SharedArrayBuffer !== "undefined" && heap instanceof SharedArrayBuffer)
      throw new Error("This binding requires a non-shared WASM heap");
    if (!Number.isSafeInteger(this.offset) || this.offset < 0 ||
        this.offset % 4 || this.offset + mailboxBytes > heap.byteLength)
      throw new Error("Invalid host-local mailbox offset");
    this.heap = heap;
    this.header = new Uint32Array(heap, this.offset, 16);
    if (this.header[0] !== 0x46474231 || this.header[1] !== 2 ||
        this.header[2] !== mailboxBytes || this.header[3] !== this.mode ||
        this.header[4] !== packetBytes(this.grid) || this.header[5] !== commandBytes)
      throw new Error("Incompatible prepared mailbox");
    this.input = new Uint8Array(heap, this.offset + 64, this.incoming.bytes);
    this.output = new Uint8Array(heap, this.offset + 64 + maxPacketBytes, this.outgoing.bytes);
  }
  checkHeap(): void {
    if (this.memory.buffer !== this.heap) throw new Error("WASM heap grew during external processing");
  }
  before(): boolean {
    this.checkHeap();
    const recovery = this.mode === 0 && this.outgoing.takeRecovery();
    if (recovery) this.header[9] = 1;
    const admitted = !this.header[6] && this.incoming.pop(this.input);
    if (admitted) this.header[6] = 1;
    if (this.mode === 0 && (admitted || recovery))
      this.onIngress?.(admitted ? this.input : undefined, recovery);
    return this.header[6] === 1;
  }
  after(): void {
    this.checkHeap();
    if (this.header[8]) { this.rejected++; this.header[8] = 0; }
    if (this.header[7]) {
      if (this.mode === 0) this.onCommand?.(this.output);
      this.outgoing.push(this.output); this.header[7] = 0;
    }
  }
  resume(): void {
    this.checkHeap(); this.header[9] = 1;
  }
}
export class SolverEngine {
  private handle: number;
  readonly mailbox: Mailbox;
  constructor(private readonly api: CsoundApi, config: readonly number[], setup: BridgeSetup) {
    this.handle = api.csoundCreate();
    if (!this.handle) throw new Error("Solver host creation failed");
    try {
      const csd = `<CsoundSynthesizer>
<CsOptions>
-n -d -m0
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 2
0dbfs = 1
instr 1
iConfig[] fillarray ${config.join(",")}
kAddress fluidgrain_browser iConfig, 1
chnset kAddress, "fg.bridge"
endin
</CsInstruments>
<CsScore>
i 1 0 -1
f 0 z
</CsScore>
</CsoundSynthesizer>`;
      if (api.csoundCompileCSD(this.handle, csd) || api.csoundStart(this.handle) ||
          api.csoundPerformKsmps(this.handle)) throw new Error("Solver preparation failed");
      if (!api.wasm) throw new Error("Pinned host raw API is unavailable");
      reserveHostScratch(api.wasm.exports);
      this.mailbox = new Mailbox(api.getMemory(),
        api.csoundGetControlChannel(this.handle, "fg.bridge"), 1, setup.grid,
        new SharedRing(commandBytes, setup.commands),
        new SharedRing(packetBytes(setup.grid), setup.fields));
    } catch (error) { this.destroy(); throw error; }
  }
  work(): boolean {
    if (!this.handle) throw new Error("Solver is closed");
    if (!this.mailbox.before()) return false;
    if (this.api.csoundPerformKsmps(this.handle)) throw new Error("Solver stopped unexpectedly");
    this.mailbox.after();
    return true;
  }
  destroy(): void {
    if (this.handle) this.api.csoundDestroy(this.handle);
    this.handle = 0;
  }
}
