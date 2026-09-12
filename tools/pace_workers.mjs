/** Concurrent timer-paced offline workers. Never represents hardware underruns. */
import assert from 'node:assert/strict';
import {mkdir,writeFile,readFile,rename} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {parseArgs} from 'node:util';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';
const {values}=parseArgs({options:{playwright:{type:'string'},browser:{type:'string'},
  url:{type:'string',default:'http://127.0.0.1:8777'},output:{type:'string',default:'build/paced-workers'},
  seconds:{type:'string',default:'30'},'hardware-vulkan':{type:'boolean',default:false}}});
assert(values.playwright&&values.browser,'Provide --playwright and --browser');
const seconds=Number(values.seconds);assert(Number.isInteger(seconds)&&seconds>=6&&seconds<=30);
await mkdir(values.output,{recursive:true});
const path=resolve(values.output,'receipt.json');
const receipt={status:'running',started:new Date().toISOString(),seconds,records:[],underruns:null,
 scope:'Independent timer-paced audio/provider workers; no audio device or AudioWorklet',
 schedule:'4 x 64 frames per nominal 5.333ms; absolute timers, no catch-up bursts; solver pumped independently (max 4 commands/8ms, 1ms yield)',
 limitations:'Timer lateness and render-call overruns are NOT underruns. Fresh workers, warm browser/OS caches. Warm CPU standby included. Source bellSource, seed 12345, grid 32, 48kHz. No cross-run PCM identity expected.'};
await writeFile(path,JSON.stringify(receipt,null,2)+'\n',{flag:'wx'});
let saving=Promise.resolve();
const save=()=>{const snapshot=JSON.stringify(receipt,null,2)+'\n';saving=saving.then(async()=>{await writeFile(path+'.tmp',snapshot);await rename(path+'.tmp',path);});return saving;};
let browser;
try {
 const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
 const args=['--no-sandbox','--enable-unsafe-webgpu',...(values['hardware-vulkan']?
   ['--use-angle=vulkan','--enable-features=Vulkan','--disable-vulkan-surface']:['--use-angle=swiftshader'])];
 browser=await chromium.launch({executablePath:values.browser,headless:true,args});
 receipt.browser=browser.version();receipt.args=args;
 const page=await browser.newPage();
 page.on('console',msg=>console.log(msg.text()));
 await page.exposeFunction('checkpoint',async progress=>{receipt.progress=progress;await save();});
 await page.exposeFunction('savePCM',async(name,base64)=>{
   assert(/^(external|gpu)-(default|swirl)-(none|stall|failure)$/.test(name));
   const bytes=Buffer.from(base64,'base64'),file=name+'.f64le';
   assert.equal(bytes.length,seconds*48000*2*8);
   await writeFile(resolve(values.output,file),bytes,{flag:'wx'});
   return {file,bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')};
 });
 await page.goto(values.url);
 receipt.provenance=await page.evaluate(async()=>{
   if(!crossOriginIsolated)throw new Error('Serve with --isolated');
   const response=await fetch('./provenance.json');if(!response.ok)throw new Error('Missing provenance');return response.json();
 });
 receipt.hashes={};
 for(const file of ['tools/pace_workers.worker.ts','tools/pace_workers.mjs','web/engine.ts','web/bridge.ts','web/gpu-provider.ts','web/gpu-solver.ts','web/solver.wgsl','build/web/pace_workers.worker.js'])
   {
   const bytes=await readFile(file);receipt.hashes[file]=createHash('sha256').update(bytes).digest('hex');
   const snapshot=resolve(values.output,'sources',file);await mkdir(resolve(snapshot,'..'),{recursive:true});
   await writeFile(snapshot,bytes,{flag:'wx'});
 }
 await save();
 for(const [mode,profile,fault] of [['external','default','none'],['gpu','default','none'],
   ['gpu','swirl','none'],['external','swirl','none'],['external','default','stall'],['gpu','default','failure']]) {
   const request={mode,profile,fault,seconds};receipt.progress={...request,state:'preparing'};await save();
   const record=await page.evaluate(async request=>{
     const {SharedRing,commandBytes,packetBytes}=await import('./bridge.js');
     const setup={grid:32,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(32)).buffer};
     const clock=new SharedArrayBuffer(8),workers=[],ready={},results={};
     try {
       await new Promise((resolve,reject)=>{
         const timer=setTimeout(()=>reject(new Error('Paced workers timed out')),180000);
         const finishError=message=>{clearTimeout(timer);reject(new Error(message));};
         for(const role of ['audio','provider']) {
           const worker=new Worker('./pace_workers.worker.js',{type:'module'});workers.push(worker);
           worker.onerror=e=>finishError(e.message);
           worker.onmessage=({data})=>{
             if(data.type==='error'){finishError(data.message);return;}
             if(data.type==='progress') {
               console.log(request.mode,request.profile,request.fault,data.second);
               void globalThis.checkpoint({...request,state:'rendering',second:data.second}).catch(e=>finishError(String(e)));
             }
             if(data.type==='ready') {
               ready[role]=data;
               if(ready.audio&&ready.provider)for(const item of workers)item.postMessage({type:'go'});
             }
             if(data.type==='done') {
               results[role]=data;
               if(results.audio&&results.provider){clearTimeout(timer);resolve();}
             }
           };
           worker.postMessage({...request,role,setup,clock});
         }
       });
       const pcm=results.audio.pcm,bytes=new Uint8Array(pcm.length*8),view=new DataView(bytes.buffer);
       pcm.forEach((v,i)=>view.setFloat64(i*8,v,true));
       let base64='';for(let i=0;i<bytes.length;i+=24576)base64+=btoa(String.fromCharCode(...bytes.subarray(i,i+24576)));
       const artifact=await globalThis.savePCM([request.mode,request.profile,request.fault].join('-'),base64);
       return {...request,audio:results.audio.record,provider:results.provider.record,artifact};
     } finally {Atomics.store(new Int32Array(clock),1,1);for(const worker of workers)worker.terminate();}
   },request);
   receipt.records.push(record);receipt.progress={...request,state:'complete'};await save();
   console.log('completed',mode,profile,fault,record.audio.elapsedMs.toFixed(1),'ms');
 }
 await browser.close();browser=undefined;
 receipt.status='complete';receipt.finished=new Date().toISOString();await save();console.log(path);
} catch(error) {receipt.status='failed';receipt.error=error.stack??String(error);await save();throw error;}
finally {await browser?.close();}
