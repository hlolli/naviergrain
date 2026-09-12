import assert from 'node:assert/strict';
import {particlePosition,validateParticles} from '../ui/particle-map.js';
import {FlowView} from '../ui/flow.js';
assert.doesNotThrow(()=>validateParticles([2,16,0,256,48000,0,0,0,55,12000]));
assert.throws(()=>validateParticles([2,16,1,256,48000,0,0,0,55,12000]));
assert.throws(()=>validateParticles([2,16,4097,256,48000,0,0,0,55,12000]));
assert.throws(()=>validateParticles([1,12,0,4096,48000,0,0,0]));
assert.throws(()=>validateParticles([2,16,0,256,48000,0,0,0,12000,55]));
// The renderer must preserve the engine's position, regardless of frequency.
const record={px:-.4,py:.9,pz:.3,hz:110};
assert.deepEqual(particlePosition(record),[-.4,.9,.3]);
assert.deepEqual(particlePosition({...record,hz:8000}),particlePosition(record));
// Exercise snapshot matching without a WebGL context. A wrapped particle keeps
// its identity, but its trail must end at the edge of the displayed chart.
const view=Object.assign(Object.create(FlowView.prototype),{previous:new Map(),draw(){},lastTime:0});
function snapshot(x,y,pathX=x,pathY=y,id=1,epoch=0){
  return [2,16,1,256,48000,1,epoch,0,55,12000,
    id,0,x,y,pathX,pathY,0,0,.5,440,.2,500,x,(y-.5)*3.6,0,.5];
}
view.accept(snapshot(.4,.99));
view.accept(snapshot(.4,.995));
assert(view.records[0].before,'Ordinary motion keeps its trail');
view.accept(snapshot(.4,.005,.4,1.005));
assert.equal(view.records[0].before,undefined,'No vertical trail across the y seam');
view.accept(snapshot(.4,.002,.4,1.002));
assert(view.records[0].before,'Trails resume after the seam');
view.accept(snapshot(.4,.998,.4,.998));
assert.equal(view.records[0].before,undefined,'No trail across the reverse y seam');
view.accept(snapshot(.99,.998,.99,.998));
view.accept(snapshot(.01,.998,1.01,.998));
assert.equal(view.records[0].before,undefined,'No trail across the x seam');
view.accept(snapshot(.02,.998,2.02,.998));
assert.equal(view.records[0].before,undefined,'Skipped snapshots still detect whole wraps');
view.accept(snapshot(.03,.998,2.03,.998,2));
assert.equal(view.records[0].before,undefined,'New grains cannot inherit a trail');
view.accept(snapshot(.04,.998,2.04,.998,2,1));
assert.equal(view.records[0].before,undefined,'Reset epochs cannot inherit a trail');
console.log('Particle view: versioned snapshots and unchanged engine world coordinates passed');
