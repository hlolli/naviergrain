import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import {resolve} from "node:path";
import {OfflineEngine, bellSource, encodeWav, type CsoundApi, type Settings} from "../web/engine";
import {validateView} from "../web/visualizer";
import {controlDefaults} from "../web/fluidgrain-schema";

const root = resolve(import.meta.dir, "..");
const entry = await readFile(resolve(root,
  "../csound-wasm-plugin-compiler/node_modules/@csound/browser/dist/csound.js"), "utf8");
const marker = "const Csound = kd; const libcsound = __lcs__; export { Csound, libcsound }; export default Csound;";
assert(entry.includes(marker));
Object.defineProperty(globalThis, "window", {value: {
  atob: globalThis.atob.bind(globalThis), btoa: globalThis.btoa.bind(globalThis),
  webkitAudioContext: undefined,
}, configurable: true});
const factory = new Function(entry.replace(marker, "return __lcs__;"))() as
  (options: {withPlugins: ArrayBuffer[]}) => Promise<CsoundApi>;
const plugin = await readFile(resolve(root, "build/wasm/fluidgrain.wasm"));
const api = await factory({withPlugins: [plugin.slice().buffer]});
const includes = await readFile(resolve(root, "build/web/prepared.inc"), "utf8");
const settings = (): Settings => ({
  source: bellSource(), sourceRate: 48000, controls: [...controlDefaults],
});
let renders = 0;
function render(sourceRate: number, withPause: boolean, reset = false, observed = false): Float64Array {
  const input = settings(); input.sourceRate = sourceRate;
  const engine = new OfflineEngine(api, input, includes, observed);
  const chunks: Float64Array[] = [];
  try {
    input.source.fill(0); // The plugin must own its original source copy.
    const prepared = engine.stats();
    engine.silence(withPause ? 31 : 1);
    assert.deepEqual(engine.stats(), prepared);
    for (let i = 0; i < 24; i++) {
      if (reset) engine.reset(i === 8);
      const stats = engine.stats();
      if (withPause) {
        engine.silence(7);
        assert.deepEqual(engine.stats(), stats);
      }
      chunks.push(engine.render(8, observed && i % 2 === 0));
      if (observed && i % 2 === 0) {
        const before = engine.stats();
        const view = engine.view(); validateView(view);
        assert.deepEqual(engine.view(), view);
        assert.deepEqual(engine.stats(), before);
        assert(view[8]! <= before.live_grains);
        assert.equal(view[10], 32);
        // Mutating the host copy must not touch the C table or engine.
        view.fill(NaN); validateView(engine.view());
      }
    }
  } finally { engine.destroy(); engine.destroy(); }
  const pcm = new Float64Array(chunks.length * chunks[0]!.length);
  chunks.forEach((chunk, i) => pcm.set(chunk, i * chunk.length));
  assert(pcm.some(value => Math.abs(value) > .0001));
  assert(pcm.every(Number.isFinite));
  renders++;
  return pcm;
}
try {
  for (const sourceRate of [24000, 48000, 96000]) {
    const reference = render(sourceRate, false);
    assert.deepEqual(render(sourceRate, true), reference, "Pause/pre-roll changed audio");
    assert.deepEqual(render(sourceRate, false), reference, "Restart changed seeded audio");
  }
  assert.deepEqual(render(48000, true, true), render(48000, false, true),
    "Pausing during reset changed audio");
  assert.deepEqual(render(48000, true, true, true), render(48000, true, true),
    "Observation changed audio across reset and pauses");
  assert.deepEqual(render(48000, false, false, true), render(48000, false),
    "Observation changed ordinary seeded audio");
  // Enough long-lived grains and copies to exercise host table-copy growth.
  const dense = settings(); dense.controls[1] = 600; dense.controls[2] = 500;
  const denseRender = (observed: boolean) => {
    const engine = new OfflineEngine(api, dense, includes, observed);
    const chunks = [];
    try {
      for (let i = 0; i < 188; i++) {
        chunks.push(engine.render(8, observed));
        if (observed) validateView(engine.view());
      }
    } finally { engine.destroy(); }
    renders++;
    return chunks;
  };
  assert.deepEqual(denseRender(true), denseRender(false), "Dense observation changed PCM");
  for (const invalid of [
    {...settings(), source: new Float64Array([0, 1, NaN, 0])},
    {...settings(), sourceRate: Infinity},
    {...settings(), controls: [0]},
  ]) assert.throws(() => new OfflineEngine(api, invalid, includes));
  assert.throws(() => new OfflineEngine(api, settings(), "invalid orchestra text"),
    /compilation failed|start failed/);
  // Failed creation is followed by an audible healthy instance.
  const pcm = render(48000, false);
  for (const samples of [[0], [0, NaN], [0, Infinity], [0, 1e39]])
    assert.throws(() => encodeWav(Float64Array.from(samples)));
  const wav = new DataView(encodeWav(pcm));
  assert.equal(wav.getUint16(20, true), 3);
  assert.equal(wav.getUint32(40, true), pcm.length * 4);
  pcm.forEach((sample, i) => assert.equal(wav.getFloat32(44 + i * 4, true), Math.fround(sample)));
  console.log(JSON.stringify({renders, exactPauseRestartAndReset: true,
    sourceOwnership: true, sourceRates: [24000, 48000, 96000], floatWav: true}));
} finally { Reflect.deleteProperty(globalThis, "window"); }
