/** Sequential offline measurements. Keep failed/partial receipts, never infer completion. */
import assert from "node:assert/strict";
import {mkdir,writeFile,readFile} from "node:fs/promises";
import {createHash} from "node:crypto";
import {parseArgs} from "node:util";
import {pathToFileURL} from "node:url";
import {resolve} from "node:path";
const {values}=parseArgs({options:{
 playwright:{type:"string"},browser:{type:"string"},url:{type:"string",default:"http://127.0.0.1:8770"},
 output:{type:"string",default:"build/backend-comparison"},seconds:{type:"string",default:"30"},
 "hardware-vulkan":{type:"boolean",default:false},
}});
assert(values.playwright && values.browser,"Provide --playwright and --browser");
const seconds=Number(values.seconds);
assert(Number.isInteger(seconds)&&seconds>=1&&seconds<=30);
await mkdir(values.output,{recursive:true});
const receiptPath=resolve(values.output,"receipt.json");
const receipt={status:"running",scope:"Serialized accepted-schedule comparison, not independently paced workers or live qualification",
 seconds,schedule:"One audio block, then await complete provider service; no catch-up or overlap",
 timing:"Render/copy timings include host overhead; GPU service includes warm CPU standby, dispatch, readback and packet publication",
 underruns:null,records:[],started:new Date().toISOString()};
// Reusing an output directory must not silently replace a previous experiment.
await writeFile(receiptPath,JSON.stringify(receipt,null,2)+"\n",{flag:"wx"});
const save=()=>writeFile(receiptPath,JSON.stringify(receipt,null,2)+"\n");
const args=["--no-sandbox","--enable-unsafe-webgpu",...(values["hardware-vulkan"]?
 ["--use-angle=vulkan","--enable-features=Vulkan","--disable-vulkan-surface"]:["--use-angle=swiftshader"])];
let browser;
try {
 const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
 browser=await chromium.launch({executablePath:values.browser,headless:true,args});
 receipt.browser=browser.version();receipt.args=args;
 const page=await browser.newPage();
 page.on("console",message=>console.log(message.text()));
 await page.exposeFunction("savePCM",async(name,base64)=>{
   assert(/^(internal|external|gpu)-(fixed|default|swirl)$/.test(name));
   const bytes=Buffer.from(base64,"base64");
   const file=name+".f64le";
   await writeFile(resolve(values.output,file),bytes,{flag:"wx"});
   return {file,bytes:bytes.length,sha256:createHash("sha256").update(bytes).digest("hex")};
 });
 await page.goto(values.url);
 receipt.provenance=await page.evaluate(async()=> {
   const response=await fetch("./provenance.json");if(!response.ok)throw new Error("Missing provenance");
   return response.json();
 });
 receipt.hashes={};
 for(const file of ["tools/compare_backends.worker.ts","tools/compare_backends.mjs",
   "tools/analyze_backends.py","web/engine.ts","web/bridge.ts","web/gpu-provider.ts","web/gpu-solver.ts","web/solver.wgsl"])
   receipt.hashes[file]=createHash("sha256").update(await readFile(file)).digest("hex");
 await save();
 for(const profile of ["fixed","default","swirl"]) {
   // Reverse the second full profile's mode order to expose order/caching effects.
   const modes=profile==="swirl"?["gpu","external","internal"]:["internal","external","gpu"];
   for(const mode of modes) {
     const request={mode,profile,seconds:profile==="fixed"?1:seconds};
     const record=await page.evaluate(request=>new Promise((resolve,reject)=>{
       const worker=new Worker("./compare_backends.worker.js",{type:"module"});
       const timer=setTimeout(()=>{worker.terminate();reject(new Error("Comparison timed out"));},300000);
       worker.onerror=e=>{clearTimeout(timer);worker.terminate();reject(new Error(e.message));};
       worker.onmessage=async({data})=>{
         if(data.type==="progress"){console.log(data.mode,data.profile,data.second);return;}
         clearTimeout(timer);worker.terminate();
         if(data.type==="error"){reject(new Error(data.message));return;}
         try {
           // Explicit little-endian binary64, independent of host typed-array byte order.
           const bytes=new Uint8Array(data.pcm.length*8),view=new DataView(bytes.buffer);
           data.pcm.forEach((v,i)=>view.setFloat64(i*8,v,true));
           let base64="";
           // Chunk sizes divisible by 3 allow concatenation of independently encoded chunks.
           for(let i=0;i<bytes.length;i+=24576)
             base64+=btoa(String.fromCharCode(...bytes.subarray(i,i+24576)));
           const artifact=await globalThis.savePCM(request.mode+"-"+request.profile,base64);
           resolve({...data.record,artifact});
         } catch(error){reject(error);}
       };
       worker.postMessage(request);
     }),request);
     receipt.records.push(record);await save();
     console.log("completed",mode,profile,record.renderMs.toFixed(1),"ms");
   }
 }
 const fixed=receipt.records.filter(r=>r.profile==="fixed");
 assert(fixed.every(r=>r.artifact.sha256===fixed[0].artifact.sha256),"Fixed mappings changed PCM across backends");
 // Cleanup is part of the run: a browser-close failure cannot leave success.
 await browser.close();browser=undefined;
 receipt.fixedMappingExact=true;receipt.status="complete";receipt.finished=new Date().toISOString();
 await save();
 console.log(receiptPath);
} catch(error) {
 receipt.status="failed";receipt.error=error.stack??String(error);await save();throw error;
} finally {await browser?.close();}
