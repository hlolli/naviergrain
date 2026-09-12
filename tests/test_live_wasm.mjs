import {readFileSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import assert from 'node:assert/strict';
import {tmpdir} from 'node:os';
import {mkdtempSync,rmSync} from 'node:fs';
import {join} from 'node:path';
const [wasmFile,reference]=process.argv.slice(2);
assert(wasmFile&&reference,'Pass the WASM file and native live-test executable');
const temporary=mkdtempSync(join(tmpdir(),'fluidgrain-live-'));
try {
  const output=join(temporary,'reference.bin');
  const run=spawnSync(reference,[output],{encoding:'utf8'});assert.equal(run.status,0,run.stderr);
  const native=readFileSync(output);
  const module=await WebAssembly.compile(readFileSync(wasmFile));
  assert.deepEqual(WebAssembly.Module.imports(module),[],'Live WASM must need no host filesystem or clock');
  const w=(await WebAssembly.instantiate(module,{})).exports;
  const live=w.fg_live_create(48000,-1);assert(live);let offset=0,error=0;const history=[];
  function compare(values,tolerance){for(const value of values){const expected=native.readDoubleLE(offset);offset+=8;assert(Number.isFinite(value));error=Math.max(error,Math.abs(value-expected));assert(Math.abs(value-expected)<tolerance,`${value} != ${expected}`);}}
  for(let b=0;b<32;++b){
    assert(w.fg_live_control(live,3,55));assert(w.fg_live_control(live,10,b<12?.7:-.7));
    assert(w.fg_live_control(live,21,Number(b>=16&&b<20)));if(b===24)assert(w.fg_live_control(live,22,1));
    assert(w.fg_live_render(live,512));compare(new Float64Array(w.memory.buffer,w.fg_live_audio(live),1024),1e-9);
    const audio=new Float64Array(w.memory.buffer,w.fg_live_audio(live),1024);
    for(let f=0;f<512;++f)history.push((audio[f*2]+audio[f*2+1])*.5);
  }
  compare(new Float64Array(w.memory.buffer,w.fg_live_view(live),3344),1e-8);
  const pointer=w.fg_live_particles(live),header=new Float64Array(w.memory.buffer,pointer,10);
  compare(new Float64Array(w.memory.buffer,pointer,10+header[2]*16),1e-7);
  compare(new Float64Array(w.memory.buffer,w.fg_live_spectrum(live),96),1e-5);
  // Independent scalar DFT checks the FFT's window, scale, ordering and bands.
  const spectrum=new Float64Array(w.memory.buffer,w.fg_live_spectrum(live),96),tail=history.slice(-2048);
  for(const band of [0,16,24,48,95]){
    const first=Math.max(1,Math.round(55*(12000/55)**(band/96)*2048/48000));
    const last=Math.max(first,Math.min(1024,Math.round(55*(12000/55)**((band+1)/96)*2048/48000)));
    let peak=0;
    for(let k=first;k<=last;++k){let re=0,im=0;for(let i=0;i<2048;++i){const x=tail[i]*(.5-.5*Math.cos(2*Math.PI*i/2048)),a=2*Math.PI*k*i/2048;re+=x*Math.cos(a);im-=x*Math.sin(a);}peak=Math.max(peak,Math.hypot(re,im)*4/2048);}
    const expected=Math.max(-100,Math.min(0,20*Math.log10(Math.max(1e-10,peak))));
    assert(Math.abs(expected-spectrum[band])<1e-6,`Spectrum band ${band}: ${expected} != ${spectrum[band]}`);
  }
  assert.equal(offset,native.byteLength);w.fg_live_destroy(live);
  console.log(`Native/WASM live audio and field parity passed; maximum error ${error}`);
}finally{rmSync(temporary,{recursive:true,force:true});}
