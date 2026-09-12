import assert from "node:assert/strict";
import {mkdir,writeFile,readFile} from "node:fs/promises";
import {createHash} from "node:crypto";
import {parseArgs} from "node:util";
import {pathToFileURL} from "node:url";
import {resolve} from "node:path";
const {values}=parseArgs({options:{
 playwright:{type:"string"},browser:{type:"string"},
 url:{type:"string",default:"http://127.0.0.1:8770"},
 output:{type:"string",default:"build/gpu-check"},
 "hardware-vulkan":{type:"boolean",default:false},
 "adapter-startup-ms":{type:"string",default:"0"},
}});
assert(values.playwright&&values.browser);
const startupMs=Number(values["adapter-startup-ms"]);
assert(Number.isInteger(startupMs)&&startupMs>=0&&startupMs<=10000);
const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
const args=["--no-sandbox","--enable-unsafe-webgpu"];
if(!values["hardware-vulkan"])args.push("--use-angle=swiftshader");
if(values["hardware-vulkan"])args.push("--use-angle=vulkan","--enable-features=Vulkan","--disable-vulkan-surface");
const browser=await chromium.launch({executablePath:values.browser,headless:true,args});
try {
 const page=await browser.newPage();
 page.on("console",message=>console.log(message.text()));
 await page.goto(values.url);
 // Optional diagnostic for adapters unavailable during browser GPU startup.
 // This warms the device before fixture timing; it is not a production retry.
 const adapterStartup=startupMs ? await page.evaluate(async budget=>{
   const start=performance.now(),attempts=[];
   do {
     const adapter=await navigator.gpu?.requestAdapter();
     attempts.push(adapter ? {vendor:adapter.info.vendor,architecture:adapter.info.architecture} : null);
     if(adapter){const device=await adapter.requestDevice();device.destroy();
       return {attempts,elapsedMs:performance.now()-start};}
     const remaining=budget-(performance.now()-start);
     if(remaining<=0)break;
     await new Promise(resolve=>setTimeout(resolve,Math.min(500,remaining)));
   } while(performance.now()-start<budget);
   throw new Error("Adapter startup failed: "+JSON.stringify(attempts));
 },startupMs) : null;
 console.log("adapter startup",JSON.stringify(adapterStartup));
 const result=await page.evaluate(()=>new Promise((resolve,reject)=>{
   const worker=new Worker("./test_gpu.worker.js",{type:"module"});
   const timer=setTimeout(()=>{worker.terminate();reject(new Error("GPU test timed out"));},240000);
   worker.onerror=event=>{clearTimeout(timer);worker.terminate();reject(new Error(event.message));};
   worker.onmessage=({data})=>{
     if(data.type==="progress"){console.log(data.name,data.grid);return;}
     clearTimeout(timer);worker.terminate();
     if(data.type==="error")reject(new Error(data.message));else resolve(data.result);
   };
 }));
 const files=["src/fluidgrain_field.c","tests/gpu_reference.c","web/gpu-solver.ts",
 "web/solver.wgsl","tests/test_gpu.worker.ts","tests/test_gpu.mjs"];
 const hashes={};
 for(const file of files)hashes[file]=createHash("sha256").update(await readFile(file)).digest("hex");
 const fixtureHash=createHash("sha256").update(await readFile("build/web/gpu-reference.json")).digest("hex");
 const receipt={...result,fixtureSha256:fixtureHash,browser:browser.version(),args,adapterStartup,hashes};
 await mkdir(values.output,{recursive:true});
 await writeFile(resolve(values.output,"receipt.json"),JSON.stringify(receipt,null,2)+"\n");
 console.log(JSON.stringify(receipt,null,2));
}finally{await browser.close();}
