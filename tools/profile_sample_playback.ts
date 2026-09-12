/** Short paired CPU-fallback probe; no browser/GPU/device timing claim.
 * bun tools/profile_sample_playback.ts --before path/to/old.wasm
 */
import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {mkdir, readFile, writeFile} from "node:fs/promises";
import {resolve} from "node:path";
import {parseArgs} from "node:util";
import {GrainPlanHost} from "../web/grain-plan";
import {bellSource, type CsoundApi} from "../web/engine";
import {control, controlDefaults} from "../web/fluidgrain-schema";

const root = resolve(import.meta.dir, "..");
const {values} = parseArgs({options: {
  before: {type: "string"},
  after: {type: "string", default: resolve(root, "build/wasm/fluidgrain.wasm")},
  output: {type: "string", default: resolve(root, "build/sample-playback")},
  seconds: {type: "string", default: "2"},
}});
assert(values.before, "--before must identify the retained baseline module");
const seconds = Number(values.seconds);
assert(Number.isFinite(seconds) && seconds >= 1 && seconds <= 30);
const frames = Math.round(seconds * 48000);
const output = resolve(values.output!);
await mkdir(output, {recursive: true});
const hash = (data: Uint8Array | string) => createHash("sha256").update(data).digest("hex");
const entry = await readFile(resolve(root,
  "../csound-wasm-plugin-compiler/node_modules/@csound/browser/dist/csound.js"), "utf8");
const marker = "const Csound = kd; const libcsound = __lcs__; export { Csound, libcsound }; export default Csound;";
assert(entry.includes(marker), "Pinned host entry changed");
Object.defineProperty(globalThis, "window", {value: {atob, btoa, webkitAudioContext: undefined}, configurable: true});
const factory = new Function(entry.replace(marker, "return __lcs__;"))() as
  (options: {withPlugins: ArrayBuffer[]}) => Promise<CsoundApi>;
const controls = [...controlDefaults];
controls[control.grain_rate] = 2000;
controls[control.grain_ms] = 200;
controls[control.speed_to_density] = 0;
controls[control.strain_to_duration] = 0;
controls[control.gain] = .15;

async function render(path: string) {
  const plugin = await readFile(path);
  const host = new GrainPlanHost(await factory({withPlugins: [plugin.slice().buffer]}),
    {source: bellSource(), sourceRate: 48000, controls}, undefined, 512);
  const audio = new Float64Array(frames * 2);
  let captureMs = 0, playbackMs = 0, commitMs = 0, peakVoices = 0, deliveries = 0;
  try {
    // Warm both code and the grain population, outside the measured interval.
    for (let i = 0; i < 48; ++i) {
      const batch = host.capture(512, false);
      host.commit(batch, host.fallback(batch));
    }
    for (let offset = 0; offset < frames; offset += 512) {
      const start = performance.now();
      const batch = host.capture(Math.min(512, frames - offset), false);
      const captured = performance.now();
      const pcm = host.fallback(batch);
      const rendered = performance.now();
      const committed = host.commit(batch, pcm);
      const end = performance.now();
      captureMs += captured - start;
      playbackMs += rendered - captured;
      commitMs += end - rendered;
      assert.equal(batch.packet.length, 0, "CPU continuation packed a GPU packet");
      audio.set(committed, offset * 2);
      peakVoices = Math.max(peakVoices, batch.peakVoices);
      ++deliveries;
    }
    assert(peakVoices > 350);
    assert(audio.every(x => Number.isFinite(x) && Math.abs(x) < 1));
    const diagnostics = host.diagnostics();
    assert.deepEqual(diagnostics, {voice_drops: 0, cap_drops: 0, numeric_interventions: 0});
    const record = {wasmSha256: hash(plugin), frames, deliveries, peakVoices,
      captureMs, playbackMs, commitMs, diagnostics,
      pcmSha256: hash(new Uint8Array(audio.buffer))};
    console.log(JSON.stringify(record));
    return {audio, record};
  } finally {
    host.destroy();
  }
}
try {
  const before = await render(resolve(values.before!));
  const after = await render(resolve(values.after!));
  let maximumError = 0, errorPower = 0;
  for (let i = 0; i < before.audio.length; ++i) {
    const error = after.audio[i]! - before.audio[i]!;
    maximumError = Math.max(maximumError, Math.abs(error));
    errorPower += error * error;
  }
  assert(maximumError < 2e-12, "Core change altered dense audio beyond rounding tolerance");
  assert.equal(after.record.peakVoices, before.record.peakVoices);
  const receipt = {status: "complete",
    scope: "One sequential paired Bun/WASM CPU-fallback probe; not sustained capacity, Chromium or device timing",
    bun: Bun.version, hostSha256: hash(entry), controls, warmupFrames: 48 * 512,
    before: before.record, after: after.record, maximumError,
    rmsError: Math.sqrt(errorPower / before.audio.length),
    captureReductionPercent: 100 * (1 - after.record.captureMs / before.record.captureMs),
    playbackReductionPercent: 100 * (1 - after.record.playbackMs / before.record.playbackMs)};
  await writeFile(resolve(output, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n");
  console.log(JSON.stringify(receipt));
} catch (error) {
  await writeFile(resolve(output, "receipt.json"), JSON.stringify({
    status: "failed", error: String(error),
  }, null, 2) + "\n");
  throw error;
} finally {
  Reflect.deleteProperty(globalThis, "window");
}
