/** Native C vs WebGPU fixed grain snapshots. GPU is never substituted by CPU. */
import assert from 'node:assert/strict';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseArgs} from 'node:util';
import {createHash} from 'node:crypto';
import {cpus} from 'node:os';
const {values} = parseArgs({options: {playwright: {type: 'string'}, browser: {type: 'string'},
  url: {type: 'string', default: 'http://127.0.0.1:8781'},
  input: {type: 'string', default: 'build/grain-comparison-input'},
  output: {type: 'string'}, 'hardware-vulkan': {type: 'boolean', default: false},
  reverse: {type: 'boolean', default: false}}});
assert(values.playwright && values.browser && values.output);
await mkdir(values.output); // refuse existing experiment directories
const receipt = {status: 'running', started: new Date().toISOString(), rows: [], hashes: {},
  cpu: cpus()[0].model, scope: 'Isolated fixed-grain convolution/window/pan/stereo mix; no solver or live audio',
  constraints: 'Looping 1021-sample source, constant dyadic pitches/pans, 8193-sample Hann, fixed gain; no births, smoothing, overlap control or particle updates. Native single-thread C double vs GPU float32. Warm repeated snapshots. GPU and prepared C cache identical band choices; also measure the unmodified production reader.',
  samplesPerCase: 7, thresholds: {maxAbsoluteError: 1e-5, relativeRmsError: 1e-4}, reverse: !!values.reverse};
const path = resolve(values.output, 'receipt.json');
const save = () => writeFile(path, JSON.stringify(receipt, null, 2) + '\n');
await save();let browser;
try {
  for (const file of ['tools/grain_benchmark.c', 'tools/grain_benchmark.wgsl', 'tools/grain_benchmark.worker.js',
    'tools/compare_grains.mjs', 'src/fluidgrain_resampler.c', 'src/fluidgrain_resampler.h',
    'build/grain_benchmark', ...['cpu.json', 'input.f32', 'voices.f32'].map(n => `${values.input}/${n}`)]) {
    const content = await readFile(file);
    receipt.hashes[file] = createHash('sha256').update(content).digest('hex');
    const snapshot = resolve(values.output, 'sources', file);
    await mkdir(resolve(snapshot, '..'), {recursive: true});
    await writeFile(snapshot, content, {flag: 'wx'});
  }
  const {chromium} = await import(pathToFileURL(resolve(values.playwright)).href);
  const args = ['--no-sandbox', '--enable-unsafe-webgpu', ...(values['hardware-vulkan'] ?
    ['--use-angle=vulkan', '--enable-features=Vulkan', '--disable-vulkan-surface'] : ['--use-angle=swiftshader'])];
  browser = await chromium.launch({executablePath: values.browser, headless: true, args});
  receipt.browser = browser.version();receipt.args = args;
  const page = await browser.newPage();
  let pending = Promise.resolve();
  await page.exposeFunction('checkpoint', data => {
    pending = pending.then(async () => {
      const {result, pcm} = data;
      const bytes = Buffer.from(new Float32Array(pcm).buffer);
      const name = `gpu-${result.voices}-${result.frames}.f32`;
      await writeFile(resolve(values.output, name), bytes, {flag: 'wx'});
      result.pcmSha256 = createHash('sha256').update(bytes).digest('hex');
      const refName = `reference-${result.voices}-${result.frames}.f64`;
      const ref = await readFile(resolve(values.input, refName));
      await writeFile(resolve(values.output, refName), ref, {flag: 'wx'});
      result.referenceSha256 = createHash('sha256').update(ref).digest('hex');
      receipt.rows.push(result);await save();console.log(JSON.stringify(result));
    });return pending;
  });
  await page.goto(values.url);
  const result = await page.evaluate(request => new Promise((resolve, reject) => {
    const worker = new Worker('./grain_benchmark.worker.js', {type: 'module'});
    const timeout = setTimeout(() => {worker.terminate();reject(new Error('GPU grain benchmark timed out'));}, 240000);
    const close = () => {clearTimeout(timeout);worker.terminate();};
    worker.onerror = e => {close();reject(new Error(e.message));};
    worker.onmessage = ({data}) => {
      if (data.type === 'row') {window.checkpoint({result: data.result, pcm: Array.from(data.pcm)}).catch(reject);return;}
      close();if (data.type === 'error') reject(new Error(data.message));else resolve(data.result);
    };
    worker.postMessage(request);
  }), {hardware: !!values['hardware-vulkan'], reverse: !!values.reverse});
  await pending;
  assert.equal(receipt.rows.length, 12);
  const {rows, ...metadata} = result;Object.assign(receipt, metadata);
  await browser.close();browser = null;
  receipt.status = 'complete';await save();
} catch (error) {
  receipt.status = 'failed';receipt.error = String(error.stack || error);await save();throw error;
} finally {await browser?.close();}
