import assert from 'node:assert/strict';
import {particlePosition,validateParticles} from '../ui/particle-map.js';
assert.doesNotThrow(()=>validateParticles([2,16,0,256,48000,0,0,0,55,12000]));
assert.throws(()=>validateParticles([2,16,1,256,48000,0,0,0,55,12000]));
assert.throws(()=>validateParticles([2,16,4097,256,48000,0,0,0,55,12000]));
assert.throws(()=>validateParticles([1,12,0,4096,48000,0,0,0]));
assert.throws(()=>validateParticles([2,16,0,256,48000,0,0,0,12000,55]));
// The renderer must preserve the engine's position, regardless of frequency.
const record={px:-.4,py:.9,pz:.3,hz:110};
assert.deepEqual(particlePosition(record),[-.4,.9,.3]);
assert.deepEqual(particlePosition({...record,hz:8000}),particlePosition(record));
console.log('Particle view: versioned snapshots and unchanged engine world coordinates passed');
