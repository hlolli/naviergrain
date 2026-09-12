/** Offline real-scheduler grain playback. One owned 512-frame plan at a time;
 * GPU failure permanently selects the same host's exact C/WASM renderer. */
import {previewSlots} from './preview-queue';
import {GrainPlanHost} from './grain-plan';
import {GrainRenderer} from './grain-renderer';
import {sampleRate, maxRenderSeconds, encodeWav, type CsoundApi, type Settings} from './engine';

Object.defineProperty(globalThis, 'window', {value: {
  atob: globalThis.atob.bind(globalThis), btoa: globalThis.btoa.bind(globalThis),
  webkitAudioContext: undefined,
}});
const deliveryFrames = 512;
let renderer: GrainRenderer | undefined;
let started = false;
let paused = false;
let pumping = false;
let pcm: Float64Array;
let offset = 0;
let peak = 0;
let startedAt = 0;
let lastTelemetry = 0;
let gpuBatches = 0;
let cpuBatches = 0;
let peakVoices = 0;
// Constant-space totals; exclude preparation, pauses and timer scheduling from
// measured processing cost. No per-delivery trace retained by the workbench.
const processing = {captureMs: 0, playbackMs: 0, commitMs: 0, maximumDeliveryMs: 0,
  packedBytes: 0, cpuPackedBytes: 0, audioMs: 0, overBudgetDeliveries: 0,
  maximumBudgetRatio: 0, cpuProcessingMs: 0, cpuAudioMs: 0};
let reportedMode: string | undefined;
let previewPort: MessagePort | undefined;
let previewCredits = previewSlots;
const post = (data: unknown) => globalThis.postMessage(data);
function provider() {
  if (!renderer || reportedMode === renderer.mode) return;
  reportedMode = renderer.mode;
  // Keep low-level device errors out of the sound controls; retain them in
  // the completion metadata for diagnostics.
  post({type: 'provider', mode: renderer.mode});
}
function fail(error: unknown) {
  renderer?.destroy(); renderer = undefined;
  post({type: 'error', message: error instanceof Error ? error.message : String(error)});
}
async function pump(): Promise<void> {
  if (paused || pumping || !renderer || (previewPort && !previewCredits)) return;
  pumping = true;
  if (previewPort) previewCredits--;
  try {
    const output = await renderer.render(Math.min(deliveryFrames, (pcm.length - offset) / 2));
    if (output.start !== BigInt(offset / 2)) throw new Error('Grain delivery clock mismatch');
    pcm.set(output.pcm, offset);
    offset += output.pcm.length;
    if (previewPort) {
      const preview = Float32Array.from(output.pcm);
      previewPort.postMessage({start: Number(output.start), pcm: preview,
        final: offset === pcm.length}, [preview.buffer]);
    }
    for (const sample of output.pcm) peak = Math.max(peak, Math.abs(sample));
    peakVoices = Math.max(peakVoices, output.peakVoices);
    if (output.mode === 'gpu') gpuBatches++; else cpuBatches++;
    processing.captureMs += output.captureMs;
    processing.playbackMs += output.playbackMs;
    processing.commitMs += output.commitMs;
    const deliveryMs = output.captureMs + output.playbackMs + output.commitMs;
    const audioMs = output.pcm.length / 2 / sampleRate * 1000;
    processing.maximumDeliveryMs = Math.max(processing.maximumDeliveryMs, deliveryMs);
    processing.audioMs += audioMs;
    if (deliveryMs > audioMs) processing.overBudgetDeliveries++;
    processing.maximumBudgetRatio = Math.max(processing.maximumBudgetRatio, deliveryMs / audioMs);
    if (output.mode === 'cpu') {
      processing.cpuProcessingMs += deliveryMs;
      processing.cpuAudioMs += audioMs;
    }
    processing.packedBytes += output.bytes;
    if (output.mode === 'cpu') processing.cpuPackedBytes += output.bytes;
    provider();
    const grains = {gpuBatches, cpuBatches, peakVoices, ...renderer.host.diagnostics()};
    if (offset === pcm.length) {
      const wav = encodeWav(pcm);
      const fallbackReason = renderer.failureReason;
      renderer.destroy(); renderer = undefined;
      globalThis.postMessage({type: 'done', wav, peak, grains, fallbackReason,
        renderMs: performance.now() - startedAt, processing}, {transfer: [wav]});
    } else if (performance.now() - lastTelemetry >= 100) {
      lastTelemetry = performance.now();
      post({type: 'progress', fraction: offset / pcm.length, peak, grains});
    }
  } catch (error) { fail(error); }
  finally {
    pumping = false;
    if (renderer) {
      // A pause finishes at the owned delivery boundary, never half a batch.
      if (paused) post({type: 'paused'});
      else if (!previewPort || previewCredits) setTimeout(() => { void pump(); }, 0);
    }
  }
}

globalThis.onmessage = async ({data}: MessageEvent<
  {type: 'start'; settings: Settings; seconds: number; previewPort?: MessagePort} |
  {type: 'pause' | 'resume' | 'view'}
>) => {
  try {
    if (data.type === 'view') { post({type: 'view', snapshot: null}); return; }
    if (data.type === 'pause') { paused = true; if (!pumping) post({type: 'paused'}); return; }
    if (data.type === 'resume') { paused = false; void pump(); return; }
    if (data.type !== 'start') return;
    if (started) throw new Error('Create a fresh worker for each render');
    started = true;
    previewPort = data.previewPort;
    if (previewPort) {
      previewPort.onmessage = ({data}) => {
        if (data.type !== 'credit') return;
        previewCredits = Math.min(previewSlots, previewCredits + 1);
        void pump();
      };
      previewPort.start();
    }
    if (!Number.isFinite(data.seconds) || data.seconds < .1 || data.seconds > maxRenderSeconds)
      throw new Error('Render duration must be 0.1–30 seconds');
    startedAt = performance.now();
    const {libcsound} = await import(new URL('./csound.js', import.meta.url).href) as {
      libcsound(options: {withPlugins: ArrayBuffer[]}): Promise<CsoundApi>
    };
    const response = await fetch(new URL('./fluidgrain.wasm', import.meta.url));
    if (!response.ok) throw new Error('Missing instrument assets');
    const api = await libcsound({withPlugins: [await response.arrayBuffer()]});
    // These failures select C playback; they must not prevent a take.
    let adapter: GPUAdapter | null = null;
    let shader = '';
    try {
      const response = await fetch(new URL('./grain-plan.wgsl', import.meta.url));
      if (response.ok) shader = await response.text();
      adapter = await navigator.gpu?.requestAdapter() ?? null;
    } catch { /* Prepared C/WASM playback remains available. */ }
    const host = new GrainPlanHost(api, data.settings, undefined, deliveryFrames);
    try { renderer = await GrainRenderer.create(host, adapter, shader); }
    catch (error) { host.destroy(); throw error; }
    pcm = new Float64Array(Math.round(data.seconds * sampleRate) * 2);
    provider();
    post({type: 'ready', blockSize: deliveryFrames});
    void pump();
  } catch (error) { fail(error); }
};
