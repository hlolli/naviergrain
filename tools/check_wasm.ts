/** Build with the installed workbench compiler; exercise its actual WASM host.
 * Run with Bun. No packages are installed or changed by this script. */
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { parseArgs } from "node:util";

const root = resolve(import.meta.dir, "..");
const { values } = parseArgs({ options: {
  workbench: { type: "string", default: resolve(root, "../csound-wasm-plugin-compiler") },
  csound: { type: "string", default: resolve(root, "../csound/build/csound") },
  module: { type: "string", default: resolve(root, "build/libnaviergrain.so") },
  output: { type: "string", default: resolve(root, "build/wasm") },
} });
const workbench = resolve(values.workbench!);
const output = resolve(values.output!);
await mkdir(output, { recursive: true });
const hash = (data: string | Uint8Array) => createHash("sha256").update(data).digest("hex");
const importFile = (path: string) => import(pathToFileURL(resolve(workbench, path)).href);
const { compilePlugin, initializeCompiler } = await importFile("src/compiler/compile.ts");
const { extractCsoundHeaders } = await importFile("src/compiler/sdk-archive.ts");
const sdk = await readFile(resolve(workbench, "node_modules/@csound/wasm-bin/lib/csound-plugin-sdk.tar.gz"));
const source = await readFile(resolve(root, "build/naviergrain.c"), "utf8");
await initializeCompiler(() => {});
const compiled = await compilePlugin(source, "c", extractCsoundHeaders(Bun.gunzipSync(sdk)));
await writeFile(resolve(output, "compiler.log"), compiled.output);
assert(compiled.ok && compiled.wasm, JSON.stringify(compiled));
const wasm: ArrayBuffer = compiled.wasm;
await writeFile(resolve(output, "naviergrain.wasm"), new Uint8Array(wasm));

interface BrowserApi {
  csoundCreate(): number;
  csoundCompileCSD(csound: number, csd: string): number;
  csoundStart(csound: number): number;
  csoundPerformKsmps(csound: number): number;
  csoundGetSpout(csound: number): number;
  csoundGetKsmps(csound: number): number;
  csoundReset(csound: number): void;
  csoundDestroy(csound: number): void;
  getMemory(): WebAssembly.Memory;
}
// Same headless entry used in the pinned workbench's tests. Assert the exact
// export boundary before adapting the browser bundle for Bun.
const entry = await readFile(resolve(workbench, "node_modules/@csound/browser/dist/csound.js"), "utf8");
const marker = "const Csound = kd; const libcsound = __lcs__; export { Csound, libcsound }; export default Csound;";
assert(entry.includes(marker), "Pinned browser entry changed; update the adapter explicitly");
Object.defineProperty(globalThis, "window", { value: {
  atob: globalThis.atob.bind(globalThis), btoa: globalThis.btoa.bind(globalThis),
  webkitAudioContext: undefined,
}, configurable: true });
const factory = new Function(entry.replace(marker, "return __lcs__;"))() as
  (options: { withPlugins: ArrayBuffer[] }) => Promise<BrowserApi>;
const api = await factory({ withPlugins: [wasm] });
const csound = api.csoundCreate();
const schema = await readFile(resolve(root, "include/naviergrain.inc"), "utf8");

function probe(block: number, sourceRatio: number): string {
  return `<CsoundSynthesizer>
<CsOptions>
-n -d -m0 --sample-accurate
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = ${block}
nchnls = 2
0dbfs = 1
${schema}
instr 1
iSource ftgen 0, 0, -997, 10, 1, .2, .1
iConfig[] fillarray $NG_CONFIG_DEFAULTS
iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
iConfig[$NG_CONFIG_SEED] = 0
kControl[] fillarray $NG_CONTROL_DEFAULTS
kControl[$NG_CONTROL_GRAIN_RATE] init 600
kControl[$NG_CONTROL_SCHEDULER] init 0
aL, aR, kStats[] naviergrain iSource, sr * ${sourceRatio}, iConfig, kControl
outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 0 .6
e
</CsScore>
</CsoundSynthesizer>
`;
}

