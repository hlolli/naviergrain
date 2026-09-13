import assert from "node:assert/strict";
import {readFile, writeFile} from "node:fs/promises";
import {resolve} from "node:path";
import {SharedRing, commandBytes, packetBytes, SolverEngine, type BridgeSetup} from "../web/bridge";
import {OfflineEngine, bellSource, browserConfig, type CsoundApi} from "../web/engine";
import {controlDefaults, control, NG_STATUS_PROVIDER_LOST, NG_STATUS_STALE_FIELD} from "../web/naviergrain-schema";

const root = resolve(import.meta.dir, "..");
const entry = await readFile(resolve(root, "../csound-wasm-plugin-compiler/node_modules/@csound/browser/dist/csound.js"), "utf8");
const marker = "const Csound = kd; const libcsound = __lcs__; export { Csound, libcsound }; export default Csound;";
assert(entry.includes(marker));
Object.defineProperty(globalThis, "window", {value: {
  atob: globalThis.atob.bind(globalThis), btoa: globalThis.btoa.bind(globalThis), webkitAudioContext: undefined,
}, configurable: true});
const factory = new Function(entry.replace(marker, "return __lcs__;"))() as
  (options: {withPlugins: ArrayBuffer[]}) => Promise<CsoundApi>;
const plugin = await readFile(resolve(root, "build/wasm/naviergrain.wasm"));
const create = () => factory({withPlugins: [plugin.slice().buffer]});
const [audioApi, solverApi] = await Promise.all([create(), create()]);
assert.notEqual(audioApi.getMemory(), solverApi.getMemory());
assert(audioApi.getMemory().buffer instanceof ArrayBuffer);
const includes = await readFile(resolve(root, "build/web/prepared.inc"), "utf8");
function setup(grid = 32): BridgeSetup {
  return {grid, commands: new SharedRing(commandBytes).buffer,
    fields: new SharedRing(packetBytes(grid)).buffer};
}
function ringChecks() {
  const producer = new SharedRing(commandBytes);
  const consumer = new SharedRing(commandBytes, producer.buffer);
  const source = new Uint8Array(commandBytes), output = new Uint8Array(commandBytes);
  for (let i = 1; i <= 4; i++) { source.fill(i); assert(producer.push(source)); }
  source.fill(9); assert(!producer.push(source));
  for (let i = 1; i <= 4; i++) { assert(consumer.pop(output)); assert(output.every(v => v === i)); }
  assert(!consumer.pop(output)); assert(producer.push(source)); assert(consumer.pop(output));
  assert.deepEqual(consumer.stats(), {published: 5, dropped: 1, consumed: 5});
  assert.throws(() => producer.push(new Uint8Array(4)));
  assert.throws(() => new SharedRing(12, producer.buffer));
  producer.stop(); assert(consumer.stopped);
}
const records: object[] = [];
function render({external = true, mix = 1, stall = false, pause = false, reset = false, observed = false, grid = 32, seconds = 1} = {}) {
  const queues = setup(grid);
  const settings = {source: bellSource(), sourceRate: 48000, controls: [...controlDefaults]};
  settings.controls[control.mapping_mix] = mix;
  settings.controls[control.scheduler] = 0;
  const engine = new OfflineEngine(audioApi, settings, includes, observed, external ? queues : undefined);
  const solver = external ? new SolverEngine(solverApi, browserConfig(true, grid), queues) : undefined;
  const audioHeap = audioApi.getMemory().buffer, solverHeap = solverApi.getMemory().buffer;
  const chunks = [];
  let lost = false, recovered = false, stale = false, maximumAge = 0;
  let copyMs = 0, maxCopyMs = 0, copyCalls = 0;
  try {
    engine.silence(5);
    for (let block = 0; block < 750 * seconds; block++) {
      if (pause && block === 440) {
        const before = engine.stats();
        engine.silence(9); assert.deepEqual(engine.stats(), before);
        engine.resumeExternal();
      }
      if (reset) engine.reset(block === 600);
      chunks.push(engine.render(1, observed && block % 16 === 0));
      if (observed && block % 16 === 0) engine.view();
      if (solver && !(stall && block >= 100 && block < 440)) solver.work();
      const stats = engine.stats();
      lost ||= Boolean(stats.status & NG_STATUS_PROVIDER_LOST);
      stale ||= Boolean(stats.status & NG_STATUS_STALE_FIELD);
      recovered ||= lost && block > 480 && !(stats.status & NG_STATUS_STALE_FIELD);
      maximumAge = Math.max(maximumAge, stats.field_age_ms);
      // Time the bounded host copy alone, excluding render/solve/channel access.
      if (engine.bridge) {
        const start = performance.now(); engine.bridge.before(); engine.bridge.after();
        const elapsed = performance.now() - start; copyMs += elapsed;
        maxCopyMs = Math.max(maxCopyMs, elapsed); copyCalls++;
      }
    }
    if (external) {
      assert(engine.stats().snapshot_sequence > 5, "External field never drove audio");
      assert.equal(engine.stats().backend, 1);
      assert.equal(engine.bridge!.rejected, 0);
      assert.equal(solver!.mailbox.rejected, 0);
      if (stall) { assert(lost && stale && recovered); assert(maximumAge > 350); }
    }
    assert.equal(audioApi.getMemory().buffer, audioHeap);
    assert.equal(solverApi.getMemory().buffer, solverHeap);
    records.push({external, grid, seconds, mix, stall, pause, reset, observed, lost, recovered,
      maximumAge, fields: engine.stats().snapshot_sequence,
      commands: new SharedRing(commandBytes, queues.commands).stats(),
      fieldQueue: new SharedRing(packetBytes(grid), queues.fields).stats(),
      hostCopyMeanMs: copyMs / Math.max(1, copyCalls), hostCopyMaxMs: maxCopyMs});
  } finally { solver?.destroy(); engine.destroy(); }
  const result = new Float64Array(chunks.length * 128);
  chunks.forEach((chunk, i) => result.set(chunk, i * 128));
  assert(result.some(v => Math.abs(v) > .0001)); assert(result.every(Number.isFinite));
  return result;
}
try {
  ringChecks();
  assert.deepEqual(render({mix: 0}), render({external: false, mix: 0}));
  const baseline = render();
  assert.deepEqual(render(), baseline, "Identical accepted schedule changed PCM");
  assert.deepEqual(render({observed: true}), baseline, "Visuals changed accepted-schedule PCM");
  assert.notDeepEqual(baseline, render({mix: 0}));
  assert.deepEqual(render({stall: true, pause: true, reset: true}),
    render({stall: true, pause: true, reset: true}), "Recovery is not replayable");
  render({seconds: 30});
  render({external: false, seconds: 30});
  for (const grid of [16, 64]) render({grid, reset: true});
  console.log(JSON.stringify({renders: records.length, distinctHeaps: true, records}));
  await writeFile(resolve(root, "build/browser-bridge-engine.json"),
    JSON.stringify({renders: records.length, distinctHeaps: true, records}, null, 2) + "\n");
} finally { Reflect.deleteProperty(globalThis, "window"); }
