// A muted producer/consumer probe: the consumer drains at 48 kHz wall time.
// Run on an idle machine; this is a performance check, not a portable unit test.
import fs from 'node:fs';
import {performance} from 'node:perf_hooks';
import {setTimeout as delay} from 'node:timers/promises';
import {liveControls} from '../ui/live-controls.js';
const [file='build/ui/fluidgrain-live.wasm',rate='32000',duration='80',views='1',stress='0']=process.argv.slice(2);
const schema=JSON.parse(fs.readFileSync(new URL('../schema/fluidgrain-v1.json',import.meta.url)));
schema.control=liveControls(schema.control);
const liveStat=schema.stat.findIndex(s=>s.name==='live_grains');
const {instance:{exports:w}}=await WebAssembly.instantiate(fs.readFileSync(file),{});
const live=w.fg_live_create(48000,-1);
if(!live)throw Error('Cannot create live engine');
w.fg_live_control(live,1,Number(rate));w.fg_live_control(live,2,Number(duration));w.fg_live_control(live,20,0);
if(Number(stress)){
  for(const i of [3,6,8,9,10,11,12,13,14,17,18,19,20])
    if(!w.fg_live_control(live,i,schema.control[i].max))throw Error('Invalid stress control');
}
let queued=2048,under=0,peak=0,renderMs=0,maxBatch=0;
for(let i=0;i<4;i++)w.fg_live_render(live,512);
let last=performance.now();const start=last;
for(let batch=0;batch<192;batch++){
  if(queued>=2048)await delay((queued-1536)/48);
  const before=performance.now();
  if(!w.fg_live_render(live,512))throw Error('Render failed');
  if(Number(views)&&batch%2===0){w.fg_live_particles(live);w.fg_live_spectrum(live);}
  const now=performance.now(),cost=now-before;renderMs+=cost;maxBatch=Math.max(maxBatch,cost);
  queued-=(now-last)*48;if(queued<0){under++;queued=0;}queued+=512;last=now;
  peak=Math.max(peak,new Float64Array(w.memory.buffer,w.fg_live_stats(live),20)[liveStat]);
}
w.fg_live_destroy(live);
console.log(JSON.stringify({rate:Number(rate),duration:Number(duration),views:!!Number(views),stress:!!Number(stress),peakVoices:peak,renderMs:+renderMs.toFixed(1),audioMs:2048,maxBatchMs:+maxBatch.toFixed(2),wallMs:+(performance.now()-start).toFixed(1),starvedBatches:under}));
if(under||renderMs>2048*.8)throw Error('Producer missed the audio queue budget (requires 20% headroom)');
