import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {liveControls,liveControlGroups} from '../ui/live-controls.js';

const schema=JSON.parse(readFileSync(new URL('../schema/naviergrain-v1.json',import.meta.url)));
const controls=liveControls(schema.control);
const module=await WebAssembly.compile(readFileSync(process.argv[2]));
const w=(await WebAssembly.instantiate(module,{})).exports;

// Exercise the same live API as the worker, including changes during playback.
function render(index,value,applyDefaults=true) {
  const live=w.ng_live_create(48000,-1);assert(live);
  try {
    if(applyDefaults)controls.forEach((c,i)=>assert(w.ng_live_control(live,i,c.default)));
    const bands=new Float64Array(96);let energy=0,samples=0,voices=0,length=0,particles=0,observations=0;
    for(let b=0;b<563;++b){
      if(b===188&&index!==undefined)assert(w.ng_live_control(live,index,value));
      assert(w.ng_live_render(live,512));
      if(b<376)continue;
      for(const x of new Float64Array(w.memory.buffer,w.ng_live_audio(live),1024)){
        assert(Number.isFinite(x));energy+=x*x;++samples;
      }
      if(b%8===0){
        const spectrum=new Float64Array(w.memory.buffer,w.ng_live_spectrum(live),96);
        for(let i=0;i<96;++i)bands[i]+=10**(spectrum[i]/10);
        const p=w.ng_live_particles(live),header=new Float64Array(w.memory.buffer,p,10);
        const values=new Float64Array(w.memory.buffer,p,10+header[2]*16);
        voices+=header[2];++observations;
        for(let i=0;i<header[2];++i){length+=values[10+i*16+11];++particles;}
      }
    }
    const stats=Array.from(new Float64Array(w.memory.buffer,w.ng_live_stats(live),20));
    return {bands:Array.from(bands,x=>x/observations),rms:Math.sqrt(energy/samples),voices:voices/observations,lifetime:length/particles,drops:stats[9]};
  } finally {w.ng_live_destroy(live);}
}

const baseline=render();
assert.equal(baseline.drops,0,'Default settings must leave room in the live voice pool');
assert(baseline.voices>20&&baseline.voices<180,'Default should sustain a cloud without filling the pool');
assert(baseline.lifetime>100,'Default grains should retain distinct, narrow tones');
assert(baseline.rms>.005&&baseline.rms<.04,'Default should be audible at a gentle level');
assert.deepEqual(render(undefined,undefined,false),baseline,'C and UI live defaults must render the same audio');
console.log('Default cloud:',JSON.stringify(baseline, (key,value)=>key==='bands'?undefined:value));

for(const index of Object.values(liveControlGroups).flat()){
  const c=controls[index],value=c.default>(c.min+c.max)/2?c.min:c.max;
  const changed=render(index,value);
  const power=baseline.bands.reduce((a,x)=>a+x,0);
  const difference=changed.bands.reduce((a,x,i)=>a+Math.abs(x-baseline.bands[i]),0)/power;
  // A sign inversion changes every sample but not the sound. Compare spectral
  // power instead, so a polarity-only control cannot pass this audit.
  assert(difference>.005,`${c.name}: no measurable acoustic effect (${difference})`);
  console.log(`${c.name}: spectral power difference ${difference.toFixed(3)}`);
}
