/** Focused UI integration gate: GPU take, pause/cancel, unavailable-shader CPU
 * recovery, and an ordinary CPU reference. No throughput benchmark. */
import assert from 'node:assert/strict';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseArgs} from 'node:util';
const {values} = parseArgs({options: {
  playwright: {type: 'string'}, browser: {type: 'string'},
  url: {type: 'string', default: 'http://127.0.0.1:8791'},
  output: {type: 'string', default: 'build/grain-workbench'},
  'fallback-seconds': {type: 'string', default: '0'},
  'fallback-profile': {type: 'string', default: 'default'},
}});
assert(values.playwright && values.browser);
const fallbackSeconds = Number(values['fallback-seconds']);
assert(Number.isFinite(fallbackSeconds) && (fallbackSeconds === 0 || (fallbackSeconds >= .1 && fallbackSeconds <= 30)));
assert(['default', 'dense'].includes(values['fallback-profile']));
assert(values['fallback-profile'] === 'default' || fallbackSeconds > 0);
const {chromium} = await import(pathToFileURL(resolve(values.playwright)).href);
await mkdir(values.output, {recursive: true});
const receipt = {status: 'running', profile: values['fallback-profile'], scope: 'Offline completion and processing budgets, not live deadlines', cases: []};
const save = () => writeFile(resolve(values.output, 'receipt.json'), JSON.stringify(receipt, null, 2) + '\n');
const browser = await chromium.launch({executablePath: values.browser, headless: true,
  args: ['--no-sandbox', '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan', '--disable-vulkan-surface']});
