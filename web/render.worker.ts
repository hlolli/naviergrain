import {ReplayCapture, sha256} from "./replay";
import {requireIsolation, type BridgeSetup} from "./bridge";
import {OfflineEngine, blockSize, sampleRate, maxRenderSeconds, encodeWav,
  type CsoundApi, type Settings} from "./engine";

// The pinned browser bundle accesses these browser globals during module load.
// Its public libcsound entry itself needs no DOM, AudioContext, SAB or WebGPU.
Object.defineProperty(globalThis, "window", {value: {
  atob: globalThis.atob.bind(globalThis), btoa: globalThis.btoa.bind(globalThis),
  webkitAudioContext: undefined,
}});
let engine: OfflineEngine | undefined;
let capture: ReplayCapture | undefined;
let paused = false;
let started = false;
let pumping = false;
let offset = 0;
let pcm: Float64Array;
let peak = 0;
let startedAt = 0;
let lastTelemetry = 0;
let viewRequested = false;
let external = false;
const post = (value: unknown) => globalThis.postMessage(value);
function bridgeStats() {
  return engine?.bridge ? {commands: engine.bridge.outgoing.stats(),
    fields: engine.bridge.incoming.stats(), rejected: engine.bridge.rejected} : undefined;
}
function fail(error: unknown) {
  capture?.destroy(); capture = undefined;
  engine?.destroy(); engine = undefined;
  post({type: "error", message: error instanceof Error ? error.message : String(error)});
}
function pump() {
  if (paused || !engine || pumping) return;
  pumping = true;
  try {
    const deadline = performance.now() + 8;
    let blocks = 0;
    do {
      blocks++;
      const audio = capture ? capture.render(viewRequested) : engine.render(1, viewRequested);
      if (viewRequested) {
        const snapshot = engine.view();
        globalThis.postMessage({type: "view", snapshot}, {transfer: [snapshot.buffer]});
        viewRequested = false;
      }
      const count = Math.min(audio.length, pcm.length - offset);
      pcm.set(audio.subarray(0, count), offset);
      for (let i = 0; i < count; i++) peak = Math.max(peak, Math.abs(audio[i]!));
      offset += count;
    } while (offset < pcm.length && performance.now() < deadline && (!external || blocks < 4));
    const stats = engine.stats();
    if (offset === pcm.length) {
      const wav = encodeWav(pcm);
      engine.bridge?.outgoing.stop();
      const bridge = bridgeStats();
      const saved = capture?.finish(pcm.length / 2);
      engine.destroy(); engine = undefined;
      if (saved) post({type: "finalizing"});
      Promise.resolve(saved).then(replay => {
        const transfer: Transferable[] = [wav];
        if (replay) transfer.push(replay.buffer as ArrayBuffer);
        globalThis.postMessage({type: "done", wav, replay, stats, peak, bridge,
          renderMs: performance.now() - startedAt}, {transfer});
        capture = undefined;
      }).catch(fail);
    } else {
      if (performance.now() - lastTelemetry >= 100) {
        lastTelemetry = performance.now();
        post({type: "progress", fraction: offset / pcm.length, stats, peak, bridge: bridgeStats()});
      }
      setTimeout(pump, external ? 5 : 0);
    }
  } catch (error) { fail(error); }
  finally { pumping = false; }
}
globalThis.onmessage = async ({data}: MessageEvent<
  {type: "start"; settings: Settings; seconds: number; setup?: BridgeSetup; record?: boolean} | {type: "pause"} | {type: "resume"} | {type: "view"}
>) => {
  try {
    if (data.type === "view") {
      if (!engine || paused) post({type: "view", snapshot: null});
      else viewRequested = true;
      return;
    }
    if (data.type === "pause") { paused = true; post({type: "paused"}); return; }
    if (data.type === "resume") { if (capture) capture.resume(); else engine?.resumeExternal(); paused = false; pump(); return; }
    if (started) throw new Error("Create a fresh worker for each render");
    started = true; external = Boolean(data.setup);
    if (external) requireIsolation();
    if (!Number.isFinite(data.seconds) || data.seconds < .1 || data.seconds > maxRenderSeconds)
      throw new Error("Render duration must be 0.1–30 seconds");
    startedAt = performance.now();
    // Relative import stays external to the small application bundle.
    const hostUrl = new URL("./csound.js", import.meta.url).href;
    const {libcsound} = await import(hostUrl) as {
      libcsound(options: {withPlugins: ArrayBuffer[]}): Promise<CsoundApi>
    };
    const [pluginResponse, includesResponse] = await Promise.all([
      fetch(new URL("./fluidgrain.wasm", import.meta.url)),
      fetch(new URL("./prepared.inc", import.meta.url)),
    ]);
    if (!pluginResponse.ok || !includesResponse.ok) throw new Error("Missing instrument assets");
    const api = await libcsound({withPlugins: [await pluginResponse.arrayBuffer()]});
    const includes = await includesResponse.text();
    if (data.record) {
      if (!data.setup) throw new Error("Replay capture requires an external solver");
      const response = await fetch(new URL("./provenance.json", import.meta.url));
      if (!response.ok) throw new Error("Missing replay build identity");
      const build = await sha256(new Uint8Array(await response.arrayBuffer()));
      capture = new ReplayCapture(api, data.settings, includes, data.setup, build, true);
      engine = capture.engine;
    } else engine = new OfflineEngine(api, data.settings, includes, true, data.setup);
    pcm = new Float64Array(Math.round(data.seconds * sampleRate) * 2);
    post({type: "ready", stats: engine.stats(), blockSize});
    pump();
  } catch (error) { fail(error); }
};
