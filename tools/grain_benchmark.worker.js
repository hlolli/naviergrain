/* Deliberately standalone benchmark: production UI and solver are not imported. */
const check = (ok, message) => { if (!ok) throw new Error(message); };
async function bytes(name) {
  const response = await fetch(name);check(response.ok, `Missing ${name}`);
  return response.arrayBuffer();
}
self.onmessage = async ({data: request}) => {
  let device;
  const buffers = [];
  try {
    const start = performance.now(), attempts = [];
    let adapter;
    do {
      adapter = await navigator.gpu?.requestAdapter({powerPreference: 'high-performance'});
      attempts.push(adapter ? {vendor: adapter.info.vendor, architecture: adapter.info.architecture,
        device: adapter.info.device, description: adapter.info.description} : null);
      if (adapter) break;
      await new Promise(resolve => setTimeout(resolve, 500));
    } while (performance.now() - start < 3000);
    check(adapter, 'No WebGPU adapter within startup budget');
    if (request.hardware) check(adapter.info.vendor.toLowerCase().includes('nvidia'), 'Requested NVIDIA, got another adapter');
    const timestamps = adapter.features.has('timestamp-query');
    device = await adapter.requestDevice({requiredFeatures: timestamps ? ['timestamp-query'] : []});
    let failure;
    device.addEventListener('uncapturederror', e => { failure = e.error.message; });
    const cpu = JSON.parse(new TextDecoder().decode(await bytes('./cpu.json')));
    const [input, voices, shader] = await Promise.all([
      bytes('./input.f32'), bytes('./voices.f32'), bytes('./grain_benchmark.wgsl')]);
    const prepareStart = performance.now();
    const module = device.createShaderModule({code: new TextDecoder().decode(shader)});
    const diagnostics = await module.getCompilationInfo();
    check(!diagnostics.messages.some(m => m.type === 'error'), JSON.stringify(diagnostics.messages));
    const pipeline = await device.createComputePipelineAsync({layout: 'auto', compute: {module, entryPoint: 'main'}});
    const buffer = (size, usage) => {
      const b = device.createBuffer({size, usage});buffers.push(b);return b;
    };
    const settings = buffer(16, GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
    const resident = buffer(input.byteLength, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
    const grains = buffer(voices.byteLength, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
    const output = buffer(2048 * 8, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC);
    // The same one-slot staging buffer is reused only after unmap completes.
    const staging = buffer(2048 * 8 + 16, GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ);
    const query = timestamps ? device.createQuerySet({type: 'timestamp', count: 2}) : null;
    const resolved = timestamps ? buffer(16, GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC) : null;
    const bind = device.createBindGroup({layout: pipeline.getBindGroupLayout(0), entries:
      [settings, resident, grains, output].map((b, binding) => ({binding, resource: {buffer: b}}))});
    device.queue.writeBuffer(resident, 0, input);
    await device.queue.onSubmittedWorkDone();
    const preparationMs = performance.now() - prepareStart;
    const rows = [];
    const cases = request.reverse ? [...cpu.rows].reverse() : cpu.rows;
    for (const row of cases) {
      const reference = new Float64Array(await bytes(`./reference-${row.voices}-${row.frames}.f64`));
      const elapsedMs = [], computeMs = [];
      let maxAbsoluteError = 0, relativeRmsError = 0, pcm;
      for (let rep = -1; rep < 7; rep++) {
        const t = performance.now();
        // Steady-state end-to-end cost includes active voice upload, command
        // encoding/submission, dispatch, stereo readback and an owned PCM copy.
        device.queue.writeBuffer(settings, 0, new Uint32Array([row.voices, row.frames, cpu.sourceLength, 0]));
        device.queue.writeBuffer(grains, 0, voices, 0, row.voices * 32);
        const encoder = device.createCommandEncoder();
        const pass = encoder.beginComputePass(query ? {timestampWrites:
          {querySet: query, beginningOfPassWriteIndex: 0, endOfPassWriteIndex: 1}} : {});
        pass.setPipeline(pipeline);pass.setBindGroup(0, bind);pass.dispatchWorkgroups(row.frames);pass.end();
        encoder.copyBufferToBuffer(output, 0, staging, 0, row.frames * 8);
        if (query) {
          encoder.resolveQuerySet(query, 0, 2, resolved, 0);
          encoder.copyBufferToBuffer(resolved, 0, staging, 2048 * 8, 16);
        }
        device.queue.submit([encoder.finish()]);
        await staging.mapAsync(GPUMapMode.READ);
        const mapped = staging.getMappedRange();
        pcm = new Float32Array(mapped, 0, row.frames * 2).slice();
        let gpuTime;
        if (query) {
          const nanos = new BigUint64Array(mapped, 2048 * 8, 2);
          gpuTime = Number(nanos[1] - nanos[0]) / 1e6;
        }
        staging.unmap();
        const elapsed = performance.now() - t;
        check(!failure, failure);
        if (rep >= 0) { elapsedMs.push(elapsed); if (query) computeMs.push(gpuTime); }
        let errorPower = 0, signalPower = 0;
        for (let i = 0; i < pcm.length; i++) {
          check(Number.isFinite(pcm[i]), 'Non-finite GPU audio');
          const error = pcm[i] - reference[i];
          maxAbsoluteError = Math.max(maxAbsoluteError, Math.abs(error));
          errorPower += error * error;signalPower += reference[i] * reference[i];
        }
        relativeRmsError = Math.max(relativeRmsError, Math.sqrt(errorPower / signalPower));
        check(maxAbsoluteError < 1e-5 && relativeRmsError < 1e-4,
          `Audio mismatch ${row.voices}/${row.frames}: abs=${maxAbsoluteError} relative=${relativeRmsError}`);
      }
      const result = {...row, gpuEndToEndMs: elapsedMs, gpuComputeMs: computeMs,
        maxAbsoluteError, relativeRmsError, nominalBatchMs: row.frames / 48};
      rows.push(result);
      postMessage({type: 'row', result, pcm}, [pcm.buffer]);
    }
    query?.destroy();
    for (const b of buffers) b.destroy();buffers.length = 0;
    device.destroy();device = null;
    postMessage({type: 'complete', result: {rows, adapter: attempts.at(-1), attempts, preparationMs, timestamps}});
  } catch (error) {
    postMessage({type: 'error', message: String(error.stack || error)});
  } finally {
    for (const b of buffers) b.destroy();device?.destroy();
  }
};
