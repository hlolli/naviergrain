import {BufferedPreview} from './preview';
import {SharedRing, commandBytes, packetBytes, type BridgeSetup} from "./bridge";
import {Visualizer} from "./visualizer";
import schema from "../schema/naviergrain-v1.json";
import {bellSource, maxSourceFrames, sampleRate} from "./engine";
import {controlDefaults} from "./naviergrain-schema";

function element<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error("Missing element: " + id);
  return node as T;
}
const form = element<HTMLFormElement>("settings");
const status = element("status");
const renderButton = element<HTMLButtonElement>("render");
const pauseButton = element<HTMLButtonElement>("pause");
const cancelButton = element<HTMLButtonElement>("cancel");
const progress = element<HTMLProgressElement>("progress");
const audio = element<HTMLAudioElement>("audio");
const download = element<HTMLAnchorElement>("download");
const replayDownload = element<HTMLAnchorElement>("replay-download");
const recordReplay = element<HTMLInputElement>("record-replay");
const previewEnabled = element<HTMLInputElement>("preview-enabled");
const previewStatus = element("preview-status");
const previewResume = element<HTMLButtonElement>("preview-resume");
const previewStop = element<HTMLButtonElement>("preview-stop");
let preview: BufferedPreview | undefined;
let replayUrl: string | undefined;
const telemetry = element("telemetry");
const sourceLabel = element("source-label");
const controls: HTMLInputElement[] = [];
let source = bellSource();
let worker: Worker | undefined;
let solver: Worker | undefined;
const backend = element<HTMLSelectElement>("backend");
const externalAvailable = globalThis.isSecureContext && globalThis.crossOriginIsolated &&
  typeof SharedArrayBuffer !== "undefined";
backend.options[1]!.disabled = !externalAvailable;
backend.options[2]!.disabled = !externalAvailable;
function updateBackend() {
  const grains = backend.value === "grains";
  previewEnabled.disabled = !grains || !globalThis.isSecureContext || !("AudioWorkletNode" in globalThis);
  if (previewEnabled.disabled) previewEnabled.checked = false;
  recordReplay.disabled = !externalAvailable || !["external", "gpu"].includes(backend.value);
  if (recordReplay.disabled) recordReplay.checked = false;
  visualEnabled.disabled = grains || !visual;
  trailCount.disabled = grains; visualMode.disabled = grains;
  element("visual-panel").hidden = grains || !visualEnabled.checked;
  element("backend-detail").textContent = grains ?
    "GPU grain playback with CPU fluid motion. Falls back to CPU when needed. Offline only; replay files and flow view are unavailable in this mode." :
    externalAvailable ? "Internal CPU is repeatable. External solvers depend on worker scheduling. WebGPU fields include a prepared CPU fallback." :
    "Internal CPU and GPU grain playback are available without isolation. External solvers need a cross-origin-isolated page.";
  visual?.clear();
}
backend.addEventListener("change", updateBackend);
let paused = false;
let loading = false;
let renderUrl: string | undefined;
let generation = 0;
let viewPending = false;
const visualEnabled = element<HTMLInputElement>("visual-enabled");
const trailCount = element<HTMLSelectElement>("trail-count");
const visualMode = element<HTMLSelectElement>("visual-mode");
let visual: Visualizer | undefined;
try { visual = new Visualizer(element<HTMLCanvasElement>("flow"), element("visual-detail")); }
catch { visualEnabled.checked = false; visualEnabled.disabled = true; element("visual-detail").textContent = "Visuals unavailable; audio rendering still works."; }
updateBackend();
function drawView() { visual?.draw(Number(trailCount.value), visualMode.value); }
visualEnabled.addEventListener("change", () => {
  element("visual-panel").hidden = !visualEnabled.checked;
  visual?.clear();
});
trailCount.addEventListener("change", drawView);
visualMode.addEventListener("change", drawView);
document.addEventListener("visibilitychange", () => visual?.clear());
// One request in flight. A blocked/hidden UI cannot queue per-grain messages.
setInterval(() => {
  if (backend.value !== "grains" && worker && visual && visualEnabled.checked && !paused &&
      document.visibilityState === "visible" && !viewPending) {
    viewPending = true; worker.postMessage({type: "view"});
  }
}, 100);

