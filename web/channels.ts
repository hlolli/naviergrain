/** Pinned libcsound's exposed raw WASM boundary, prepared outside performance.
 * Public JS channel helpers allocate string scratch on every call. Resolve
 * supported Csound channel pointers once instead; never pass them to workers. */
export interface ChannelExports {
  allocStringMem(bytes: number): number;
  freeStringMem(pointer: number): void;
  csoundGetChannelPtr(handle: number, result: number, name: number, type: number): number;
}
/** Reserve reusable allocator scratch before capturing any views. This is
 * headroom for the bounded 30-second offline workbench, not an allocation-free
 * host or unlimited live playback claim. */
export function reserveHostScratch(exports: ChannelExports): void {
  const pointer = exports.allocStringMem(8 * 1024 * 1024);
  if (!pointer) throw new Error("Cannot reserve browser host scratch");
  exports.freeStringMem(pointer);
}
export class PreparedChannels {
  private readonly offsets = new Map<string, number>();
  private readonly views = new Map<string, Float64Array>();
  constructor(private readonly memory: WebAssembly.Memory, exports: ChannelExports,
              handle: number, names: string[]) {
    if (!exports || typeof exports.csoundGetChannelPtr !== "function" ||
        typeof exports.allocStringMem !== "function" || typeof exports.freeStringMem !== "function")
      throw new Error("Pinned host channel-pointer API is unavailable");
    const encoder = new TextEncoder();
    for (const name of names) {
      const bytes = encoder.encode(name + "\0");
      const text = exports.allocStringMem(bytes.length);
      const result = exports.allocStringMem(8);
      try {
        if (!text || !result) throw new Error("Channel preparation allocation failed");
        new Uint8Array(memory.buffer, text, bytes.length).set(bytes);
        if (exports.csoundGetChannelPtr(handle, result, text, 1 | 16 | 32))
          throw new Error("Cannot prepare channel: " + name);
        const offset = new DataView(memory.buffer).getUint32(result, true);
        if (!offset || offset % 8 || offset + 8 > memory.buffer.byteLength)
          throw new Error("Invalid prepared channel: " + name);
        this.offsets.set(name, offset);
      } finally {
        if (text) exports.freeStringMem(text);
        if (result) exports.freeStringMem(result);
      }
    }
    this.reanchorOffline();
  }
  reanchorOffline(): void {
    for (const [name, offset] of this.offsets)
      this.views.set(name, new Float64Array(this.memory.buffer, offset, 1));
  }
  get(name: string): number { return this.view(name)[0]!; }
  set(name: string, value: number): void { this.view(name)[0] = value; }
  private view(name: string): Float64Array {
    const view = this.views.get(name);
    if (!view || view.buffer !== this.memory.buffer) throw new Error("Prepared channel view is unavailable");
    return view;
  }
}
