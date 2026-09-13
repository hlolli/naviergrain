import {GPUProvider, decodeFieldCommand, encodeGPUField, type GPUBackend} from "../web/gpu-provider";
import {GPUFieldSolver, type GPUProfile} from "../web/gpu-solver";
import {SharedRing, commandBytes, packetBytes, SolverEngine} from "../web/bridge";
import {OfflineEngine, bellSource, browserConfig, type CsoundApi} from "../web/engine";
import {controlDefaults, control as C} from "../web/naviergrain-schema";
function check(value: unknown, message: string): asserts value { if (!value) throw new Error(message); }
Object.defineProperty(globalThis,"window",{value:{atob:globalThis.atob.bind(globalThis),
  btoa:globalThis.btoa.bind(globalThis),webkitAudioContext:undefined}});
const same = (a: Float64Array,b: Float64Array) => a.length===b.length && a.every((v,i)=>v===b[i]);
async function main() {
  const host = new URL("./csound.js",import.meta.url).href;
  const {libcsound} = await import(host) as {libcsound(o:{withPlugins:ArrayBuffer[]}):Promise<CsoundApi>};
  const plugin = await (await fetch("./naviergrain.wasm")).arrayBuffer();
  const includes = await (await fetch("./prepared.inc")).text();
  const [audioApi,cpuApi] = await Promise.all([libcsound({withPlugins:[plugin]}),libcsound({withPlugins:[plugin]})]);
  const records: object[] = [];
  async function render(mode: string, mix = 1, grid = 16, seconds = 1) {
    const setup = {grid,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(grid)).buffer};
    const controls: number[] = [...controlDefaults]; controls[C.mapping_mix]=mix; controls[C.scheduler]=0;
    let actualGPU: GPUFieldSolver | undefined;
    const factory = async (profile: GPUProfile): Promise<GPUBackend> => {
      if (mode === "missing") throw new Error("No test GPU adapter");
      actualGPU = await GPUFieldSolver.create(profile,mode === "shader" ? "invalid shader" : undefined);
      return actualGPU;
    };
    const provider = mode === "cpu" ? new SolverEngine(cpuApi,browserConfig(true,grid),setup) :
      await GPUProvider.create(cpuApi,browserConfig(true,grid),setup,factory);
    const engine = new OfflineEngine(audioApi,{source:bellSource(),sourceRate:48000,controls},includes,false,setup);
    const audioHeap = audioApi.getMemory().buffer, cpuHeap = cpuApi.getMemory().buffer;
    const pcm = new Float64Array(750*seconds*128);
    let lossEpoch = 0, fieldsBeforeLoss = 0, maximumAge = 0;
    let meanCallback = 0, maxCallback = 0;
    try {
      for (let block=0;block<750*seconds;block++) {
        if (mode === "reset") engine.reset(block === 320);
        if (mode === "resume" && block === 320) {engine.silence(5);engine.resumeExternal();}
        if (["loss","timeout"].includes(mode) && block===250) {
          lossEpoch = engine.stats().epoch; fieldsBeforeLoss = engine.stats().snapshot_sequence;
          check(actualGPU,"No actual GPU");
          if (mode === "loss") {
            const device = Reflect.get(actualGPU,"device") as GPUDevice;
            device.destroy(); await device.lost;
          } else {
            // Test-only unresolved readback. The provider deadline must destroy
            // its owner, request recovery, and never publish a late result.
            actualGPU.step = () => new Promise(() => {});
          }
        }
        const before=performance.now();
        pcm.set(engine.render(1),block*128);
        const elapsed=performance.now()-before;
        meanCallback+=elapsed; maxCallback=Math.max(maxCallback,elapsed);
        await provider.work();
        maximumAge=Math.max(maximumAge,engine.stats().field_age_ms);
      }
      const stats = engine.stats();
      check(pcm.every(Number.isFinite) && pcm.some(v=>Math.abs(v)>.0001),"Invalid/silent GPU audio");
      check(stats.snapshot_sequence>5 && stats.backend===1,"Fields did not drive the instrument");
      check(engine.bridge!.rejected===0,"Audio mailbox rejected GPU packet");
      check(stats.numeric_interventions===0,"Unexpected numerical intervention");
      check(audioHeap===audioApi.getMemory().buffer && cpuHeap===cpuApi.getMemory().buffer,"Processing heap grew");
      const report = provider instanceof GPUProvider ? provider.report() : undefined;
      if (report) {
        check(report.rejected===0,"GPU provider rejected valid commands");
        if (["loss","timeout","missing","shader"].includes(mode)) {
          check(report.backend==="cpu-fallback" && report.cpuFields>10,"Fallback did not publish CPU fields");
          if (lossEpoch) {
            check(stats.epoch>lossEpoch && report.waitingEpoch===null,"No audio-owned recovery epoch");
            check(fieldsBeforeLoss>5 && report.gpuFields>5,"No GPU audio before loss");
          }
        } else check(report.gpuFields>5 && report.cpuFields===0,"GPU mode secretly used CPU output");
      }
      records.push({mode,mix,grid,seconds,maximumAge,stats,report,
        meanCallbackMs:meanCallback/(750*seconds),maxCallbackMs:maxCallback});
      postMessage({type:"progress",name:mode,grid});
      return pcm;
    } finally {provider.destroy();engine.destroy();}
  }
  const baseline = await render("gpu");
  check(same(baseline,await render("gpu")),"GPU accepted-schedule replay changed PCM");
  const cpu=await render("cpu");
  let square=0,power=0,max=0;
  for(let i=0;i<cpu.length;i++) {const d=baseline[i]!-cpu[i]!;square+=d*d;power+=cpu[i]!*cpu[i]!;max=Math.max(max,Math.abs(d));}
  const comparison={relativeRms:Math.sqrt(square/power),max};
  check(comparison.relativeRms<.01,"Short GPU/CPU audio error exceeds 1% RMS");
  check(!same(baseline,await render("gpu",0)),"GPU fields made no audible mapping change");
  check(same(await render("gpu",0),await render("cpu",0)),"Fixed mapping changed GPU audio");
  check(same(await render("reset"),await render("reset")),"Musical reset not repeatable");
  await render("resume");
  await render("loss"); await render("timeout");
  check(same(cpu,await render("missing")),"Missing GPU differs from prepared CPU");
  check(same(cpu,await render("shader")),"Shader failure differs from prepared CPU");
  for(const grid of [32,64]) await render("loss",1,grid);
  await render("gpu",1,32,30);

  // Exact uint64 packet/command metadata, not rounded through Number.
  const bytes=new Uint8Array(commandBytes),view=new DataView(bytes.buffer);
  view.setUint32(0,0x4d43474e,true);view.setUint32(4,1,true);view.setBigUint64(8,1n,true);
  const wide=(1n<<60n)+37n;
  for(const offset of [16,24,32,40])view.setBigUint64(offset,wide,true);
  controlDefaults.forEach((v,i)=>view.setFloat64(64+8*i,i===C.pitch_ratio?Math.log2(v):v,true));
  const command=decodeFieldCommand(bytes,1n);
  const packet=encodeGPUField(command,{planes:new Float32Array(4*256),diagnostics:new Float32Array(16),
    time:.5,sequence:1,interventions:3},16);
  const header=new DataView(packet.buffer);
  check(header.getBigUint64(24,true)===wide&&header.getBigUint64(32,true)===wide&&
    header.getBigUint64(40,true)===wide,"uint64 clocks rounded");
  for (const offset of [0,4,8,48,64]) {
    const bad=bytes.slice(); const v=new DataView(bad.buffer);
    if(offset===64)v.setFloat64(64,NaN,true);else bad[offset]=255;
    let rejected=false;try{decodeFieldCommand(bad,1n);}catch{rejected=true;}
    check(rejected,"Invalid command accepted at "+offset);
  }
  // Independent C encoder oracle using the existing solver mailbox, including
  // clocks above 2^53. Re-encode its field, require every byte to agree.
  const local={grid:16,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(16)).buffer};
  const cSolver=new SolverEngine(cpuApi,browserConfig(true,16),local);
  try {
    new SharedRing(commandBytes,local.commands).push(bytes);
    check(cSolver.work(),"C oracle did not consume command");
    const expected=new Uint8Array(packetBytes(16));
    check(new SharedRing(expected.length,local.fields).pop(expected),"C encoder rejected wide timestamps");
    const v=new DataView(expected.buffer),d=new Float32Array(16);
    [4,5,6,7,8,1].forEach((index,i)=>{d[index]=v.getFloat32(72+4*i,true);});
    const actual=encodeGPUField(command,{planes:new Float32Array(expected.buffer,128).slice(),diagnostics:d,
      time:v.getFloat64(48,true),sequence:1,interventions:Number(v.getBigUint64(96,true))},16);
    check(actual.every((byte,i)=>byte===expected[i]),"JS packet differs from C encoder");
  } finally {cSolver.destroy();}
  // Cancellation while a real mapAsync is pending must not publish late data.
  const closeSetup={grid:16,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(16)).buffer};
  const pendingOwner=await GPUProvider.create(cpuApi,browserConfig(true,16),closeSetup);
  try {
    new SharedRing(commandBytes,closeSetup.commands).push(bytes);
    const pending=pendingOwner.work();
    let rejected=false;try{await pendingOwner.work();}catch{rejected=true;}
    check(rejected,"Concurrent work was accepted");
    pendingOwner.destroy();await pending;
    check(new SharedRing(packetBytes(16),closeSetup.fields).stats().published===0,"Late GPU publication after teardown");
  } finally {pendingOwner.destroy();}
  const pressureSetup={grid:16,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(16)).buffer};
  const pressure=await GPUProvider.create(cpuApi,browserConfig(true,16),pressureSetup);
  const writer=new SharedRing(commandBytes,pressureSetup.commands);
  const reader=new SharedRing(packetBytes(16),pressureSetup.fields);
  try {
    for(let i=0;i<9;i++) {
      view.setBigUint64(32,wide+BigInt(i),true);
      view.setBigUint64(40,wide+BigInt(i*800),true);
      check(writer.push(bytes),"Command queue unexpectedly full");await pressure.work();
    }
    check(reader.stats().published===4&&reader.stats().dropped===5,"Full queue must drop NEW fields");
    const packet=new Uint8Array(packetBytes(16));
    for(let i=0;i<4;i++){check(reader.pop(packet),"Missing queued field");
      check(new DataView(packet.buffer).getBigUint64(32,true)===wide+BigInt(i),"Full queue overwrote old field");}
    view.setBigUint64(32,wide+10n,true);view.setBigUint64(16,wide+1n,true);
    view.setBigUint64(24,wide+1n,true);view.setBigUint64(40,0n,true);
    writer.push(bytes);await pressure.work();check(reader.pop(packet),"Reset did not recover from queue pressure");
    const header=new DataView(packet.buffer);
    check(header.getBigUint64(24,true)===wide+1n&&header.getFloat64(48,true)===1/60,"Reset epoch/time not preserved");
  } finally {pressure.destroy();}
  return {scope:"Actual WebGPU fields drive Csound/WASM audio; software backend correctness, not live qualification",
    renders:records.length,comparison,records,uint64Clocks:true,invalidCommands:true,cEncoderParity:true,concurrentWorkRejected:true,teardownDuringReadback:true,fullQueueReset:true};
}
void main().then(result=>postMessage({type:"done",result}),
 error=>postMessage({type:"error",message:error instanceof Error?error.stack:String(error)}));
