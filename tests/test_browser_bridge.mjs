import assert from "node:assert/strict";
import {writeFile, mkdir} from "node:fs/promises";
import {parseArgs} from "node:util";
import {pathToFileURL} from "node:url";
import {resolve} from "node:path";
const {values} = parseArgs({options: {
  playwright: {type: "string"}, browser: {type: "string"},
  url: {type: "string", default: "http://127.0.0.1:8766"},
  output: {type: "string", default: "build/browser-bridge-check"},
}});
assert(values.playwright && values.browser);
const {chromium} = await import(pathToFileURL(resolve(values.playwright)).href);
const browser = await chromium.launch({executablePath: values.browser, headless: true, args: ["--no-sandbox"]});
const page = await browser.newPage();
const errors = [];
page.on("pageerror", error => errors.push(error.message));
const records = [];
try {
  await page.goto(values.url);
  assert(await page.evaluate(() => isSecureContext && crossOriginIsolated && typeof SharedArrayBuffer === "function"));
  assert(!(await page.locator('#backend option[value="external"]').evaluate(option => option.disabled)));
  const concurrency = await page.evaluate(async () => {
    const url = new URL("./bridge.js", location.href).href;
    const {SharedRing} = await import(url);
    const results = [];
    for (const [bytes, count] of [[256, 30000], [65664, 512]]) {
      const consumer = new SharedRing(bytes);
      const blob = new Blob([`
        import {SharedRing} from ${JSON.stringify(url)};
        onmessage = ({data}) => {
          const writer = new SharedRing(data.bytes, data.buffer);
          const payload = new Uint8Array(data.bytes);
          const words = new DataView(payload.buffer);
          let sequence = 1;
          function pump() {
            for (let i = 0; i < 64 && sequence <= data.count; i++) {
              payload.fill(sequence % 251);
              words.setUint32(0, sequence, true);
              words.setUint32(data.bytes - 4, sequence, true);
              if (!writer.push(payload)) break;
              sequence++;
            }
            if (sequence <= data.count) setTimeout(pump, 0);
            else postMessage(writer.stats());
          }
          pump();
        };
      `], {type: "text/javascript"});
      const blobUrl = URL.createObjectURL(blob);
      const writer = new Worker(blobUrl, {type: "module"});
      const output = new Uint8Array(bytes), words = new DataView(output.buffer);
      try {
        writer.postMessage({buffer: consumer.buffer, bytes, count});
        const deadline = performance.now() + 60000;
        let sequence = 1;
        while (sequence <= count) {
          for (let i = 0; i < 64 && sequence <= count; i++) {
            if (!consumer.pop(output)) break;
            if (words.getUint32(0, true) !== sequence || words.getUint32(bytes - 4, true) !== sequence)
              throw new Error("Torn or reordered shared publication");
            for (let j = 4; j < bytes - 4; j++)
              if (output[j] !== sequence % 251) throw new Error("Torn field payload");
            sequence++;
          }
          if (performance.now() > deadline) throw new Error("SAB stress timed out");
          await new Promise(resolve => setTimeout(resolve, 0));
        }
        results.push({bytes, count, ...consumer.stats()});
      } finally { writer.terminate(); URL.revokeObjectURL(blobUrl); }
    }
    return results;
  });
  for (const mode of ["normal", "stall", "busy", "pause", "loss"]) {
    const result = await page.evaluate(async mode => {
      const {SharedRing, commandBytes, packetBytes} = await import("./bridge.js");
      const setup = {grid: 32, commands: new SharedRing(commandBytes).buffer,
        fields: new SharedRing(packetBytes(32)).buffer};
      const solver = new Worker("./field.worker.js", {type: "module"});
      const audio = new Worker("./render.worker.js", {type: "module"});
      const source = Float64Array.from({length: 48000}, (_, i) => .3 * Math.sin(2 * Math.PI * 220 * i / 48000));
      // Read exactly the schema-driven UI settings, including its reset default.
      const inputs = document.querySelectorAll("#controls input");
      const controls = Array.from(inputs, item => Number(item.value)); controls.splice(22, 0, 0);
      let lost = false, recovered = false, triggered = false, fields = 0, views = 0;
      const fail = (reject, event) => reject(new Error(event.message || event.data?.message || "worker error"));
      try {
        await new Promise((resolve, reject) => {
          solver.onerror = event => fail(reject, event);
          solver.onmessage = ({data}) => {
            if (data.type === "error") reject(new Error(data.message));
            if (data.type === "ready") resolve();
          };
          solver.postMessage({type: "start", setup});
        });
        const output = await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => reject(new Error("External render timed out")), 45000);
          audio.onerror = event => { clearTimeout(timeout); fail(reject, event); };
          audio.onmessage = ({data}) => {
            if (data.type === "error") { clearTimeout(timeout); reject(new Error(data.message)); return; }
            if (data.type === "view") { if (data.snapshot) views++; return; }
            if (data.stats) {
              fields = Math.max(fields, data.stats.snapshot_sequence);
              lost ||= Boolean(data.stats.status & 8);
              recovered ||= lost && !(data.stats.status & 4);
            }
            if (data.type === "progress") {
              audio.postMessage({type: "view"});
              if (!triggered && data.fraction > .15) {
                triggered = true;
                if (mode === "stall") {
                  solver.postMessage({type: "pause"});
                  setTimeout(() => solver.postMessage({type: "resume"}), 650);
                }
                if (mode === "loss") solver.terminate();
                if (mode === "busy") {
                  const until = performance.now() + 350;
                  while (performance.now() < until) Math.sqrt(performance.now());
                }
                if (mode === "pause") {
                  audio.postMessage({type: "pause"});
                  setTimeout(() => audio.postMessage({type: "resume"}), 200);
                }
              }
            }
            if (data.type === "done") { clearTimeout(timeout); resolve(data); }
          };
          audio.postMessage({type: "start", settings: {source, sourceRate: 48000, controls},
            seconds: 3, setup}, [source.buffer]);
        });
        const view = new DataView(output.wav);
        let energy = 0, peak = 0;
        for (let i = 44; i < output.wav.byteLength; i += 4) {
          const x = view.getFloat32(i, true);
          if (!Number.isFinite(x)) throw new Error("Non-finite PCM");
          energy += x*x; peak = Math.max(peak, Math.abs(x));
        }
        return {mode, fields, views, lost, recovered, peak, energy, bridge: output.bridge,
          bytes: output.wav.byteLength, renderMs: output.renderMs,
          stats: output.stats};
      } finally { audio.terminate(); solver.terminate(); }
    }, mode);
    assert.equal(result.bytes, 44 + 3 * 48000 * 8);
    assert(result.fields > 5 && result.energy > .01 && result.peak < 1);
    assert.equal(result.stats.backend, 1);
    assert.equal(result.bridge.rejected, 0);
    if (mode === "stall") { assert(result.lost && result.recovered); assert(result.bridge.commands.dropped > 0); }
    if (mode === "loss") assert(result.lost && !result.recovered);
    records.push(result);
  }
  // UI cancellation stops both sibling workers and a new render still works.
  await page.locator("#backend").selectOption("external");
  await page.locator("#seconds").fill("3");
  await page.locator("#render").click();
  await page.waitForFunction(() => document.querySelector("#status").textContent === "Rendering…");
  await page.locator("#cancel").click();
  assert.equal(await page.locator("#status").textContent(), "Render cancelled.");
  await page.locator("#seconds").fill("1");
  await page.locator("#render").click();
  await page.waitForFunction(() => !document.querySelector("#download").hidden, {timeout: 30000});
  const wavBytes = await page.evaluate(async () =>
    (await (await fetch(document.querySelector("#download").href)).arrayBuffer()).byteLength);
  assert.equal(wavBytes, 44 + 48000 * 8);
  await page.locator("#audio").evaluate(audio => audio.play());
  await page.waitForFunction(() => document.querySelector("#audio").currentTime > .05);
  assert.deepEqual(errors, []);
  await mkdir(values.output, {recursive: true});
  await writeFile(resolve(values.output, "receipt.json"),
    JSON.stringify({browser: browser.version(), isolated: true, concurrency, records, cancelRestartPlayback: true, errors}, null, 2) + "\n");
  console.log(JSON.stringify({renders: records.length + 1, cancelRestartPlayback: true, records}));
} finally { await browser.close(); }
