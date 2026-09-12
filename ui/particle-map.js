export const PARTICLE_HEADER=10, PARTICLE_STRIDE=16;
export function validateParticles(p) {
  if(p.length<PARTICLE_HEADER||p[0]!==2||p[1]!==PARTICLE_STRIDE||!Number.isInteger(p[2])||p[2]<0||p[2]>4096||
    p.length!==PARTICLE_HEADER+p[2]*PARTICLE_STRIDE||!Array.from(p).every(Number.isFinite)||p[8]<=0||p[9]<p[8])throw new Error('Invalid grain snapshot');
}
// World coordinates already drive the oscillator and stereo gains in C.
// Camera transforms affect only the view, never the instrument's pan axis.
export function particlePosition(record) {return [record.px,record.py,record.pz];}
