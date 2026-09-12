/** Measurement only: separate audio/provider workers; no provider waits on audio. */
import {GPUProvider} from '../web/gpu-provider';
import {GPUFieldSolver} from '../web/gpu-solver';
import {SharedRing, commandBytes, packetBytes, SolverEngine, type BridgeSetup} from '../web/bridge';
import {OfflineEngine, bellSource, browserConfig, type CsoundApi} from '../web/engine';
import {controlDefaults, control as C} from '../web/fluidgrain-schema';
Object.defineProperty(globalThis,'window',{value:{atob:atob.bind(globalThis),btoa:btoa.bind(globalThis),webkitAudioContext:undefined}});
type Request = {role:'audio'|'provider'; mode:'external'|'gpu'; profile:'default'|'swirl'; fault:'none'|'stall'|'failure'; seconds:number; setup:BridgeSetup; clock:SharedArrayBuffer};
const sleep = (ms:number) => new Promise<void>(resolve=>setTimeout(resolve,ms));
function check(ok:unknown,message:string):asserts ok {if(!ok)throw new Error(message);}
function distribution(values:number[]) {
  const sorted=values.slice().sort((a,b)=>a-b);
  return {count:values.length,mean:values.reduce((a,b)=>a+b,0)/(values.length||1),
    p50:sorted[Math.max(0,Math.ceil(values.length*.5)-1)]??0,
    p99:sorted[Math.max(0,Math.ceil(values.length*.99)-1)]??0,max:sorted.at(-1)??0};
}
let startRun:()=>void;
const armed=new Promise<void>(resolve=>{startRun=resolve;});
let started=false;
async function run(r:Request) {
  check(['audio','provider'].includes(r.role)&&['external','gpu'].includes(r.mode)&&
    ['default','swirl'].includes(r.profile)&&['none','stall','failure'].includes(r.fault)&&
    Number.isInteger(r.seconds)&&r.seconds>=6&&r.seconds<=30,'Invalid paced run');
  const clock=new Int32Array(r.clock), cold=performance.now();
  const {libcsound}=await import(new URL('./csound.js',import.meta.url).href) as {
    libcsound(options:{withPlugins:ArrayBuffer[]}):Promise<CsoundApi>};
  const response=await fetch('./fluidgrain.wasm');check(response.ok,'Missing plugin');
  const api=await libcsound({withPlugins:[await response.arrayBuffer()]});
  const faultFrame=Math.floor(r.seconds/3)*48000;
  let provider:SolverEngine|GPUProvider|undefined, engine:OfflineEngine|undefined;
  const services:number[]=[], telemetry:unknown[]=[];
  let injected=false, recovered=false, faultAt:number|undefined;
  try {
    if(r.role==='provider') {
      provider=r.mode==='gpu'?await GPUProvider.create(api,browserConfig(true,32),r.setup,async profile=>{
        const gpu=await GPUFieldSolver.create(profile);
        return {adapterInfo:gpu.adapterInfo,get failure(){return gpu.failure;},reset:()=>gpu.reset(),destroy:()=>gpu.destroy(),
          step:controls=>{
            if(r.fault==='failure'&&!injected&&Atomics.load(clock,0)>=faultFrame) {
              injected=true;faultAt=Atomics.load(clock,0);throw new Error('Benchmark injected provider-step failure');
            }
            return gpu.step(controls);
          }};
      }):new SolverEngine(api,browserConfig(true,32),r.setup);
      if(provider instanceof GPUProvider)check(provider.report().backend==='gpu','Requested GPU unavailable; refusing startup substitution');
    } else {
      const controls:number[]=[...controlDefaults];controls[C.scheduler]=0;
      if(r.profile==='swirl') {
        controls[C.grain_rate]=600;controls[C.grain_ms]=180;controls[C.turbulence]=.6;
        controls[C.strain_drive]=.4;controls[C.confinement]=.15;controls[C.inertia_ms]=80;
      }
      const includes=await fetch('./prepared.inc');check(includes.ok,'Missing includes');
      engine=new OfflineEngine(api,{source:bellSource(),sourceRate:48000,controls},await includes.text(),false,r.setup);
    }
    const prepareMs=performance.now()-cold,heap=api.getMemory().buffer;
    postMessage({type:'ready',role:r.role,prepareMs,provider:provider instanceof GPUProvider?provider.report():undefined});
    await armed;
    const began=performance.now();
    if(provider) {
      let last=0;
      while(!Atomics.load(clock,1)) {
        const frame=Atomics.load(clock,0);
        if(r.fault==='stall'&&frame>=faultFrame&&frame<faultFrame+48000) {
          if(!injected){injected=true;faultAt=frame;}
          await sleep(1);continue;
        }
        const deadline=performance.now()+8;
        for(let count=0;count<4&&performance.now()<deadline&&!Atomics.load(clock,1);count++) {
          const begin=performance.now();
          if(!await provider.work())break;
          services.push(performance.now()-begin);
          if(injected&&frame>=faultFrame+48000)recovered=true;
        }
        if(performance.now()-last>=1000) {
          last=performance.now();telemetry.push({frame,report:provider instanceof GPUProvider?provider.report():undefined});
        }
        await sleep(1);
      }
      const report=provider instanceof GPUProvider?provider.report():undefined;
      check(!report?.rejected,'Rejected provider command');
      if(report&&r.fault!=='failure')check(report.cpuFields===0&&report.gpuFields>5,'Unexpected GPU fallback');
      if(r.fault==='failure')check(injected&&report&&report.gpuFields>5&&report.cpuFields>5&&report.waitingEpoch===null,'Failover did not finish');
      if(r.fault==='stall')check(injected&&recovered,'Stall not exercised/recovered');
      check(heap===api.getMemory().buffer,'Provider heap grew');
      provider.destroy();provider=undefined;
      postMessage({type:'done',role:r.role,record:{prepareMs,elapsedMs:performance.now()-began,serviceMs:distribution(services),report,injected,recovered,faultAt,telemetry,heapUnchanged:true}});
    } else if(engine) {
      const pcm=new Float64Array(r.seconds*48000*2), callbacks:number[]=[],lateness:number[]=[],ages:number[]=[];
      const period=256/48;
      let due=performance.now(),missedBatches=0,maximumLive=0,peak=0;
      for(let block=0;block<r.seconds*750;block+=4) {
        // Timer-driven mock sink: no catch-up bursts and no wait for provider progress.
        while(performance.now()<due)await sleep(Math.ceil(due-performance.now()));
        const beganBatch=performance.now();lateness.push(Math.max(0,beganBatch-due));
        for(let i=0;i<4&&block+i<r.seconds*750;i++) {
          const begin=performance.now();const samples=engine.render(1);pcm.set(samples,(block+i)*128);
          callbacks.push(performance.now()-begin);
          const stats=engine.stats();ages.push(stats.field_age_ms);maximumLive=Math.max(maximumLive,stats.live_grains);
          if((block+i+1)%750===0) {
            telemetry.push({second:(block+i+1)/750,wallMs:performance.now()-began,...stats,
              commands:engine.bridge!.outgoing.stats(),fields:engine.bridge!.incoming.stats()});
            if((block+i+1)%3750===0)postMessage({type:'progress',second:(block+i+1)/750});
          }
        }
        Atomics.store(clock,0,Math.min(block+4,r.seconds*750)*64);
        if(performance.now()-beganBatch>period)missedBatches++;
        due=Math.max(due+period,beganBatch+period,performance.now());
      }
      Atomics.store(clock,1,1);
      check(pcm.every(Number.isFinite)&&pcm.some(v=>Math.abs(v)>.0001),'Invalid/silent audio');
      for(const v of pcm)peak=Math.max(peak,Math.abs(v));
      const stats=engine.stats(),commands=engine.bridge!.outgoing.stats(),fields=engine.bridge!.incoming.stats();
      check(stats.voice_drops===0&&stats.numeric_interventions===0&&!engine.bridge!.rejected,'Audio safety failure');
      check(heap===api.getMemory().buffer,'Audio heap grew');
      if(r.fault==='stall')check(commands.dropped>0,'Stall failed to fill command queue');
      const record={prepareMs,elapsedMs:performance.now()-began,frames:pcm.length/2,callbackMs:distribution(callbacks),
        timerLatenessMs:distribution(lateness),fieldAgeMs:distribution(ages),missedBatches,
        callbacksOverNominalPeriod:callbacks.filter(v=>v>64/48).length,underruns:null,
        maximumLive,peak,stats,commands,fields,telemetry,heapUnchanged:true};
      engine.destroy();engine=undefined;
      postMessage({type:'done',role:r.role,record,pcm},{transfer:[pcm.buffer]});
    }
  } finally {Atomics.store(clock,1,1);provider?.destroy();engine?.destroy();}
}
globalThis.onmessage=({data}:MessageEvent<Request|{type:'go'}>)=>{
  if('type'in data){startRun();return;}
  if(started){postMessage({type:'error',message:'Worker already started'});return;}
  started=true;
  void run(data).catch(error=>postMessage({type:'error',message:error instanceof Error?error.stack:String(error)}));
};
