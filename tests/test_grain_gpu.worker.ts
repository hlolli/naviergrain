import {GrainPlanHost} from '../web/grain-plan';
import {GrainRenderer} from '../web/grain-renderer';
import {bellSource,browserConfig,type CsoundApi} from '../web/engine';
import {controlDefaults,control,config} from '../web/naviergrain-schema';
function check(v:unknown,message:string): asserts v {if(!v)throw new Error(message);}
Object.defineProperty(globalThis,'window',{value:{atob:globalThis.atob.bind(globalThis),btoa:globalThis.btoa.bind(globalThis),webkitAudioContext:undefined}});
async function main() {
 const {libcsound}=await import(new URL('./csound.js',import.meta.url).href) as {libcsound(options:{withPlugins:ArrayBuffer[]}):Promise<CsoundApi>};
 const plugin=await(await fetch('./naviergrain.wasm')).arrayBuffer();
 const shader=await(await fetch('./grain-plan.wgsl')).text();
 const records:object[]=[];
 async function adapter() {
  for(let i=0;i<4;i++){const a=await navigator.gpu?.requestAdapter();if(a)return a;await new Promise(r=>setTimeout(r,500));}
  throw new Error('Hardware adapter unavailable');
 }
 for(const test of [
  {name:'normal',ratio:1,capacity:128,frames:32,blocks:192},
  {name:'high-rate',ratio:4,capacity:128,frames:32,blocks:128},
  {name:'tiny-nonloop',ratio:.5,capacity:64,frames:7,blocks:384},
  {name:'dense',ratio:1,capacity:1024,frames:32,blocks:1536},
  ...(['loss','readback','timeout','missing','shader'] as const).map(fault=>({name:fault,ratio:2,capacity:128,frames:32,blocks:128,fault})),
 ]) {
  const cfg=browserConfig(false,16);cfg[config.max_grains]=test.capacity;cfg[config.source_loop]=Number(test.name!=='tiny-nonloop');
  const c=[...controlDefaults];c[control.grain_rate]=test.name==='dense'?2000:800;c[control.grain_ms]=500;c[control.gain]=.15;
  const input={source:test.name==='tiny-nonloop'?Float64Array.from({length:31},(_,i)=>.3*Math.sin(i)):bellSource(),sourceRate:48000*test.ratio,controls:c};
  const cpu=new GrainPlanHost(await libcsound({withPlugins:[plugin]}),input,cfg);
  const host=new GrainPlanHost(await libcsound({withPlugins:[plugin]}),input,cfg);
  const a=test.name==='missing'?null:await adapter();const adapterInfo=a?{vendor:a.info.vendor,architecture:a.info.architecture}:null;
  const renderer=await GrainRenderer.create(host,a,test.name==='shader'?'invalid shader':shader);
  if(test.name!=='missing'&&test.name!=='shader')check(renderer.mode==='gpu','GPU preparation fell back: '+renderer.failureReason);
  let maximum=0,energy=0,squaredError=0,bytes=0,gpuBatches=0,cpuBatches=0,exactAfterFailure=true;
  let captureMs=0,playbackMs=0,commitMs=0,cpuMs=0,recordsCount=0,peakVoices=0;
  const pcm=new Float64Array(test.blocks*test.frames*2);
  try {
   for(let block=0;block<test.blocks;block++) {
    if(test.name!=='dense') {
     c[control.pitch_ratio]=block<40?.25:block<90?4:1;
     c[control.stereo_width]=block<55?.2:.9;
     c[control.freeze]=Number(block>=60&&block<80);
     c[control.reset]=Number(block===100);
     cpu.controls(c);host.controls(c);
    }
    const before=performance.now();const exact=cpu.capture(test.frames);const expected=cpu.fallback(exact);cpu.commit(exact,expected);cpuMs+=performance.now()-before;
    const fault='fault' in test && block===50 && ['loss','readback','timeout'].includes(test.fault)?test.fault as 'loss'|'readback'|'timeout':undefined;
    const output=await renderer.render(test.frames,fault);
    check(output.start===BigInt(block*test.frames),'Audio clock gap');
    recordsCount+=output.records;peakVoices=Math.max(peakVoices,output.peakVoices);bytes+=output.bytes;captureMs+=output.captureMs;playbackMs+=output.playbackMs;commitMs+=output.commitMs;
    if(output.mode==='gpu')gpuBatches++;else cpuBatches++;
    for(let i=0;i<expected.length;i++) {
     const delta=output.pcm[i]!-expected[i]!;
     check(Number.isFinite(output.pcm[i]),'Non-finite output');maximum=Math.max(maximum,Math.abs(delta));
     energy+=expected[i]!**2;squaredError+=delta*delta;
     if(output.mode==='cpu'&&delta!==0)exactAfterFailure=false;
    }
    pcm.set(output.pcm,block*test.frames*2);
   }
   check(maximum<1e-5,`GPU audio error ${maximum}`);check(energy>0,'Silent reference');
   check(exactAfterFailure,'CPU recovery differs from continuous reference');
   if('fault' in test)check(cpuBatches>0&&renderer.fallbacks===1,'Missing fallback');else check(cpuBatches===0,'Unexpected CPU substitution');
   const row={...test,adapter:adapterInfo,maximum,relativeRms:Math.sqrt(squaredError/energy),bytes,gpuBatches,cpuBatches,exactAfterFailure,
    captureMs,playbackMs,commitMs,cpuMs,records:recordsCount,peakVoices,exactDescriptorBytes:recordsCount*48};records.push(row);
   const encoded=new Float32Array(pcm);postMessage({type:'progress',row,pcm:encoded},[encoded.buffer]);
  } finally {renderer.destroy();cpu.destroy();}
 }
 // Teardown while mapping must never regenerate/commit into the destroyed C host.
 const input={source:bellSource(),sourceRate:48000,controls:[...controlDefaults]};
 const host=new GrainPlanHost(await libcsound({withPlugins:[plugin]}),input);
 const renderer=await GrainRenderer.create(host,await adapter(),shader);
 const pending=renderer.render();let busyRejected=false;try{await renderer.render();}catch{busyRejected=true;}
 renderer.destroy();let cancelled=false;try{await pending;}catch{cancelled=true;}
 check(busyRejected&&cancelled,'Busy/teardown ownership failed');renderer.destroy();
 return {records,busyRejected,cancelled};
}
main().then(result=>postMessage({type:'complete',result})).catch(error=>postMessage({type:'error',message:String(error.stack||error)}));
