import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
let Processor;const messages=[],credits=[];
class AudioWorkletProcessor {constructor(){this.port={postMessage:m=>messages.push(m)};}}
vm.runInNewContext(readFileSync(new URL('../ui/audio.worklet.js',import.meta.url),'utf8'),{
  AudioWorkletProcessor,Float32Array,currentFrame:0,
  registerProcessor:(name,type)=>{assert.equal(name,'naviergrain-output');Processor=type;}
});
const p=new Processor(),source={postMessage:m=>credits.push(m),start(){}};
p.port.onmessage({data:{type:'connect',port:source}});assert.equal(credits[0].count,4);
const block=n=>{const a=new Float32Array(1024);for(let i=0;i<512;++i)a[2*i]=a[2*i+1]=(n*512+i+1)/4096;return a;};
function submit(n){source.onmessage({data:{type:'pcm',audio:block(n).buffer}});}
function render(){const output=[new Float32Array(128),new Float32Array(128)];p.process([], [output]);return output;}
submit(0);assert(render().every(a=>a.every(v=>v===0)));assert.equal(p.played,0,'Wait for prefill');
submit(1);submit(2);submit(3);
for(let i=0;i<16;++i){const out=render();for(let j=0;j<128;++j){const frame=i*128+j;const gain=frame<32?(frame+1)/32:1;assert.equal(out[0][j],Math.fround((frame+1)/4096*gain));assert.equal(out[0][j],out[1][j]);}}
assert.equal(p.played,2048);assert.equal(credits.length,5);
render();assert.equal(p.underruns,1);render();assert.equal(p.underruns,1,'Count one starvation episode');
submit(4);render();assert.equal(p.played,2048,'Retain a short queue while waiting');
source.onmessage({data:{type:'drain'}});
for(let i=0;i<4;++i)render();
assert.equal(p.played,2560);assert.equal(p.frames,0);assert.equal(p.underruns,1);
assert.equal(messages.filter(m=>m.type==='ended').length,1);render();assert.equal(p.played,2560);
console.log('Live worklet: bounded credits, exact FIFO, prefill, starvation/recovery and final drain passed');