try {
  const page = await browser.newPage();
  page.setDefaultTimeout(60000);
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
    window.grainCheck = {messages: [], pauseOnReady: false};
    const OriginalWorker = Worker;
    window.Worker = class extends OriginalWorker {
      constructor(...args) {
        super(...args);
        this.addEventListener('message', ({data}) => {
          window.grainCheck.messages.push({type: data.type, grains: data.grains, mode: data.mode, processing: data.processing, renderMs: data.renderMs});
          if (data.type === 'ready' && window.grainCheck.pauseOnReady) {
            window.grainCheck.pauseOnReady = false;
            document.getElementById('pause').click();
          }
        });
      }
    };
  });
  await page.goto(values.url);
  assert.equal(await page.evaluate(() => crossOriginIsolated), false);
  assert.equal(await page.locator('#backend').inputValue(), 'internal');
  await page.selectOption('#backend', 'grains');
  assert.equal(await page.locator('#record-replay').isDisabled(), true);
  assert.equal(await page.locator('#visual-enabled').isDisabled(), true);
  assert.equal(await page.locator('#visual-panel').isVisible(), false);
  await page.fill('#seconds', '0.3'); // final 64-frame tail after full 512 deliveries
  await page.evaluate(() => { window.grainCheck.pauseOnReady = true; });
  await page.click('#render');
  await page.waitForFunction(() => window.grainCheck.messages.some(message => message.type === 'paused')); // actual worker boundary acknowledgement
  const count = await page.evaluate(() => window.grainCheck.messages.length);
  await page.waitForTimeout(120);
  assert.equal(await page.evaluate(() => window.grainCheck.messages.length), count, 'Paused worker still publishing');
  await page.click('#pause');
  async function finish(name, seconds = .3) {
    await page.waitForFunction(() => !document.getElementById('download').hidden, null, {timeout: 180000});
    const result = await page.evaluate(async () => ({
      bytes: Array.from(new Uint8Array(await (await fetch(document.getElementById('audio').src)).arrayBuffer())),
      done: window.grainCheck.messages.findLast(message => message.type === 'done'),
    }));
    const bytes = Buffer.from(result.bytes);
    assert.equal(bytes.length, 44 + Math.round(seconds * 48000) * 8);
    assert.equal(bytes.toString('ascii', 0, 4), 'RIFF');
    const pcm = new Float32Array(bytes.buffer.slice(bytes.byteOffset + 44, bytes.byteOffset + bytes.length));
    assert(pcm.every(Number.isFinite));
    assert(pcm.some(sample => Math.abs(sample) > 1e-5));
    await writeFile(resolve(values.output, name + '.wav'), bytes);
    receipt.cases.push({name, frames: pcm.length / 2, ...result.done?.grains, processing: result.done?.processing, renderMs: result.done?.renderMs});
    await save();
    return {pcm, bytes, done: result.done};
  }
  const gpu = await finish('gpu-paused');
  assert(gpu.done.grains.gpuBatches > 0, 'No real GPU deliveries');
  assert.equal(gpu.done.grains.cpuBatches, 0, 'Unexpected GPU fallback');
  const downloadPromise = page.waitForEvent('download');
  await page.click('#download');
  const download = await downloadPromise;
  assert.deepEqual(await readFile(await download.path()), gpu.bytes);
  await page.locator('#audio').evaluate(async node => { await node.play(); node.pause(); });
  // Cancel a live worker and immediately use a fresh one for a CPU-recovered take.
  await page.fill('#seconds', '30');
  await page.click('#render');
  await page.waitForFunction(() => document.getElementById('progress').value > 0);
  await page.click('#cancel');
  assert.equal(await page.locator('#status').textContent(), 'Render cancelled.');
  assert.equal(await page.locator('#download').isVisible(), false);
  await page.route('**/grain-plan.wgsl', route => route.fulfill({status: 404, body: ''}));
  await page.fill('#seconds', '0.3');
  await page.click('#render');
  const fallback = await finish('cpu-fallback');
  assert.equal(fallback.done.grains.gpuBatches, 0);
  assert.equal(fallback.done.grains.cpuBatches, Math.ceil(14400 / 512));
  assert.match(await page.locator('#provider-detail').textContent(), /fallback active/);
  assert.equal(fallback.done.processing.packedBytes, 0, 'CPU-only take packed GPU data');
  assert(fallback.done.grains.peakVoices > 0, 'CPU metadata lost without packets');
  await page.selectOption('#backend', 'internal');
  assert.equal(await page.locator('#visual-enabled').isDisabled(), false);
  await page.uncheck('#visual-enabled');
  await page.click('#render');
  const cpu = await finish('ordinary-cpu');
  assert.deepEqual(fallback.bytes, cpu.bytes, 'C/WASM recovered take differs from ordinary CPU');
  let maximum = 0;
  for (let i = 0; i < cpu.pcm.length; i++) maximum = Math.max(maximum, Math.abs(cpu.pcm[i] - gpu.pcm[i]));
  assert(maximum < 1e-5, 'GPU differs from CPU');
  if (fallbackSeconds) {
    if (values['fallback-profile'] === 'dense') {
      // ~400 overlapping grains in the existing 512-slot workbench. Remove
      // density/duration field modulation so the offered load is explicit.
      receipt.controls = {grain_rate: 2000, grain_ms: 200, speed_to_density: 0,
        strain_to_duration: 0, gain: .15};
      for (const [name, value] of Object.entries(receipt.controls))
        await page.fill(`input[name="${name}"]`, String(value));
    }
    await page.selectOption('#backend', 'grains');
    await page.fill('#seconds', String(fallbackSeconds));
    await page.click('#render');
    const sustained = await finish('sustained-cpu-fallback', fallbackSeconds);
    receipt.processingRatio = sustained.done.processing.cpuProcessingMs / sustained.done.processing.cpuAudioMs;
    assert.equal(sustained.done.grains.gpuBatches, 0);
    assert.equal(sustained.done.grains.cpuBatches, Math.ceil(fallbackSeconds * 48000 / 512));
    assert.equal(sustained.done.processing.packedBytes, 0);
    assert(sustained.done.grains.peakVoices > (values['fallback-profile'] === 'dense' ? 350 : 0));
    assert.equal(sustained.done.grains.numeric_interventions, 0);
    assert.equal(sustained.done.grains.voice_drops, 0, 'Pool overflow changes offered load');
    assert.equal(sustained.done.grains.cap_drops, 0);
    assert(Math.abs(sustained.done.processing.audioMs - fallbackSeconds * 1000) < 1e-5);
    assert.equal(sustained.done.processing.cpuAudioMs, sustained.done.processing.audioMs);
    assert(sustained.done.processing.overBudgetDeliveries <= sustained.done.grains.cpuBatches);
    assert(sustained.pcm.every(sample => Math.abs(sample) < 1), 'Dense take clips');
    for (const value of Object.values(sustained.done.processing)) assert(Number.isFinite(value) && value >= 0);
    await page.selectOption('#backend', 'internal');
    await page.uncheck('#visual-enabled');
    await page.click('#render');
    const ordinary = await finish('sustained-ordinary-cpu', fallbackSeconds);
    assert.deepEqual(sustained.bytes, ordinary.bytes, 'Sustained fallback differs from ordinary CPU');
  }
  assert.deepEqual(errors, []);
  receipt.maximumGpuError = maximum;
  receipt.checks = ['non-isolated mode availability', 'pause/resume boundary', 'cancel/restart', 'final partial delivery', 'WAV download/playback', 'prepared CPU fallback', 'exact ordinary CPU match', 'CPU capture skips packet fitting/copy', 'packet-independent voice statistics'];
  if (fallbackSeconds) receipt.checks.push(`${fallbackSeconds}-second exact CPU fallback continuation`);
  receipt.status = 'complete'; await save();
  console.log(JSON.stringify(receipt));
} catch (error) {
  receipt.status = 'failed'; receipt.error = String(error.stack || error); await save(); throw error;
} finally { await browser.close(); }
