/** Focused AudioWorklet transport/lifecycle smoke, not a performance benchmark. */
import assert from "node:assert/strict";
import {parseArgs} from "node:util";
import {pathToFileURL} from "node:url";
import {resolve} from "node:path";
import {mkdir, writeFile} from "node:fs/promises";
const {values} = parseArgs({options: {
  playwright: {type: "string"}, browser: {type: "string"},
  url: {type: "string", default: "http://127.0.0.1:8791"},
  output: {type: "string", default: "build/buffered-preview"},
}});
const {chromium} = await import(pathToFileURL(resolve(values.playwright)).href);
await mkdir(values.output, {recursive: true});
const receipt = {status: "running", scope: "Headless AudioWorklet smoke; no physical device qualification", cases: []};
const save = () => writeFile(resolve(values.output, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n");
const browser = await chromium.launch({executablePath: values.browser, headless: true,
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required",
    "--enable-unsafe-webgpu", "--use-angle=vulkan", "--enable-features=Vulkan", "--disable-vulkan-surface"]});
try {
  const page = await browser.newPage();
  page.setDefaultTimeout(45000);
  const errors = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.addInitScript(() => {
    window.previewCheck = {contexts: [], nodes: [], messages: [], workers: []};
    const OriginalContext = AudioContext, OriginalNode = AudioWorkletNode, OriginalWorker = Worker;
    window.AudioContext = class extends OriginalContext {
      constructor(...args) { super(...args); window.previewCheck.contexts.push(this); }
    };
    window.AudioWorkletNode = class extends OriginalNode {
      constructor(...args) {
        super(...args); window.previewCheck.nodes.push(this);
        if (window.previewCheck.suspendOnStart) void this.context.suspend();
        this.port.addEventListener("message", ({data}) => window.previewCheck.messages.push(data));
      }
    };
    window.Worker = class extends OriginalWorker {
      constructor(...args) { super(...args); window.previewCheck.workers.push(this); }
    };
  });
  await page.goto(values.url);
  assert.equal(await page.evaluate(() => crossOriginIsolated), false);
  assert(await page.locator("#preview-enabled").isDisabled());
  await page.selectOption("#backend", "grains");
  assert.equal(await page.locator("#preview-enabled").isChecked(), false);
  // Force prepared CPU fallback: reliably exercise the queue without GPU qualification.
  await page.route("**/grain-plan.wgsl", route => route.fulfill({status: 404, body: ""}));
  await page.fill("#seconds", "0.3"); // includes a 64-frame final delivery
  await page.check("#preview-enabled");
  await page.click("#render");
  await page.waitForFunction(() => !document.getElementById("download").hidden);
  await page.waitForFunction(() => window.previewCheck.messages.some(message => message.type === "ended"));
  const first = await page.evaluate(async () => ({
    wav: Array.from(new Uint8Array(await (await fetch(document.getElementById("download").href)).arrayBuffer())),
    ended: window.previewCheck.messages.findLast(message => message.type === "ended"),
  }));
  assert.equal(first.ended.played, 14400);
  assert(first.ended.maximumBuffered <= 8192);
  await page.waitForFunction(() => window.previewCheck.contexts.every(context => context.state === "closed"));
  assert.match(await page.locator("#provider-detail").textContent(), /fallback active/);
  receipt.cases.push({name: "fallback-preview-tail", ...first.ended});
  await page.uncheck("#preview-enabled");
  await page.click("#render");
  await page.waitForFunction(() => !document.getElementById("download").hidden);
  const plain = await page.evaluate(async () =>
    Array.from(new Uint8Array(await (await fetch(document.getElementById("download").href)).arrayBuffer())));
  assert.deepEqual(first.wav, plain, "Preview must not change the WAV");
  receipt.cases.push({name: "wav-unchanged", bytes: plain.length});
  await page.check("#preview-enabled");
  await page.fill("#seconds", "5");
  await page.click("#render");
  await page.waitForFunction(() => document.getElementById("progress").value > 0);
  await page.click("#pause");
  await page.waitForFunction(() => document.getElementById("status").textContent === "Render paused.");
  await page.waitForTimeout(80);
  const played = await page.evaluate(() => window.previewCheck.messages.findLast(m => m.type === "progress")?.played);
  await page.waitForTimeout(120);
  assert.equal(await page.evaluate(() => window.previewCheck.messages.findLast(m => m.type === "progress")?.played), played);
  // Device suspension is independent from a deliberate render pause.
  await page.evaluate(() => window.previewCheck.contexts.at(-1).suspend());
  await page.waitForFunction(() => !document.getElementById("preview-resume").hidden);
  await page.click("#preview-resume");
  await page.waitForFunction(() => window.previewCheck.contexts.at(-1).state === "running");
  await page.waitForTimeout(120);
  assert.equal(await page.evaluate(() => window.previewCheck.messages.findLast(m => m.type === "progress")?.played), played);
  assert.equal(await page.locator("#pause").textContent(), "Resume render");
  receipt.cases.push({name: "suspend-resume-preserves-manual-pause", played});
  await page.click("#pause");
  await page.waitForFunction(old => window.previewCheck.messages.findLast(m => m.type === "progress")?.played > old, played);
  // Stop the producer to exercise actual Worklet rebuffering while the UI is blocked.
  await page.evaluate(() => window.previewCheck.workers.at(-1).postMessage({type: "pause"}));
  await page.waitForFunction(() => window.previewCheck.messages.findLast(m => m.type === "progress")?.underruns > 0);
  await page.evaluate(() => { const end = performance.now() + 180; while (performance.now() < end) {} });
  await page.click("#cancel");
  await page.waitForFunction(() => window.previewCheck.contexts.every(context => context.state === "closed"));
  assert(await page.locator("#download").isHidden());
  receipt.cases.push({name: "pause-resume-stall-cancel", messages: await page.evaluate(() => window.previewCheck.messages.length)});
  // A short render can finish while its entire tail remains suspended.
  await page.evaluate(() => { window.previewCheck.suspendOnStart = true; window.previewCheck.messages = []; });
  await page.fill("#seconds", "0.1");
  await page.click("#render");
  await page.waitForFunction(() => !document.getElementById("download").hidden);
  await page.waitForFunction(() => !document.getElementById("preview-resume").hidden);
  assert(await page.locator("#pause").isDisabled(), "Render is finished while its audio tail is pending");
  assert.equal(await page.evaluate(() => window.previewCheck.messages.some(m => m.type === "ended")), false);
  // A rejected resume remains retryable and must not erase the queued take.
  await page.evaluate(() => {
    const context = window.previewCheck.contexts.at(-1);
    const resume = context.resume.bind(context);
    context.resume = () => { context.resume = resume; return Promise.reject(new Error("simulated resume rejection")); };
  });
  await page.click("#preview-resume");
  await page.waitForFunction(() => document.getElementById("preview-status").textContent.includes("could not resume"));
  await page.click("#preview-resume");
  await page.waitForFunction(() => window.previewCheck.messages.some(m => m.type === "ended"));
  const tail = await page.evaluate(() => window.previewCheck.messages.findLast(m => m.type === "ended"));
  assert.equal(tail.played, 4800);
  assert(tail.maximumBuffered <= 8192);
  assert(await page.locator("#preview-resume").isHidden());
  receipt.cases.push({name: "suspended-completed-tail-and-resume-retry", ...tail});
  await page.waitForFunction(() => window.previewCheck.contexts.every(context => context.state === "closed"));
  await page.evaluate(() => { window.previewCheck.suspendOnStart = false; });
  await page.fill("#seconds", "5");
  await page.click("#render");
  await page.waitForFunction(() => document.getElementById("progress").value > 0);
  await page.evaluate(() => window.previewCheck.contexts.at(-1).close());
  await page.waitForFunction(() => document.getElementById("status").textContent.includes("Preview failed"));
  assert(await page.locator("#download").isHidden());
  assert(await page.locator("#preview-stop").isDisabled());
  assert(await page.locator("#preview-resume").isHidden());
  receipt.cases.push({name: "unexpected-device-close-cancels-producer"});
  assert.deepEqual(errors, []);
  receipt.status = "passed"; await save();
  console.log(JSON.stringify(receipt, null, 2));
} catch (error) {
  receipt.status = "failed"; receipt.error = String(error); await save(); throw error;
} finally { await browser.close(); }
