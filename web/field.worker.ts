import {SolverEngine, SharedRing, commandBytes, requireIsolation, type BridgeSetup} from "./bridge";
import {GPUProvider} from "./gpu-provider";
import {browserConfig, type CsoundApi} from "./engine";

Object.defineProperty(globalThis, "window", {value: {
  atob: globalThis.atob.bind(globalThis), btoa: globalThis.btoa.bind(globalThis),
  webkitAudioContext: undefined,
}});
let engine: SolverEngine | GPUProvider | undefined;
let pumping = false;
let lastReport = 0;
let commands: SharedRing | undefined;
let started = false;
let stopped = false;
let paused = false;
function closeSolver() {
  stopped = true;
  engine?.destroy(); engine = undefined;
  globalThis.postMessage({type: "stopped"});
}
function fail(error: unknown) {
  closeSolver();
  globalThis.postMessage({type: "error",
    message: error instanceof Error ? error.message : String(error)});
}
async function pump() {
  if (stopped || paused || !engine || pumping) return;
  if (commands?.stopped) { closeSolver(); return; }
  pumping = true;
  try {
    const deadline = performance.now() + 8;
    for (let count = 0; count < 4 && performance.now() < deadline; count++) {
      if (stopped || paused || !engine || !await engine.work()) break;
    }
    if (engine instanceof GPUProvider && performance.now() - lastReport >= 100) {
      lastReport = performance.now();
      globalThis.postMessage({type: "provider", ...engine.report()});
    }
  } catch (error) { fail(error); }
  finally {
    pumping = false;
    if (!stopped && !paused) setTimeout(() => { void pump(); }, 1);
  }
}
globalThis.onmessage = async ({data}: MessageEvent<
  {type: "start"; setup: BridgeSetup; backend?: "gpu" | "external"} | {type: "stop"} | {type: "pause"} | {type: "resume"}
>) => {
  try {
    if (data.type === "stop") { closeSolver(); return; }
    if (data.type === "pause") { paused = true; globalThis.postMessage({type: "paused"}); return; }
    if (data.type === "resume") { paused = false; pump(); return; }
    if (started) throw new Error("Solver worker cannot be restarted");
    started = true; requireIsolation();
    commands = new SharedRing(commandBytes, data.setup.commands);
    const hostUrl = new URL("./csound.js", import.meta.url).href;
    const {libcsound} = await import(hostUrl) as {
      libcsound(options: {withPlugins: ArrayBuffer[]}): Promise<CsoundApi>
    };
    const response = await fetch(new URL("./naviergrain.wasm", import.meta.url));
    if (!response.ok) throw new Error("Missing solver plugin");
    const api = await libcsound({withPlugins: [await response.arrayBuffer()]});
    if (stopped) return;
    engine = data.backend === "gpu" ?
      await GPUProvider.create(api, browserConfig(true, data.setup.grid), data.setup) :
      new SolverEngine(api, browserConfig(true, data.setup.grid), data.setup);
    if (stopped) { engine.destroy(); engine = undefined; return; }
    globalThis.postMessage({type: "ready",
      provider: engine instanceof GPUProvider ? engine.report() : undefined});
    pump();
  } catch (error) { fail(error); }
};
