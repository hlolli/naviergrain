import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import {FlowView} from '../ui/flow.js';

// Run real engine snapshots through the renderer's trail matching. Long-lived
// grains cross the periodic boundary often enough to expose smoothed wraps.
const wasmFile=process.argv[2];
assert(wasmFile,'Pass the live WASM file');
const w=(await WebAssembly.instantiate(readFileSync(wasmFile),{})).instance.exports;
const view=Object.assign(Object.create(FlowView.prototype),{previous:new Map(),draw(){},lastTime:0});
const live=w.fg_live_create(48000,-1);
assert(live);
let previous=new Map(),crossings=0,falseFlights=0;
try {
  assert(w.fg_live_control(live,2,500));
  for(let block=0;block<1400;++block){
    assert(w.fg_live_render(live,512));
    const pointer=w.fg_live_particles(live),header=new Float64Array(w.memory.buffer,pointer,10);
    const p=new Float64Array(w.memory.buffer,pointer,10+header[2]*16),next=new Map();
    view.accept(p);
    for(let i=0;i<p[2];++i){
      const r=Array.from(p.slice(10+i*16,26+i*16)),id=r[0]+':'+r[1],old=previous.get(id);
      next.set(id,r);
      if(!old)continue;
      if(Math.floor(r[5])!==Math.floor(old[5])){
        ++crossings;
        assert.equal(view.records[i].before,undefined,'End the trail at the periodic seam');
      }
      const dy=r[13]-old[13],dx=r[12]-old[12],dz=r[14]-old[14];
      // In this fixed scene, the bug traverses over 0.3 world units vertically
      // per 10.7 ms snapshot. It persists after the boundary-crossing snapshot.
      if(view.records[i].before&&Math.abs(dy)>.3&&Math.hypot(dx,dz)<Math.abs(dy)*.5)
        ++falseFlights;
    }
    previous=next;
  }
  assert(crossings>100,'Exercise repeated wraps in the live engine');
  assert.equal(falseFlights,0,'No smoothed vertical flights after a periodic wrap');
  console.log(`Live trails: ${crossings} boundary crossings, no false vertical flights`);
} finally {
  w.fg_live_destroy(live);
}
