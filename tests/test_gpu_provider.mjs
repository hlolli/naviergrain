import assert from "node:assert/strict";
import {mkdir,writeFile,readFile} from "node:fs/promises";
import {createHash} from "node:crypto";
import {parseArgs} from "node:util";
import {pathToFileURL} from "node:url";
import {resolve} from "node:path";
const {values}=parseArgs({options:{
 playwright:{type:"string"},browser:{type:"string"},
 url:{type:"string",default:"http://127.0.0.1:8770"},
 output:{type:"string",default:"build/gpu-provider-check"},
 "hardware-vulkan":{type:"boolean",default:false},
}});
assert(values.playwright&&values.browser);
const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
const args=["--no-sandbox","--enable-unsafe-webgpu"];
if(!values["hardware-vulkan"])args.push("--use-angle=swiftshader");
if(values["hardware-vulkan"])args.push("--use-angle=vulkan","--enable-features=Vulkan","--disable-vulkan-surface");
const browser=await chromium.launch({executablePath:values.browser,headless:true,args});
try {
 const page=await browser.newPage();
 page.on("console",message=>console.log(message.text()));
 await page.goto(values.url);
 const result=await page.evaluate(()=>new Promise((resolve,reject)=>{
   const worker=new Worker("./test_gpu_provider.worker.js",{type:"module"});
   const timer=setTimeout(()=>{worker.terminate();reject(new Error("GPU test timed out"));},240000);
   worker.onerror=event=>{clearTimeout(timer);worker.terminate();reject(new Error(event.message));};
   worker.onmessage=({data})=>{
     if(data.type==="progress"){console.log(data.name,data.grid);return;}
     clearTimeout(timer);worker.terminate();
     if(data.type==="error")reject(new Error(data.message));else resolve(data.result);
   };
 }));
 const files=["web/gpu-provider.ts","web/bridge.ts","web/field.worker.ts","web/gpu-solver.ts",
 "web/solver.wgsl","tests/test_gpu_provider.worker.ts","tests/test_gpu_provider.mjs"];
 const hashes={};
 for(const file of files)hashes[file]=createHash("sha256").update(await readFile(file)).digest("hex");

 const receipt={...result,browser:browser.version(),args,hashes};
 await mkdir(values.output,{recursive:true});
 await writeFile(resolve(values.output,"receipt.json"),JSON.stringify(receipt,null,2)+"\n");
 console.log(JSON.stringify(receipt,null,2));
}finally{await browser.close();}
