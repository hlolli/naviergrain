import assert from "node:assert/strict";
import {mkdir,writeFile,readFile} from "node:fs/promises";
import {createHash} from "node:crypto";
import {parseArgs} from "node:util";
import {pathToFileURL} from "node:url";
import {resolve} from "node:path";
const {values}=parseArgs({options:{playwright:{type:"string"},browser:{type:"string"},
 url:{type:"string",default:"http://127.0.0.1:8770"},output:{type:"string",default:"build/browser-gpu-check"}}});
assert(values.playwright&&values.browser);
const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
const args=["--no-sandbox","--enable-unsafe-webgpu","--use-angle=swiftshader"];
const browser=await chromium.launch({executablePath:values.browser,headless:true,args});
try {
 const page=await browser.newPage();const errors=[];
 page.on("pageerror",e=>errors.push(e.message));
 await page.goto(values.url);
 const records=[];
 for (const mode of ["gpu","loss","stall","pause","busy","missing","cpu","internal"]) {
  const result=await page.evaluate(async mode=>{
   const {SharedRing,commandBytes,packetBytes}=await import("./bridge.js");
   const setup={grid:32,commands:new SharedRing(commandBytes).buffer,fields:new SharedRing(packetBytes(32)).buffer};
   const moduleURL=new URL("./field.worker.js",location.href).href;
   // Fault injection is confined to this test wrapper. Production workers
   // expose no device handles or failure controls.
   const source=`
    let device;
    const gpu=navigator.gpu;
    if (${JSON.stringify(mode)} === "missing") Object.defineProperty(navigator,"gpu",{value:undefined});
    else if(gpu) Object.defineProperty(navigator,"gpu",{value:{requestAdapter:async()=>{
      const adapter=await gpu.requestAdapter(); if(!adapter)return adapter;
      const request=adapter.requestDevice.bind(adapter);
      Object.defineProperty(adapter,"requestDevice",{value:async()=>{device=await request();return device;}});
      return adapter;
    }}});
    await import(${JSON.stringify(moduleURL)});
    const handler=onmessage;
    onmessage=event=>{if(event.data.type==="lose-device")device?.destroy();else handler(event);};
    postMessage({type:"boot"});`;
   const blobURL=URL.createObjectURL(new Blob([source],{type:"text/javascript"}));
   const solver=mode==="internal"?null:new Worker(blobURL,{type:"module"});
   const audio=new Worker("./render.worker.js",{type:"module"});
   const controls=Array.from(document.querySelectorAll("#controls input"),x=>Number(x.value));controls.splice(22,0,0);
   const samples=Float64Array.from({length:48000},(_,i)=>.3*Math.sin(2*Math.PI*220*i/48000));
   let provider,lastProvider,triggered=false,lossEpoch=0,maxEpoch=0,stale=false,lost=false,recovered=false;
   let maxAge=0,gpuBeforeLoss=0;
   const began=performance.now();let preparationMs;
   const reports=[];
   try {
    if(solver) await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>reject(new Error("Solver preparation timeout")),20000);
      solver.onerror=e=>{clearTimeout(timer);reject(new Error(e.message));};
      solver.onmessage=({data})=>{
       if(data.type==="error"){clearTimeout(timer);reject(new Error(data.message));}
       if(data.type==="ready"){clearTimeout(timer);provider=data.provider;lastProvider=provider;resolve();}
       if(data.type==="boot")solver.postMessage({type:"start",setup,backend:mode==="cpu"?"external":"gpu"});
      };
    });
    preparationMs=performance.now()-began;
    if(solver)solver.onmessage=({data})=>{
      if(data.type==="error")reports.push({error:data.message});
      if(data.type==="provider"){lastProvider=data;reports.push(data);}
    };
    const output=await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>reject(new Error("GPU audio timeout")),45000);
      audio.onerror=e=>{clearTimeout(timer);reject(new Error(e.message));};
      audio.onmessage=({data})=>{
       if(data.type==="error"){clearTimeout(timer);reject(new Error(data.message));return;}
       if(data.stats){
        maxEpoch=Math.max(maxEpoch,data.stats.epoch);maxAge=Math.max(maxAge,data.stats.field_age_ms);
        stale ||= Boolean(data.stats.status&4);lost ||= Boolean(data.stats.status&8);
        recovered ||= lost && !(data.stats.status&4);
       }
       if(data.type==="progress"&&!triggered&&data.fraction>.2){
        triggered=true;lossEpoch=data.stats.epoch;gpuBeforeLoss=lastProvider?.gpuFields??0;
        if(mode==="loss")solver.postMessage({type:"lose-device"});
        if(mode==="stall"){solver.postMessage({type:"pause"});setTimeout(()=>solver.postMessage({type:"resume"}),850);}
        if(mode==="pause"){audio.postMessage({type:"pause"});setTimeout(()=>audio.postMessage({type:"resume"}),200);}
        if(mode==="busy"){const until=performance.now()+350;while(performance.now()<until)Math.sqrt(performance.now());}
       }
       if(data.type==="done"){clearTimeout(timer);resolve(data);}
      };
      audio.postMessage({type:"start",seconds:3,setup:mode==="internal"?undefined:setup,
       settings:{source:samples,sourceRate:48000,controls}},[samples.buffer]);
    });
    let energy=0,peak=0,maxJump=0;const data=new DataView(output.wav);
    for(let i=44;i<output.wav.byteLength;i+=4){const v=data.getFloat32(i,true);
      if(!Number.isFinite(v))throw new Error("Nonfinite WAV");energy+=v*v;peak=Math.max(peak,Math.abs(v));
      if(i>=52)maxJump=Math.max(maxJump,Math.abs(v-data.getFloat32(i-8,true)));
    }
    return {mode,preparationMs,renderMs:output.renderMs,provider,lastProvider,gpuBeforeLoss,
      lossEpoch,maxEpoch,stale,lost,recovered,maxAge,energy,peak,maxJump,
      bridge:output.bridge,stats:output.stats,errors:reports.filter(x=>x.error)};
   }finally{audio.terminate();solver?.terminate();URL.revokeObjectURL(blobURL);}
  },mode);
  assert(result.energy>.01&&result.peak<1);
  assert(result.stats.snapshot_sequence>5);
  assert.deepEqual(result.errors,[]);
  if(mode!=="internal")assert.equal(result.bridge.rejected,0);
  if(!["cpu","internal","missing"].includes(mode))assert.equal(result.provider.backend,"gpu");
  if(mode==="loss"){
   assert(result.gpuBeforeLoss>5);assert.equal(result.lastProvider.backend,"cpu-fallback");
   assert(result.lastProvider.cpuFields>5);assert(result.maxEpoch>result.lossEpoch);
  }
  if(mode==="missing"){assert.equal(result.provider.backend,"cpu-fallback");assert(result.lastProvider.cpuFields>5);}
  if(mode==="pause")assert(result.maxEpoch>result.lossEpoch);
  if(mode==="stall"){assert(result.lost&&result.recovered);assert(result.bridge.commands.dropped>0);}
  records.push(result);console.log(mode,JSON.stringify(result));
 }
 // Real UI cancellation/restart, WAV playback, and narrow screen layout.
 await page.locator("#backend").selectOption("gpu");
 await page.locator("#seconds").fill("3");await page.locator("#render").click();
 await page.waitForFunction(()=>document.querySelector("#status").textContent==="Rendering…");
 await page.locator("#cancel").click();
 assert.equal(await page.locator("#status").textContent(),"Render cancelled.");
 await page.locator("#seconds").fill("1");await page.locator("#render").click();
 await page.waitForFunction(()=>!document.querySelector("#download").hidden,{timeout:30000});
 assert.match(await page.locator("#provider-detail").textContent(),/WebGPU fields/);
 const bytes=await page.evaluate(async()=> (await (await fetch(document.querySelector("#download").href)).arrayBuffer()).byteLength);
 assert.equal(bytes,44+48000*8);
 await page.locator("#audio").evaluate(a=>a.play());
 await page.waitForFunction(()=>document.querySelector("#audio").currentTime>.05);
 await page.setViewportSize({width:375,height:812});
 assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
 assert.deepEqual(errors,[]);
 await mkdir(values.output,{recursive:true});
 const hashes={};for(const name of ["web/gpu-provider.ts","web/bridge.ts","web/field.worker.ts","web/main.ts","web/index.html","tests/test_browser_gpu.mjs"])
  hashes[name]=createHash("sha256").update(await readFile(name)).digest("hex");
 await writeFile(resolve(values.output,"receipt.json"),JSON.stringify({browser:browser.version(),args,
   scope:"Offline GPU fields and prepared CPU failover; software WebGPU, no live deadline qualification",
   records,renders:records.length+1,cancelRestartPlayback:true,mobileLayout:true,hashes,errors},null,2)+"\n");
}finally{await browser.close();}
