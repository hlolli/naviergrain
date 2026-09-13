import {ReplayCapture, ReplayPlayer, sha256} from "../web/replay";
import {SolverEngine, SharedRing, commandBytes, packetBytes} from "../web/bridge";
import {OfflineEngine, bellSource, browserConfig, type CsoundApi} from "../web/engine";
import {GPUProvider} from "../web/gpu-provider";
import {GPUFieldSolver} from "../web/gpu-solver";
import {controlDefaults} from "../web/naviergrain-schema";
function check(value: unknown, message: string): asserts value { if (!value) throw new Error(message); }
Object.defineProperty(globalThis,"window",{value:{atob:globalThis.atob.bind(globalThis),
  btoa:globalThis.btoa.bind(globalThis),webkitAudioContext:undefined}});
async function main() {
  const host = new URL("./csound.js",import.meta.url).href;
  const {libcsound} = await import(host) as {libcsound(o:{withPlugins:ArrayBuffer[]}):Promise<CsoundApi>};
  const plugin = await (await fetch("./naviergrain.wasm")).arrayBuffer();
  const includes = await (await fetch("./prepared.inc")).text();
  const build = await sha256(new Uint8Array(await (await fetch("./provenance.json")).arrayBuffer()));
  const [audioApi, solverApi, replayApi] = await Promise.all([0,1,2].map(()=>libcsound({withPlugins:[plugin]})));
  const settings = {source:bellSource(),sourceRate:44100,controls:[...controlDefaults]};
  const records: object[] = [];
  let artifact: Uint8Array | undefined;
  async function run(mode: string, grid: number) {
    const queues = {grid,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(grid)).buffer};
    let gpu: GPUFieldSolver | undefined;
    const provider = mode.startsWith("gpu") ? await GPUProvider.create(solverApi,browserConfig(true,grid),queues,
      async profile => { gpu = await GPUFieldSolver.create(profile);return gpu; }) :
      new SolverEngine(solverApi,browserConfig(true,grid),queues);
    const capture = new ReplayCapture(audioApi,settings,includes,queues,build,true);
    const heap = audioApi.getMemory().buffer;
    const original: Float64Array[] = [];
    let tape: Uint8Array;
    try {
      for (let block=0;block<750;block++) {
        if(mode.includes("recovery")) {
          capture.reset(block===520);
          if(block===440)capture.resume();
          if(block===480)new SharedRing(commandBytes,queues.commands).requestRecovery();
        }
        if(mode==="gpu-loss" && block===250) {
          check(gpu,"GPU was never prepared");
          const device = Reflect.get(gpu,"device") as GPUDevice;
          device.destroy();await device.lost;
        }
        original.push(capture.render(block%16===0));
        if(block%16===0)capture.engine.view();
        if(!(mode.includes("recovery") && block>=100 && block<430))await provider.work();
      }
      check(capture.engine.stats().snapshot_sequence>5,"No applied fields in capture");
      check(capture.engine.bridge!.rejected===0,"Unexpected ingress rejection");
      check(audioApi.getMemory().buffer===heap,"Capture grew WASM heap");
      tape=await capture.finish();
    }finally{provider.destroy();capture.destroy();}
    // No provider exists during replay; pacing and solver availability differ.
    const player=await ReplayPlayer.create(replayApi,includes,tape,build);
    const replayHeap=replayApi.getMemory().buffer;
    try {
      for(let block=0;block<750;block++) {
        if(block%137===0)await new Promise(resolve=>setTimeout(resolve,1));
        const pcm=player.render();
        check(pcm.every((value,i)=>value===original[block]![i]),"PCM changed at block "+block);
      }
      await player.verify();
      check(replayApi.getMemory().buffer===replayHeap,"Replay grew WASM heap");
    }finally{player.destroy();}
    const data=JSON.parse(new TextDecoder().decode(tape));
    const accepted=data.blocks.filter((event: {accepted?: unknown})=>event.accepted);
    check(accepted.length>10,"Missing acceptance records");
    check(accepted.some((event: {frame:number;accepted:{frame:string}})=>BigInt(event.accepted.frame)!==BigInt(event.frame)),
      "Acceptance was incorrectly rounded to host blocks");
    if(mode==="gpu-loss")check(accepted.some((event: {accepted:{epoch:string}})=>event.accepted.epoch==="2"),"Missing fallback epoch");
    records.push({mode,grid,bytes:tape.length,acceptances:accepted.length,exactPCM:true,solverFreeReplay:true});
    postMessage({type:"progress",name:mode,grid});
    if(mode==="gpu-loss")artifact=tape;
    if(mode==="cpu") {
      // Capture must be read-only, not just self-consistent with replay.
      const baselineQueues={grid,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(grid)).buffer};
      const baseline=new OfflineEngine(audioApi,settings,includes,false,baselineQueues);
      const solver=new SolverEngine(solverApi,browserConfig(true,grid),baselineQueues);
      try {for(let block=0;block<750;block++) {
        check(baseline.render(1).every((value,i)=>value===original[block]![i]),"Capture changed sound");
        solver.work();
      }}finally{solver.destroy();baseline.destroy();}
      async function invalid(change:(value: typeof data)=>void, message: string) {
        const damaged=JSON.parse(JSON.stringify(data));change(damaged);
        let rejected=false, replay: ReplayPlayer | undefined;
        try {replay=await ReplayPlayer.create(replayApi,includes,new TextEncoder().encode(JSON.stringify(damaged)),build);
          for(let block=0;block<750;block++)replay.render();await replay.verify();
        }catch{rejected=true;}finally{replay?.destroy();}
        check(rejected,message);
      }
      await invalid(v=>v.build="0".repeat(64),"Wrong build accepted");
      await invalid(v=>v.sourceSha256="0".repeat(64),"Changed source accepted");
      await invalid(v=>v.frames=31*48000,"Unbounded duration accepted");
      await invalid(v=>v.blocks.reverse(),"Reordered frames accepted");
      await invalid(v=>v.blocks.find((b: {accepted?:unknown})=>b.accepted).accepted.frame="18446744073709551616","Overflow clock accepted");
      await invalid(v=>v.blocks.find((b: {accepted?:unknown})=>b.accepted).accepted.frame="0","Wrong acceptance frame accepted");
      await invalid(v=>v.pcmSha256="0".repeat(64),"Wrong PCM hash accepted");
      await invalid(v=>v.blocks.find((b: {command?:unknown})=>b.command).command=btoa("x".repeat(commandBytes)),"Changed controls accepted");
    }
  }
  for(const grid of [16,32,64])await run("cpu-recovery",grid);
  await run("cpu",16);await run("gpu",16);await run("gpu-loss",32);
  // A noisy 64-square ingress stream must fail explicitly at the archive cap,
  // not return a truncated take that looks successful. No solver is running.
  const pressureQueues={grid:64,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(64)).buffer};
  const pressure=new ReplayCapture(audioApi,settings,includes,pressureQueues,build);
  const writer=new SharedRing(packetBytes(64),pressureQueues.fields);
  const malformed=new Uint8Array(packetBytes(64));
  let capped=false,blocks=0;
  try {
    for(;blocks<1000;blocks++){check(writer.push(malformed),"Unexpected full ingress");pressure.render();}
  }catch(error){capped=String(error).includes("64 MiB");}
  check(capped && blocks<1000,"Recording silently exceeded its archive cap");
  let truncated=false;
  try{await pressure.finish();truncated=true;}catch{}
  pressure.destroy();check(!truncated,"Failed recording exported a partial take");
  check(artifact,"No replay artifact");
  return {records,checks:10,captureLimitBlocks:blocks,build,artifact:Array.from(artifact),scope:"Offline exact accepted-field replay; no live recording claim"};
}
main().then(result=>postMessage({type:"done",result})).catch(error=>postMessage({type:"error",message:error.stack??String(error)}));