function render(csd: string): Float64Array {
  assert.equal(api.csoundCompileCSD(csound, csd), 0);
  assert.equal(api.csoundStart(csound), 0);
  const block = api.csoundGetKsmps(csound);
  const samples: number[] = [];
  let ended = false;
  for (let calls = 0; calls < 30_000; calls++) {
    const status = api.csoundPerformKsmps(csound);
    assert(status >= 0);
    // Csound may return completion with the previous spout still present.
    if (status > 0 && samples.length >= 57600) { ended = true; break; }
    const audio = new Float64Array(api.getMemory().buffer, api.csoundGetSpout(csound), block * 2);
    for (const sample of audio) { assert(Number.isFinite(sample)); samples.push(sample); }
    if (status > 0) { ended = true; break; }
  }
  assert(ended, "WASM failed to reach score end");
  assert(samples.length >= 57600);
  assert(samples.some(x => Math.abs(x) > .0001));
  assert(samples.every(x => Math.abs(x) < 1), "Un-limited audio clipped");
  assert(samples.slice(57600).every(x => x === 0), "Inactive tail leaked audio");
  api.csoundReset(csound);
  return Float64Array.from(samples.slice(0, 57600));
}

function readFloatWav(bytes: Buffer): Float32Array {
  assert.equal(bytes.toString("ascii", 0, 4), "RIFF");
  let format = false;
  for (let offset = 12; offset + 8 <= bytes.length;) {
    const size = bytes.readUInt32LE(offset + 4), start = offset + 8;
    const id = bytes.toString("ascii", offset, offset + 4);
    if (id === "fmt ") {
      assert.equal(bytes.readUInt16LE(start), 3);
      assert.equal(bytes.readUInt16LE(start + 2), 2);
      assert.equal(bytes.readUInt32LE(start + 4), 48000);
      assert.equal(bytes.readUInt16LE(start + 14), 32);
      format = true;
    }
    if (id === "data") {
      assert(format);
      return Float32Array.from({ length: size / 4 }, (_, i) => bytes.readFloatLE(start + 4 * i));
    }
    offset = start + size + size % 2;
  }
  throw new Error("Native render has no WAV data");
}

const records: object[] = [];
try {
  for (const ratio of [.5, 1, 2]) {
    const reference = render(probe(32, ratio));
    assert.deepEqual(render(probe(32, ratio)), reference, "Reset/recompile changed seeded audio");
    assert.deepEqual(render(probe(37, ratio)), reference, "WASM audio depends on ksmps");
    const csdPath = resolve(output, `probe-${ratio}.csd`);
    await writeFile(csdPath, probe(32, ratio).replace("-n -d -m0", "-d -m0"));
    const wavPath = resolve(output, `native-${ratio}.wav`);
    const child = Bun.spawn([resolve(values.csound!), `--opcode-lib=${resolve(values.module!)}`,
      "-W", "-f", "-o", wavPath, csdPath], { stdout: "pipe", stderr: "pipe" });
    const [stdout, stderr, status] = await Promise.all([
      new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited]);
    await writeFile(resolve(output, `native-${ratio}.log`), stdout + stderr);
    assert.equal(status, 0, stderr);
    const native = readFloatWav(await readFile(wavPath));
    assert(native.length >= reference.length);
    let peakError = 0, errorPower = 0;
    for (let i = 0; i < reference.length; i++) {
      const error = Math.abs(reference[i]! - native[i]!);
      peakError = Math.max(peakError, error); errorPower += error * error;
    }
    assert(peakError < 2e-6, `Native/WASM disagreement: ${peakError}`);
    records.push({ sourceRatio: ratio, samples: reference.length, peakError,
      rmsError: Math.sqrt(errorPower / reference.length), resetsAndRenders: 3 });
    console.log(JSON.stringify(records.at(-1)));
  }
} finally {
  api.csoundDestroy(csound);
  Reflect.deleteProperty(globalThis, "window");
}
const packages: Record<string, string> = {};
for (const name of ["@csound/browser", "@csound/wasm-bin", "@yowasp/clang"])
  packages[name] = JSON.parse(await readFile(resolve(workbench, "node_modules", name, "package.json"), "utf8")).version;
await writeFile(resolve(output, "receipt.json"), JSON.stringify({
  scope: "Headless browser WASM engine under Bun; not AudioWorklet/UI qualification",
  packages, bun: Bun.version, sourceSha256: hash(source), sdkSha256: hash(sdk),
  browserEntrySha256: hash(entry), wasmSha256: hash(new Uint8Array(wasm)),
  nativeModuleSha256: hash(await readFile(resolve(values.module!))),
  wasmBytes: wasm.byteLength, compileMs: compiled.durationMs, records,
}, null, 2) + "\n");
