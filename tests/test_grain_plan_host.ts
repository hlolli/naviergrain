import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {GrainPlanHost} from '../web/grain-plan';
import {OfflineEngine,browserConfig,bellSource,type CsoundApi} from '../web/engine';
import {controlDefaults,control} from '../web/fluidgrain-schema';
const entry=await readFile('../csound-wasm-plugin-compiler/node_modules/@csound/browser/dist/csound.js','utf8');
const marker='const Csound = kd; const libcsound = __lcs__; export { Csound, libcsound }; export default Csound;';
assert(entry.includes(marker));
Object.defineProperty(globalThis,'window',{value:{atob,btoa,webkitAudioContext:undefined},configurable:true});
const factory=new Function(entry.replace(marker,'return __lcs__;'))() as (options:{withPlugins:ArrayBuffer[]})=>Promise<CsoundApi>;
const plugin=await readFile('build/wasm/fluidgrain.wasm');
const make=()=>factory({withPlugins:[plugin.slice().buffer]});
const includes=await readFile('build/web/prepared.inc','utf8');
const rows=[];
for(const ratio of [.5,1,2]) {
 const c=[...controlDefaults];c[control.grain_rate]=800;c[control.grain_ms]=200;c[control.gain]=.15;
 const input={source:bellSource(),sourceRate:48000*ratio,controls:c};
 const ref=new OfflineEngine(await make(),input,includes);
 const host=new GrainPlanHost(await make(),input);let packed=0,peak=0,energy=0;
 try {
  input.source.fill(0);
  for(let block=0;block<160;block++) {
   c[control.reset]=Number(block>=70&&block<72);host.controls(c);ref.reset(Boolean(c[control.reset]));
   const expected=ref.render(1);
   for(let half=0;half<2;half++) {
    const batch=host.capture();assert.equal(batch.start,BigInt(block*64+half*32));packed+=batch.packet.length;
    assert.throws(()=>host.capture());assert.throws(()=>host.controls(c));
    const pcm=host.fallback(batch);assert.deepEqual(host.fallback(batch),pcm);
    assert.throws(()=>host.commit({...batch},pcm));
    assert.deepEqual(host.commit(batch,pcm),expected.slice(half*64,half*64+64));
    assert.throws(()=>host.commit(batch,pcm));
    for(const x of pcm){peak=Math.max(peak,Math.abs(x));energy+=x*x;}
   }
  }
  assert(energy>0);const last=host.capture(7);assert.equal(host.fallback(last).length,14);
  host.destroy();assert.throws(()=>host.fallback(last));host.destroy();
  rows.push({ratio,batches:320,packed,peak,exact:true});
 } finally {host.destroy();ref.destroy();}
}
for(const frames of [7,128,512]) {
 const c=[...controlDefaults];c[control.grain_rate]=1600;c[control.grain_ms]=120;c[control.gain]=.15;
 const input={source:bellSource(),sourceRate:48000,controls:c};const cfg=browserConfig(false,16);
 const ref=new GrainPlanHost(await make(),input,cfg);
 const host=new GrainPlanHost(await make(),input,cfg,512);let clock=0,energy=0;
 try {
  assert.throws(()=>host.capture(513));assert.throws(()=>host.capture(0));
  for(let block=0;block<48;block++) {
   c[control.reset]=Number(block===20);c[control.freeze]=Number(block>=27&&block<30);
   c[control.pitch_ratio]=block<12?.25:4;host.controls(c);ref.controls(c);
   const expected=new Float64Array(frames*2);
   for(let f=0;f<frames;f+=32){const part=ref.capture(Math.min(32,frames-f));const pcm=ref.fallback(part);expected.set(ref.commit(part,pcm),f*2);}
   const batch=host.capture(frames);assert.equal(batch.start,BigInt(clock));
   const packet=new DataView(batch.packet.buffer);assert.equal(packet.getUint32(4,true),frames<=32?1:2);
   assert.throws(()=>host.controls(c));assert.throws(()=>host.capture());
   const pcm=host.fallback(batch);assert.deepEqual(host.fallback(batch),pcm);
   assert.throws(()=>host.commit(batch,new Float64Array(1)));
   assert.deepEqual(host.commit(batch,pcm),expected);assert.throws(()=>host.fallback(batch));
   clock+=frames;for(const x of pcm)energy+=x*x;
  }
  assert(energy>0);rows.push({frames,batches:48,clock,exact:true});
 }finally{host.destroy();ref.destroy();}
}
await writeFile('build/delivery-wasm-host.json' ,JSON.stringify({status:'complete',rows},null,2)+'\n');
console.log(JSON.stringify(rows));
