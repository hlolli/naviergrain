/** Bounded PCM queue, independent of DSP and the browser device clock. */
export const previewSlots = 16;
export const previewChunkFrames = 512;
export const previewPrefillFrames = 4096;
export class PreviewQueue {
  private readonly slots: (Float32Array | undefined)[] = Array(previewSlots);
  private head = 0;
  private tail = 0;
  private count = 0;
  private offset = 0;
  private expected = 0;
  private running = false;
  private final = false;
  private gain = 0;
  paused = false;
  buffered = 0;
  maximumBuffered = 0;
  played = 0;
  underruns = 0;
  ended = false;
  constructor(private readonly credit: () => void) {}
  push(start: number, pcm: Float32Array, final: boolean): void {
    if (this.final || this.count === previewSlots || start !== this.expected ||
        !(pcm instanceof Float32Array) || !pcm.length || pcm.length % 2 ||
        pcm.length > previewChunkFrames * 2 || !pcm.every(Number.isFinite))
      throw new Error("Invalid or out-of-order preview delivery");
    this.slots[this.tail] = pcm;
    this.tail = (this.tail + 1) % previewSlots;
    this.count++;
    this.expected += pcm.length / 2;
    this.buffered += pcm.length / 2;
    this.maximumBuffered = Math.max(this.maximumBuffered, this.buffered);
    this.final = final;
  }
  process(left: Float32Array, right: Float32Array): void {
    left.fill(0); right.fill(0);
    if (this.paused || this.ended) return;
    if (!this.running) {
      if (!this.final && this.buffered < previewPrefillFrames) return;
      this.running = true;
    }
    for (let i = 0; i < left.length; i++) {
      const chunk = this.slots[this.head];
      if (!chunk) {
        if (this.final) this.ended = true;
        else { this.underruns++; this.running = false; }
        this.gain = 0;
        break;
      }
      // Short ramps soften preview start/rebuffer/tail only. The saved WAV is untouched.
      const target = this.buffered < 64 ? this.buffered / 64 : 1;
      this.gain = Math.min(target, this.gain + 1 / 64);
      left[i] = chunk[this.offset]! * this.gain;
      right[i] = chunk[this.offset + 1]! * this.gain;
      this.offset += 2; this.buffered--; this.played++;
      if (this.offset === chunk.length) {
        this.slots[this.head] = undefined;
        this.head = (this.head + 1) % previewSlots;
        this.count--; this.offset = 0;
        this.credit();
      }
    }
    if (this.final && !this.buffered) this.ended = true;
  }
}
