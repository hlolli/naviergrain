import {PreviewQueue} from "./preview-queue";
declare abstract class AudioWorkletProcessor {
  readonly port: MessagePort;
}
declare function registerProcessor(name: string, processor: typeof AudioWorkletProcessor): void;

class BufferedPreview extends AudioWorkletProcessor {
  private stream?: MessagePort;
  private queue = new PreviewQueue(() => this.stream?.postMessage({type: "credit"}));
  private calls = 0;
  private failed = false;
  private reportPending = false;
  constructor() {
    super();
    this.port.onmessage = ({data}) => {
      if (data.type === "ack") this.reportPending = false;
      if (data.type === "pause") this.queue.paused = data.paused === true;
      if (data.type === "connect" && !this.stream) {
        this.stream = data.port;
        this.stream!.onmessage = ({data}) => {
          try { this.queue.push(data.start, data.pcm, data.final); }
          catch {
            this.failed = true;
            this.port.postMessage({type: "error", message: "Preview queue rejected a delivery"});
          }
        };
        this.stream!.start();
      }
    };
  }
  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const channels = outputs[0];
    if (this.failed || !channels?.[0] || !channels[1]) return false;
    this.queue.process(channels[0], channels[1]);
    if ((++this.calls % 32 === 0 && !this.reportPending) || this.queue.ended) {
      this.reportPending = true;
      this.port.postMessage({type: this.queue.ended ? "ended" : "progress",
        played: this.queue.played, buffered: this.queue.buffered,
        maximumBuffered: this.queue.maximumBuffered, underruns: this.queue.underruns});
    }
    return !this.queue.ended;
  }
}
registerProcessor("fluidgrain-preview", BufferedPreview);
