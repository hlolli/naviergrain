import assert from 'node:assert/strict';
import {readFile,writeFile,mkdir} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseArgs} from 'node:util';
const {values}=parseArgs({options:{playwright:{type:'string'},browser:{type:'string'},url:{type:'string',default:'http://127.0.0.1:8784'},output:{type:'string',default:'build/grain-delivery'},software:{type:'boolean',default:false},reverse:{type:'boolean',default:false}}});
assert(values.playwright&&values.browser);
await mkdir(values.output,{recursive:true});
const receipt={status:'running',reverse:values.reverse,rows:[],hashes:{}};
for(const file of ['src/fluidgrain_core.c','src/fluidgrain_grain_plan.h','src/fluidgrain_pack.c','src/fluidgrain_pack.h','src/fluidgrain_plan_opcode.inc','web/grain-plan.ts','web/grain-plan.wgsl','web/grain-renderer.ts','tests/test_grain_delivery.worker.ts','build/wasm/fluidgrain.wasm'])receipt.hashes[file]=createHash('sha256').update(await readFile(file)).digest('hex');
const save=()=>writeFile(resolve(values.output,'receipt.json'),JSON.stringify(receipt,null,2)+'\n');await save();
const {chromium}=await import(pathToFileURL(resolve(values.playwright)).href);
let browser,pending=Promise.resolve();
try {
 const args=['--no-sandbox','--enable-unsafe-webgpu',...(values.software?['--use-angle=swiftshader']:['--use-angle=vulkan','--enable-features=Vulkan','--disable-vulkan-surface'])];
 browser=await chromium.launch({headless:true,executablePath:values.browser,args});receipt.browser=browser.version();receipt.args=args;
 const page=await browser.newPage();await page.goto(values.url);
 await page.exposeFunction('checkpoint',({row,pcm})=>{
  pending=pending.then(async()=>{const bytes=Buffer.from(new Float32Array(pcm).buffer);await writeFile(resolve(values.output,row.name+'.f32'),bytes);
   row.pcmSha256=createHash('sha256').update(bytes).digest('hex');receipt.rows.push(row);await save();console.log(JSON.stringify(row));});return pending;
 });
 receipt.result=await page.evaluate(reverse=>new Promise((resolve,reject)=>{
  const worker=new Worker('./test_grain_delivery.worker.js'+(reverse?'?reverse':''),{type:'module'});
  const timer=setTimeout(()=>{worker.terminate();reject(new Error('Real-grain GPU test timed out'));},600000);
  worker.onerror=e=>{clearTimeout(timer);worker.terminate();reject(new Error(e.message));};
  worker.onmessage=({data})=>{
   if(data.type==='progress'){window.checkpoint({row:data.row,pcm:Array.from(data.pcm)}).catch(reject);return;}
   clearTimeout(timer);worker.terminate();data.type==='error'?reject(new Error(data.message)):resolve(data.result);
  };
 }),values.reverse);
 await pending;assert.equal(receipt.rows.length,13);receipt.status='complete';await save();
} catch(error){await pending.catch(()=>{});receipt.status='failed';receipt.error=String(error.stack||error);await save();throw error;}
finally{await browser?.close();}
