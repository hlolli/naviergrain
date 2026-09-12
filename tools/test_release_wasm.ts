/** Exercise the plugin in the Csound host built from the same source commit. */
import assert from 'node:assert/strict';
import {resolve, posix} from 'node:path';
import {pathToFileURL} from 'node:url';
import {readFile} from 'node:fs/promises';

const [checkout, runtime, plugin] = process.argv.slice(2).map(p=>resolve(p));
assert(checkout && runtime && plugin, 'Usage: bun tools/test_release_wasm.ts CSOUND_SOURCE CSOUND_WASM_Z PLUGIN');
// These declarations normally come from Closure's bootstrap. Use the upstream
// loader and API directly, without editing a compiled browser bundle.
globalThis.goog = {
  define: (_name, value)=>value,
  declareModuleId: ()=>{},
  require: name=>{assert.equal(name, 'goog.string.path');return posix;},
  global: {},
};
const source = name=>pathToFileURL(resolve(checkout, 'wasm/browser/src', name)).href;
const {default: loadWasm} = await import(source('module.js'));
const {default: factory} = await import(source('libcsound.js'));
const bytes = await readFile(plugin);
const module = await WebAssembly.compile(bytes);
assert(WebAssembly.Module.customSections(module, 'dylink.0').length, 'Plugin must use relocatable memory/table offsets');
const messages=[];
async function host(plugins) {
  const [wasm,wasi] = await loadWasm({wasmDataURI: await readFile(runtime),
    withPlugins: plugins, messagePort:{post: message=>{if(message.log)messages.push(message.log);}}});
  wasm.wasi=wasi;
  return {api:factory(wasm), memory:wasm.exports.memory};
}
const schema=await readFile(resolve(import.meta.dir,'../include/fluidgrain.inc'),'utf8');
const score=block=>`<CsoundSynthesizer>
<CsOptions>
-n -d -m0 --sample-accurate
</CsOptions>
<CsInstruments>
sr=48000
ksmps=${block}
nchnls=2
0dbfs=1
${schema}
instr 1
iSource ftgen 0,0,-240,9,1,1,90
iConfig[] fillarray $FG_CONFIG_DEFAULTS
iConfig[$FG_CONFIG_SOURCE_LOOP]=1
iConfig[$FG_CONFIG_MAX_GRAINS]=256
kControl[] fillarray $FG_CONTROL_DEFAULTS
kControl[$FG_CONTROL_GRAIN_RATE] init 600
aL,aR,kStats[] naviergrain iSource,52800,iConfig,kControl
outs aL,aR
endin
</CsInstruments>
<CsScore>
i 1 0 .3
e
</CsScore>
</CsoundSynthesizer>`;
const absent=await host([]);
const absentCs=absent.api.csoundCreate();
assert.notEqual(absent.api.csoundCompileCSD(absentCs,score(32)),0,'Negative control must fail without the plugin');
absent.api.csoundDestroy(absentCs);
const {api,memory}=await host([bytes]);
const cs=api.csoundCreate();
function render(block) {
  assert.equal(api.csoundCompileCSD(cs,score(block)),0,messages.join('\n'));
  assert.equal(api.csoundStart(cs),0,messages.join('\n'));
  const samples=[];
  let ended=false;
  for(let i=0;i<2000;i++) {
    const result=api.csoundPerformKsmps(cs);
    assert(result>=0,messages.join('\n'));
    if(result>0){ended=true;break;}
    const audio=new Float64Array(memory.buffer,api.csoundGetSpout(cs),block*2);
    for(const sample of audio){assert(Number.isFinite(sample));samples.push(sample);}
  }
  assert(ended,'Score did not end');
  assert(samples.length>=28800);
  const output=Float64Array.from(samples.slice(0,28800));
  assert(output.some(v=>Math.abs(v)>.0001),'Plugin produced silence');
  assert(output.every(v=>Math.abs(v)<1),'Plugin clipped');
  assert(output.some((v,i)=>i%2===0&&v!==output[i+1]),'Stereo collapsed to mono');
  api.csoundReset(cs);
  return output;
}
try {
  const first=render(32);
  assert.deepEqual(render(32),first,'Reset changed seeded output');
  assert.deepEqual(render(37),first,'Output depends on ksmps');
  console.log(JSON.stringify({samples:first.length,peak:Math.max(...first.map(Math.abs)),
    checks:['opcode registration','finite stereo audio','reset','ksmps 32/37','missing-plugin control']}));
} finally {api.csoundDestroy(cs);}
