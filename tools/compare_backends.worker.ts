/** Offline accepted-schedule comparison, NOT a device or worker-throughput test. */
import {GPUProvider} from "../web/gpu-provider";
import {SharedRing, commandBytes, packetBytes, SolverEngine} from "../web/bridge";
import {OfflineEngine, bellSource, browserConfig, type CsoundApi} from "../web/engine";
import {controlDefaults, control as C} from "../web/naviergrain-schema";

Object.defineProperty(globalThis, "window", {value: {
  atob: globalThis.atob.bind(globalThis), btoa: globalThis.btoa.bind(globalThis),
  webkitAudioContext: undefined,
}});
type Request = {mode: "internal" | "external" | "gpu"; profile: "default" | "swirl" | "fixed"; seconds: number};
function check(value: unknown, message: string): asserts value {
  if (!value) throw new Error(message);
}
function distribution(values: number[]) {
  if (!values.length) return {count: 0, mean: 0, p50: 0, p95: 0, p99: 0, max: 0};
  const sorted = values.slice().sort((a,b) => a-b);
  const at = (q: number) => sorted[Math.ceil(q * sorted.length)-1]!;
  return {count: values.length, mean: values.reduce((a,b) => a+b,0)/values.length,
    p50: at(.5), p95: at(.95), p99: at(.99), max: sorted[sorted.length-1]!};
}
async function render(request: Request) {
  check(["internal","external","gpu"].includes(request.mode) &&
    ["default","swirl","fixed"].includes(request.profile) &&
    Number.isInteger(request.seconds) && request.seconds >= 1 && request.seconds <= 30, "Invalid comparison");
  const cold = performance.now();
  const {libcsound} = await import(new URL("./csound.js", import.meta.url).href) as {
    libcsound(options: {withPlugins: ArrayBuffer[]}): Promise<CsoundApi>
  };
  const plugin = await (await fetch("./naviergrain.wasm")).arrayBuffer();
  const includes = await (await fetch("./prepared.inc")).text();
  const audioApi = await libcsound({withPlugins: [plugin]});
  const controls: number[] = [...controlDefaults];
  controls[C.scheduler] = 0;
  if (request.profile === "fixed") controls[C.mapping_mix] = 0;
  if (request.profile === "swirl") {
    controls[C.grain_rate] = 600; controls[C.grain_ms] = 180;
    controls[C.turbulence] = .6; controls[C.strain_drive] = .4;
    controls[C.confinement] = .15; controls[C.inertia_ms] = 80;
  }
  const setup = request.mode === "internal" ? undefined :
    {grid: 32, commands: new SharedRing(commandBytes).buffer, fields: new SharedRing(packetBytes(32)).buffer};
  let provider: GPUProvider | SolverEngine | undefined, engine: OfflineEngine | undefined;
  let cpuApi: CsoundApi | undefined;
  try {
    const providerStart = performance.now();
    if (setup) {
      cpuApi = await libcsound({withPlugins: [plugin]});
      provider = request.mode === "gpu" ? await GPUProvider.create(cpuApi, browserConfig(true,32),setup) :
        new SolverEngine(cpuApi,browserConfig(true,32),setup);
      if (provider instanceof GPUProvider) check(provider.report().backend === "gpu", "Requested GPU unavailable; refusing CPU substitution");
    }
    const providerPrepareMs = performance.now() - providerStart;
    engine = new OfflineEngine(audioApi, {source: bellSource(), sourceRate: 48000, controls}, includes, false, setup);
    const prepareMs = performance.now() - cold;
    const audioHeap = audioApi.getMemory().buffer, cpuHeap = cpuApi?.getMemory().buffer;
    const pcm = new Float64Array(request.seconds * 48000 * 2);
    const callbacks: number[] = [], services: number[] = [], ages: number[] = [], telemetry = [];
    const began = performance.now();
    let maximumLive = 0, maximumDivergence = 0;
    for (let block = 0; block < request.seconds*750; block++) {
      let start = performance.now();
      pcm.set(engine.render(1), block*128);
      callbacks.push(performance.now()-start);
      if (provider) {
        start = performance.now();
        if (await provider.work()) services.push(performance.now()-start);
      }
      const stats = engine.stats();
      ages.push(stats.field_age_ms);
      maximumLive = Math.max(maximumLive,stats.live_grains);
      maximumDivergence = Math.max(maximumDivergence,stats.rms_divergence);
      if ((block+1)%750 === 0) telemetry.push({second:(block+1)/750,...stats});
      if ((block+1)%3750 === 0) postMessage({type:"progress",...request,second:(block+1)/750});
    }
    const renderMs = performance.now()-began, stats = engine.stats();
    check(pcm.every(Number.isFinite) && pcm.some(v => Math.abs(v) > .0001), "Invalid or silent PCM");
    check(stats.voice_drops === 0 && stats.numeric_interventions === 0, "Dropped grains or numerical intervention");
    check(audioHeap === audioApi.getMemory().buffer && cpuHeap === cpuApi?.getMemory().buffer, "WASM heap grew");
    const report = provider instanceof GPUProvider ? provider.report() : undefined;
    if (report) check(report.gpuFields > 5 && report.cpuFields === 0 && report.rejected === 0, "GPU failed or substituted CPU");
    const commands = engine.bridge?.outgoing.stats(), fields = engine.bridge?.incoming.stats();
    check(!commands?.dropped && !fields?.dropped && !engine.bridge?.rejected, "Schedule lost commands/fields");
    return {pcm, record: {...request, grid:32, controls, source:"bellSource", seed:12345,
      sampleRate:48000, blockSize:64, prepareMs, providerPrepareMs, renderMs,
      callbackMs:distribution(callbacks), providerServiceMs:distribution(services),
      fieldAgeMs:distribution(ages), callbacksOverNominalPeriod:callbacks.filter(v => v > 64/48).length,
      underruns:null, maximumLive, maximumDivergence, stats, commands, fields, report, telemetry,
      heapUnchanged:true}};
  } finally {provider?.destroy();engine?.destroy();}
}
globalThis.onmessage = ({data}: MessageEvent<Request>) => {
  void render(data).then(({pcm,record}) =>
    globalThis.postMessage({type:"done",record,pcm}, {transfer:[pcm.buffer]}),
    error => postMessage({type:"error",message:error instanceof Error ? error.stack : String(error)}));
};