schema.control.forEach((item, index) => {
  if (item.name === "reset") return; // Each render is a fresh, seeded instance.
  const label = document.createElement("label");
  label.textContent = item.name.replaceAll("_", " ");
  const input = document.createElement("input");
  input.type = "number"; input.name = item.name;
  input.min = String(item.min); input.max = String(item.max);
  input.step = item.integer ? "1" : "any";
  input.value = String(item.default); input.required = true;
  label.append(input);
  element("controls").append(label);
  controls[index] = input;
});
function busy(value: boolean) {
  for (const fieldset of form.querySelectorAll("fieldset")) fieldset.disabled = value;
  renderButton.disabled = value || loading;
  pauseButton.disabled = !value || !worker;
  cancelButton.disabled = !value || !worker;
}
function stop(keepPreview = false) {
  // Rendering can finish while a pause request is crossing worker boundaries.
  // Once render controls are disabled, allow the final queued tail to drain.
  if (keepPreview) preview?.pause(false);
  if (!keepPreview) { preview?.close(); preview = undefined; previewStop.disabled = true; previewResume.hidden = true; }
  generation++; viewPending = false;
  worker?.terminate(); worker = undefined;
  solver?.terminate(); solver = undefined; paused = false;
  pauseButton.textContent = "Pause render";
  busy(false);
}
function clearAudio() {
  audio.pause(); audio.removeAttribute("src"); audio.load();
  download.removeAttribute("href"); download.hidden = true;
  if (renderUrl) URL.revokeObjectURL(renderUrl);
  renderUrl = undefined;
  replayDownload.hidden = true; replayDownload.removeAttribute("href");
  if (replayUrl) URL.revokeObjectURL(replayUrl);
  replayUrl = undefined;
}
form.addEventListener("submit", async event => {
  event.preventDefault();
  if (!form.reportValidity() || loading) return;
  stop(); clearAudio(); visual?.clear();
  progress.value = 0;
  element("provider-detail").textContent = "";
  const id = generation;
  const settings = {
    source: source.slice(), sourceRate: sampleRate,
    controls: controlDefaults.map((value, index) => controls[index] ?
      Number(controls[index]!.value) : value),
  };
  const seconds = Number(element<HTMLInputElement>("seconds").value);
  let previewPort: MessagePort | undefined;
  previewStatus.textContent = "";
  previewResume.hidden = true;
  if (previewEnabled.checked && backend.value === "grains") {
    busy(true);
    const player = new BufferedPreview();
    preview = player;
    previewStop.disabled = false;
    previewStatus.textContent = "Preparing buffered preview…";
    try {
      previewPort = await player.start(data => {
        if (id !== generation && preview !== player) return;
        if (data.type === "context") {
          previewResume.hidden = !data.suspended;
          previewResume.disabled = false;
          if (data.suspended) previewStatus.textContent = "Audio suspended · Resume audio to continue the queued sound.";
          return;
        }
        if (data.type === "error") {
          previewStatus.textContent = data.message ?? "Preview stopped";
          // A stopped consumer cannot return credits: cancel its producer too.
          stop();
          status.textContent = "Preview failed. Turn preview off to render a WAV.";
        } else {
          previewStatus.textContent = (data.type === "ended" ? "Preview finished" : "Buffered preview") +
            " · " + ((data.played ?? 0) / sampleRate).toFixed(1) + " s played" +
            " · Rebuffers: " + (data.underruns ?? 0);
          if (data.type === "ended") { previewStop.disabled = true; previewResume.hidden = true; }
        }
      });
    } catch (error) {
      if (id !== generation) return;
      previewStatus.textContent = error instanceof Error ? error.message : String(error);
      preview = undefined; previewStop.disabled = true; previewResume.hidden = true;
      previewEnabled.checked = false;
      // Audio-device unavailability must not disable offline rendering.
    }
    if (id !== generation) { previewPort?.close(); player.close(); return; }
  }
  let setup: BridgeSetup | undefined;
  if (["external", "gpu"].includes(backend.value)) {
    if (!externalAvailable) { status.textContent = "External solvers are unavailable on this page."; return; }
    setup = {grid: 32, commands: new SharedRing(commandBytes).buffer,
      fields: new SharedRing(packetBytes(32)).buffer};
  }
  worker = new Worker(new URL(backend.value === "grains" ? "./grain.worker.js" : "./render.worker.js", import.meta.url), {type: "module"});
  busy(true); progress.value = 0; telemetry.textContent = "";
  status.textContent = "Preparing the instrument…";
  worker.onmessage = ({data}) => {
    if (id !== generation) return;
    if (data.type === "view") {
      viewPending = false;
      if (data.snapshot && visualEnabled.checked && document.visibilityState === "visible") {
        try { visual?.accept(data.snapshot); drawView(); }
        catch { visualEnabled.checked = false; element("visual-detail").textContent = "Visual snapshot unavailable; rendering continues."; }
      }
      return;
    }
    if (data.type === "provider") {
      element("provider-detail").textContent = data.mode === "gpu" ?
        "GPU grain playback · CPU recovery ready" : "CPU grain playback · fallback active";
      return;
    }
    if (data.grains) telemetry.textContent =
      "Peak sounding grains: " + data.grains.peakVoices +
      " · Dropped: " + data.grains.voice_drops +
      " · Interventions: " + data.grains.numeric_interventions +
      " · GPU deliveries: " + data.grains.gpuBatches +
      " · CPU deliveries: " + data.grains.cpuBatches +
      " · Peak: " + Number(data.peak ?? 0).toFixed(4);
    if (data.type === "error") { status.textContent = data.message; stop(); return; }
    if (data.type === "paused") { status.textContent = "Render paused."; return; }
    if (data.type === "ready") status.textContent = paused ? "Render paused." : "Rendering…";
    if (data.stats) telemetry.textContent =
      "Sounding grains: " + data.stats.live_grains +
      " · Silent emitters: " + data.stats.emitter_count +
      " · Dropped: " + data.stats.voice_drops +
      " · Interventions: " + data.stats.numeric_interventions +
      " · Field age: " + Number(data.stats.field_age_ms).toFixed(1) + " ms" +
      (data.bridge ? " · Transport drops: " +
        (data.bridge.commands.dropped + data.bridge.fields.dropped) : "") +
      " · RMS divergence: " + Number(data.stats.rms_divergence).toExponential(2) +
      " · Peak: " + Number(data.peak ?? 0).toFixed(4);
    if (data.type === "finalizing") status.textContent = "Saving replay…";
    if (data.type === "progress") progress.value = data.fraction;
    if (data.type === "done") {
      renderUrl = URL.createObjectURL(new Blob([data.wav], {type: "audio/wav"}));
      audio.src = renderUrl; download.href = renderUrl; download.hidden = false;
      if (data.replay) {
        replayUrl = URL.createObjectURL(new Blob([data.replay], {type: "application/json"}));
        replayDownload.href = replayUrl; replayDownload.hidden = false;
      }
      progress.value = 1;
      status.textContent = "Ready to play · " + seconds + " seconds rendered in " +
        (data.renderMs / 1000).toFixed(1) + " seconds." +
        (data.peak > 1 ? " Peak exceeds full scale; reduce gain and render again." : "");
      stop(true);
    }
  };
  worker.onerror = event => { status.textContent = event.message || "Render worker failed."; stop(); };
  let solverReady = false;
  const startAudio = () => {
    solverReady = true;
    if (id !== generation) return;
    const transfer: Transferable[] = [settings.source.buffer];
    if (previewPort) transfer.push(previewPort);
    worker?.postMessage({type: "start", seconds, settings, setup, record: recordReplay.checked, previewPort}, transfer);
  };
  if (setup) {
    solver = new Worker(new URL("./field.worker.js", import.meta.url), {type: "module"});
    solver.onmessage = ({data}) => {
      if (id !== generation) return;
      if (data.type === "provider" || data.provider) {
        const provider = data.provider ?? data;
        element("provider-detail").textContent = provider.backend === "gpu" ?
          "WebGPU fields · CPU standby ready" :
          "CPU fallback active" + (provider.reason ? " · " + provider.reason : "");
      }
      if (data.type === "ready") startAudio();
      if (data.type === "error") {
        // Do not substitute a different backend. If audio is running, its
        // stale-field damping keeps grains playing while the provider is lost.
        status.textContent = solverReady ? "Solver stopped; existing grains continue with fading motion." : data.message;
        if (!solverReady) stop();
      }
    };
    solver.onerror = () => {
      status.textContent = solverReady ? "Solver worker lost; grains continue with fading motion." : "Solver worker failed.";
      if (!solverReady) stop();
    };
    solver.postMessage({type: "start", setup, backend: backend.value});
  } else startAudio();
});
pauseButton.addEventListener("click", () => {
  paused = !paused;
  preview?.pause(paused);
  worker?.postMessage({type: paused ? "pause" : "resume"});
  pauseButton.textContent = paused ? "Resume render" : "Pause render";
  status.textContent = paused ? "Pausing…" : "Rendering…";
});
previewResume.addEventListener("click", async () => {
  const player = preview;
  previewResume.disabled = true;
  try { await player?.resume(); }
  catch {
    if (preview === player) previewStatus.textContent = "Audio could not resume. Try Resume audio again, or stop preview.";
  } finally {
    if (preview === player) previewResume.disabled = false;
  }
});
previewStop.addEventListener("click", () => {
  const rendering = Boolean(worker);
  stop();
  previewStatus.textContent = "Preview stopped.";
  if (rendering) status.textContent = "Render cancelled.";
});
cancelButton.addEventListener("click", () => { stop(); status.textContent = "Render cancelled."; });
element<HTMLButtonElement>("defaults").addEventListener("click", () => {
  controls.forEach((input, i) => { input.value = String(controlDefaults[i]); });
});
element<HTMLButtonElement>("bell").addEventListener("click", () => {
  source = bellSource(); sourceLabel.textContent = "Built-in bell · 1 second, 48 kHz";
  element<HTMLInputElement>("source").value = "";
});
element<HTMLInputElement>("source").addEventListener("change", async event => {
  const file = (event.target as HTMLInputElement).files?.[0];
  if (!file) return;
  loading = true; busy(true);
  try {
    if (file.size > 32 * 1024 * 1024) throw new Error("Choose an audio file smaller than 32 MiB");
    const context = new OfflineAudioContext(1, 1, sampleRate);
    const decoded = await context.decodeAudioData(await file.arrayBuffer());
    if (decoded.length < 4 || decoded.length > maxSourceFrames)
      throw new Error("Choose a source between 4 samples and 30 seconds");
    const next = Float64Array.from(decoded.getChannelData(0));
    if (!next.every(Number.isFinite)) throw new Error("Source contains non-finite samples");
    source = next;
    sourceLabel.textContent = file.name + " · first channel · decoded to 48 kHz";
    status.textContent = "Source loaded locally.";
  } catch (error) {
    status.textContent = error instanceof Error ? error.message : String(error);
    element<HTMLInputElement>("source").value = "";
  } finally { loading = false; busy(false); }
});
window.addEventListener("pagehide", () => { stop(); clearAudio(); });
