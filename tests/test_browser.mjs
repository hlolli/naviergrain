/** Actual Chromium UI/worker checks. Reuses an installed Playwright; no installs.
 * node tests/test_browser.mjs --playwright /path/to/playwright-core/index.mjs
 *   --browser /path/to/chromium --url http://127.0.0.1:8765
 */
import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {writeFile, mkdir} from "node:fs/promises";
import {resolve} from "node:path";
import {pathToFileURL} from "node:url";
import {parseArgs} from "node:util";

const {values} = parseArgs({options: {
  playwright: {type: "string"}, browser: {type: "string"},
  url: {type: "string", default: "http://127.0.0.1:8765"},
  output: {type: "string", default: "build/browser-check"},
}});
assert(values.playwright && values.browser, "Provide installed Playwright and Chromium paths");
const {chromium} = await import(pathToFileURL(resolve(values.playwright)).href);
const browser = await chromium.launch({executablePath: resolve(values.browser),
  headless: true, args: ["--no-sandbox"]});
const page = await browser.newPage({viewport: {width: 1120, height: 1000}});
const errors = [];
const records = [];
page.on("pageerror", error => errors.push(error.message));
await mkdir(values.output, {recursive: true});
const hash = data => createHash("sha256").update(data).digest("hex");
try {
  await page.addInitScript(() => {
    window.viewProbe = {requests: 0, replies: 0, pending: 0, maxPending: 0, snapshots: 0, grains: 0, fields: 0, repeatedIds: 0};
    const OriginalWorker = Worker;
    window.Worker = class extends OriginalWorker {
      constructor(...args) {
        super(...args);
        let previousIds = new Set();
        this.addEventListener("message", ({data}) => {
          const probe = window.viewProbe;
          if (data.type === "view") {
            probe.replies++; probe.pending--;
            if (data.snapshot) {
              probe.snapshots++;
              probe.grains = Math.max(probe.grains, data.snapshot[8]);
              probe.fields += Number(data.snapshot[11] === 1);
              const ids = new Set();
              for (let i = 0; i < data.snapshot[8]; i++) {
                const k = 2064 + i * 8;
                const id = data.snapshot[3] + ":" + data.snapshot[2] + ":" + data.snapshot[k + 1] + ":" + data.snapshot[k];
                ids.add(id); if (previousIds.has(id)) probe.repeatedIds++;
              }
              previousIds = ids;
            }
          }
        });
      }
      postMessage(data, ...args) {
        if (data.type === "view") {
          const probe = window.viewProbe;
          probe.requests++; probe.pending++; probe.maxPending = Math.max(probe.maxPending, probe.pending);
        }
        return super.postMessage(data, ...args);
      }
      terminate() { window.viewProbe.pending = 0; super.terminate(); }
    };
    // Exercise the dependency-free CPU path, regardless of the test machine.
    Object.defineProperty(navigator, "gpu", {value: undefined});
    Object.defineProperty(globalThis, "SharedArrayBuffer", {value: undefined});
  });
  await page.goto(values.url);
  assert.equal(await page.evaluate(() => crossOriginIsolated), false);
  assert(await page.locator('#backend option[value="external"]').evaluate(option => option.disabled));
  assert.equal(await page.locator("#controls input").count(), 23);
  await page.locator("#seconds").fill("0.5");
  async function rendered(name, {pause = false, hidden = false, busy = false, seconds = .5} = {}) {
    if (hidden) await page.evaluate(() => document.body.style.visibility = "hidden");
    if (hidden) await page.evaluate(() => document.querySelector("#settings").requestSubmit());
    else await page.locator("#render").click();
    if (pause) {
      await page.locator("#pause").click();
      await page.waitForFunction(() => document.querySelector("#status").textContent === "Render paused.");
      const before = await page.locator("#progress").getAttribute("value");
      await page.waitForTimeout(150);
      assert.equal(await page.locator("#progress").getAttribute("value"), before);
      await page.locator("#pause").click();
    }
    if (busy) {
      await page.waitForFunction(() => document.querySelector("#status").textContent === "Rendering…");
      await page.evaluate(() => {
        document.querySelector("#trail-count").value = "128";
        document.querySelector("#trail-count").dispatchEvent(new Event("change"));
        const until = performance.now() + 350;
        while (performance.now() < until) Math.sqrt(performance.now());
      });
    }
    await page.waitForFunction(() => !document.querySelector("#render").disabled,
      null, {timeout: 60000});
    const status = await page.locator("#status").textContent();
    assert.match(status, /^Ready to play/, status);
    const bytes = Buffer.from(await page.evaluate(async () =>
      Array.from(new Uint8Array(await (await fetch(document.querySelector("#audio").src)).arrayBuffer()))));
    assert.equal(bytes.toString("ascii", 0, 4), "RIFF");
    assert.equal(bytes.readUInt16LE(20), 3);
    assert.equal(bytes.readUInt32LE(24), 48000);
    assert.equal(bytes.length, 44 + Math.round(seconds * 48000) * 2 * 4);
    if (hidden) await page.evaluate(() => document.body.style.visibility = "");
    records.push({name, sha256: hash(bytes), bytes: bytes.length, status});
    return bytes;
  }
  const first = await rendered("baseline");
  assert.deepEqual(await rendered("pause-resume", {pause: true}), first);
  assert.deepEqual(await rendered("hidden-controls", {hidden: true}), first);
  assert.deepEqual(await rendered("busy-ui-changing-trails", {busy: true}), first);
  await page.locator("#visual-enabled").uncheck();
  const requests = await page.evaluate(() => window.viewProbe.requests);
  assert.deepEqual(await rendered("visuals-disabled"), first);
  assert.equal(await page.evaluate(() => window.viewProbe.requests), requests);
  await page.locator("#visual-enabled").check();
  await page.locator("#trail-count").selectOption("0");
  assert.deepEqual(await rendered("zero-trails"), first);
  await page.locator("#trail-count").selectOption("128");
  assert.deepEqual(await rendered("maximum-trails"), first);
  await page.locator("#visual-mode").selectOption("vorticity");
  assert.deepEqual(await rendered("vorticity-view"), first);
  await page.locator("#visual-mode").selectOption("strain");
  assert.deepEqual(await rendered("strain-view"), first);
  let observation = await page.evaluate(() => window.viewProbe);
  assert(observation.snapshots > 0 && observation.grains > 0 && observation.fields > 0);
  assert.equal(observation.maxPending, 1);
  assert.match(await page.locator("#visual-detail").textContent(), /CPU · simulation/);
  await page.locator("#visual-mode").selectOption("velocity");
  const audio = await page.evaluate(async () => {
    const player = document.querySelector("#audio");
    await player.play();
    return {duration: player.duration, paused: player.paused};
  });
  assert(Math.abs(audio.duration - .5) < .0001);
  assert.equal(audio.paused, false);
  await page.locator("#seconds").fill("30");
  await page.locator("#render").click();
  await page.locator("#cancel").click();
  assert.equal(await page.locator("#status").textContent(), "Render cancelled.");
  await page.waitForTimeout(150);
  assert.equal(await page.locator("#status").textContent(), "Render cancelled.");
  await page.locator("#seconds").fill("0.5");
  assert.deepEqual(await rendered("cancel-restart"), first);
  await page.locator('[name="swirl"]').fill("0");
  assert.notDeepEqual(await rendered("changed-swirl"), first);
  await page.locator("#defaults").click();
  assert.deepEqual(await rendered("restore-defaults"), first);

  // First-channel upload with a declared 24 kHz rate and a silent second channel.
  const source = Buffer.alloc(44 + 2400 * 4);
  source.write("RIFF"); source.writeUInt32LE(source.length - 8, 4); source.write("WAVE", 8);
  source.write("fmt ", 12); source.writeUInt32LE(16, 16); source.writeUInt16LE(1, 20);
  source.writeUInt16LE(2, 22); source.writeUInt32LE(24000, 24);
  source.writeUInt32LE(96000, 28); source.writeUInt16LE(4, 32);
  source.writeUInt16LE(16, 34); source.write("data", 36);
  source.writeUInt32LE(source.length - 44, 40);
  for (let i = 0; i < 2400; i++)
    source.writeInt16LE(Math.round(10000 * Math.sin(2 * Math.PI * 330 * i / 24000)), 44 + i * 4);
  await page.locator("#source").setInputFiles({name: "source-24k.wav", mimeType: "audio/wav", buffer: source});
  await page.waitForFunction(() => !document.querySelector("#render").disabled);
  assert.match(await page.locator("#source-label").textContent(), /first channel · decoded to 48 kHz/);
  assert.notDeepEqual(await rendered("uploaded-source"), first);
  await page.locator("#source").setInputFiles({name: "broken.wav", mimeType: "audio/wav", buffer: Buffer.from("broken")});
  await page.waitForFunction(() => !document.querySelector("#render").disabled);
  assert(!await page.locator("#source").inputValue());
  await page.locator("#bell").click();
  assert.deepEqual(await rendered("source-error-recovery"), first);
  const pendingDownload = page.waitForEvent("download");
  await page.locator("#download").click();
  const download = await pendingDownload;
  assert.equal(download.suggestedFilename(), "naviergrain.wav");
  await download.saveAs(resolve(values.output, "naviergrain.wav"));
  await page.locator("#seconds").fill("2");
  await page.locator('[name="grain_ms"]').fill("500");
  await page.locator('[name="grain_rate"]').fill("600");
  await rendered("tracked-grain-cloud", {seconds: 2});
  observation = await page.evaluate(() => window.viewProbe);
  assert(observation.repeatedIds > 0, "No persistent grain IDs observed for trails");
  assert(observation.grains <= 128 && observation.grains > 30);
  assert.equal(observation.maxPending, 1);
  await page.screenshot({path: resolve(values.output, "workbench.png"), fullPage: true});
  await page.setViewportSize({width: 390, height: 844});
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
  await page.screenshot({path: resolve(values.output, "mobile.png"), fullPage: true});
  assert.deepEqual(errors, []);
  const receipt = {scope: "Chromium offline worker UI, not AudioWorklet qualification",
    browser: browser.version(), withoutWebGPU: true, withoutSAB: true,
    crossOriginIsolated: false, audioPlayback: audio, cancellation: true,
    invalidSourceRecovery: true, download: true, mobileOverflow: false, observation, busyUiBlockMs: 350, visualAudioIndependence: true, records};
  await writeFile(resolve(values.output, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n");
  console.log(JSON.stringify(receipt, null, 2));
} finally { await browser.close(); }
