/** Device playback only: synthesis remains in the existing render worker. */
export class BufferedPreview {
  private node?: AudioWorkletNode;
  private context?: AudioContext;
  private closed = false;
  async start(report: (message: {type: string; played?: number; underruns?: number; message?: string; suspended?: boolean}) => void): Promise<MessagePort> {
    const context = new AudioContext({sampleRate: 48000});
    this.context = context;
    const fail = (message: string) => {
      if (this.closed) return;
      this.close();
      report({type: "error", message});
    };
    try {
      // Called from the render-button gesture, before asynchronous module loading.
      await context.resume();
      if (context.sampleRate !== 48000) throw new Error("Preview requires a 48 kHz audio context");
      await context.audioWorklet.addModule(new URL("./preview.worklet.js", import.meta.url));
      if (this.closed) throw new Error("Preview cancelled");
      const node = new AudioWorkletNode(context, "naviergrain-preview", {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
      });
      this.node = node;
      node.port.onmessage = ({data}) => {
        if (this.closed) return;
        node.port.postMessage({type: "ack"});
        if (data.type === "error") { fail(data.message ?? "Audio preview stopped"); return; }
        if (data.type === "ended") this.close();
        report(data);
      };
      node.onprocessorerror = () => fail("Audio preview stopped");
      const reportState = () => {
        if (this.closed) return;
        if (context.state === "closed") { fail("Audio device closed. Render again with preview off to save a WAV."); return; }
        // Also covers the browser-specific 'interrupted' state. Do not auto-resume:
        // browsers may require a fresh gesture after an OS/device interruption.
        report({type: "context", suspended: context.state !== "running"});
      };
      context.onstatechange = reportState;
      const channel = new MessageChannel();
      node.port.postMessage({type: "connect", port: channel.port1}, [channel.port1]);
      node.connect(context.destination);
      reportState();
      return channel.port2;
    } catch (error) { this.close(); throw error; }
  }
  async resume(): Promise<void> {
    if (this.closed || !this.context) return;
    // Does not change the queue's manual pause or restart the synthesis worker.
    await this.context.resume();
  }
  pause(paused: boolean): void { this.node?.port.postMessage({type: "pause", paused}); }
  close(): void {
    this.closed = true;
    if (this.context) this.context.onstatechange = null;
    if (this.node) { this.node.onprocessorerror = null; this.node.disconnect(); this.node.port.close(); }
    if (this.context && this.context.state !== "closed") void this.context.close().catch(() => {});
  }
}
