import assert from 'node:assert/strict';
import {mkdir,readFile,writeFile,copyFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseArgs} from 'node:util';
import {createHash} from 'node:crypto';
import {cpus} from 'node:os';
const {values}=parseArgs({options:{playwright:{type:'string'},browser:{type:'string'},input:{type:'string'},output:{type:'string'},url:{type:'string',default:'http://127.0.0.1:8782'},reverse:{type:'boolean',default:false}}});
assert(values.playwright&&values.browser&&values.input&&values.output);
await mkdir(values.output);
const receipt={status:'running',started:new Date().toISOString(),cpu:cpus()[0].model,rows:[],hashes:{},reverse:values.reverse,
  thresholds:{maximum:1e-5,relativeRms:1e-4},limits:'8192 samples per stream, synthetic 32-sample control/voice plans, double native CPU vs float32 GPU, no solver/device. Continuous native reference and arbitrary delivery batching. Timings exclude precomputed trajectory planning; include upload/dispatch/readback and owned output.'};
const save=()=>writeFile(resolve(values.output,'receipt.json'),JSON.stringify(receipt,null,2)+'\n');
await save();let browser,pending=Promise.resolve();
try {
  for(const file of ['tools/evolving_grains.c','tools/grain_benchmark.c','src/fluidgrain_resampler.c','src/fluidgrain_resampler.h',
    'tools/evolving_grains.wgsl','tools/evolving_grains.js','tools/evolving_grains.worker.js','tools/compare_evolving_grains.mjs','build/evolving_grains',
    ...['evolving.json','input.f32',...[32,256,1024].flatMap(n=>[`plan-${n}.f32`,`reference-${n}.f64`,`continuous-${n}.f64`])].map(f=>`${values.input}/${f}`)]) {
    const bytes=await readFile(file);receipt.hashes[file]=createHash('sha256').update(bytes).digest('hex');
    const target=resolve(values.output,'sources',file);await mkdir(resolve(target,'..'),{recursive:true});await copyFile(file,target);
  }
  const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
  const args=['--no-sandbox','--enable-unsafe-webgpu','--use-angle=vulkan','--enable-features=Vulkan','--disable-vulkan-surface'];
  browser=await chromium.launch({headless:true,executablePath:values.browser,args});receipt.browser=browser.version();receipt.args=args;
  const page=await browser.newPage();
  await page.exposeFunction('checkpoint',({result,pcm})=>{
    pending=pending.then(async()=>{
      const bytes=Buffer.from(new Float32Array(pcm).buffer),name=`gpu-${result.voices}-${result.frames}-${result.fault??'normal'}.f32`;
      await writeFile(resolve(values.output,name),bytes,{flag:'wx'});result.pcmSha256=createHash('sha256').update(bytes).digest('hex');
      receipt.rows.push(result);await save();console.log(JSON.stringify({voices:result.voices,frames:result.frames,fault:result.fault,error:result.error,deadlineMisses:result.deadlineMisses}));
    });return pending;
  });
  await page.goto(values.url);
  const result=await page.evaluate(request=>new Promise((resolve,reject)=>{
    const worker=new Worker('./evolving_grains.worker.js',{type:'module'});
    const timer=setTimeout(()=>{worker.terminate();reject(new Error('Evolving benchmark timeout'));},240000);
    const close=()=>{clearTimeout(timer);worker.terminate();};
    worker.onerror=e=>{close();reject(new Error(e.message));};
    worker.onmessage=({data})=>{
      if(data.type==='row'){window.checkpoint({result:data.result,pcm:Array.from(data.pcm)}).catch(reject);return;}
      close();if(data.type==='error')reject(new Error(data.message));else resolve(data.result);
    };worker.postMessage(request);
  }),{reverse:values.reverse});
  await pending;assert.equal(receipt.rows.length,12);receipt.summary=result;
  await browser.close();browser=null;receipt.status='complete';await save();
}catch(error){await pending.catch(()=>{});receipt.status='failed';receipt.error=String(error.stack||error);await save();throw error;}
finally{await browser?.close();}
