import assert from "node:assert/strict";
import {mkdir,writeFile} from "node:fs/promises";
import {parseArgs} from "node:util";
import {pathToFileURL} from "node:url";
import {resolve} from "node:path";
const {values}=parseArgs({options:{playwright:{type:"string"},browser:{type:"string"},
 url:{type:"string",default:"http://127.0.0.1:8770"},output:{type:"string",default:"build/browser-replay-check"}}});
assert(values.playwright&&values.browser);
const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
const browser=await chromium.launch({executablePath:values.browser,headless:true,
 args:["--no-sandbox","--enable-unsafe-webgpu","--use-angle=swiftshader"]});
try {
 const page=await browser.newPage();const errors=[];
 page.on("pageerror",error=>errors.push(error.message));
 await page.goto(values.url);
 assert(await page.locator("#record-replay").isDisabled());
 const records=[];
 await mkdir(values.output,{recursive:true});
 for(const mode of ["external","gpu"]) {
  await page.locator("#backend").selectOption(mode);
  await page.locator("#record-replay").check();
  await page.locator("#seconds").fill("1.1");
  await page.locator("#render").click();
  if(mode==="external") {
    await page.waitForFunction(()=>document.querySelector("#status").textContent==="Rendering…");
    await page.locator("#pause").click();
    await page.waitForFunction(()=>document.querySelector("#status").textContent==="Render paused.");
    await page.locator("#pause").click();
  }
  await page.waitForFunction(()=>!document.querySelector("#replay-download").hidden,null,{timeout:30000});
  const result=await page.evaluate(async()=>{
    const take=new Uint8Array(await (await fetch(document.querySelector("#replay-download").href)).arrayBuffer());
    const wav=await (await fetch(document.querySelector("#download").href)).arrayBuffer();
    const hash=Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256",wav)),v=>v.toString(16).padStart(2,"0")).join("");
    const replay=await new Promise((resolve,reject)=>{
      const worker=new Worker("./test_replay_download.worker.js",{type:"module"});
      const timer=setTimeout(()=>{worker.terminate();reject(new Error("Replay timed out"));},30000);
      worker.onerror=event=>{clearTimeout(timer);worker.terminate();reject(new Error(event.message));};
      worker.onmessage=({data})=>{clearTimeout(timer);worker.terminate();data.error?reject(new Error(data.error)):resolve(data);};
      worker.postMessage(take);
    });
    return {hash,replay,take:Array.from(take),wavBytes:wav.byteLength};
  });
  assert(result.replay.withoutGPU);assert.equal(result.replay.sha256,result.hash);
  assert.equal(result.replay.bytes,result.wavBytes);
  await writeFile(resolve(values.output,mode+".fgreplay.json"),new Uint8Array(result.take));
  records.push({mode,wavSha256:result.hash,wavBytes:result.wavBytes,takeBytes:result.take.length,exactReplay:true,noGPU:true});
 }
 await page.locator("#seconds").fill("3");await page.locator("#render").click();
 await page.waitForFunction(()=>document.querySelector("#status").textContent==="Rendering…");
 await page.locator("#cancel").click();
 assert(await page.locator("#replay-download").isHidden());
 await page.locator("#backend").selectOption("internal");
 assert(await page.locator("#record-replay").isDisabled());assert(!await page.locator("#record-replay").isChecked());
 await page.setViewportSize({width:375,height:812});
 assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
 assert.deepEqual(errors,[]);
 await writeFile(resolve(values.output,"receipt.json"),JSON.stringify({browser:browser.version(),records,cancel:true,mobile:true,errors},null,2)+"\n");
 console.log(JSON.stringify(records));
}finally{await browser.close();}
