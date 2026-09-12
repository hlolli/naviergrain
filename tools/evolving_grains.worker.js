import {EvolvingRenderer,cpuBatch} from './evolving_grains.js';
const check=(ok,message)=>{if(!ok)throw new Error(message);};
async function bytes(name){const r=await fetch(name);check(r.ok,`Missing ${name}`);return r.arrayBuffer();}
function compare(pcm,reference) {
  check(pcm.length===reference.length,'Wrong PCM length');
  let maximum=0,error=0,signal=0,boundary=0;
  for(let i=0;i<pcm.length;i++) {
    check(Number.isFinite(pcm[i]),'Nonfinite PCM');
    const delta=pcm[i]-reference[i];maximum=Math.max(maximum,Math.abs(delta));error+=delta*delta;signal+=reference[i]*reference[i];
    if(i>=2)boundary=Math.max(boundary,Math.abs((pcm[i]-pcm[i-2])-(reference[i]-reference[i-2])));
  }
  const relative=Math.sqrt(error/Math.max(signal,1e-30));
  check(maximum<1e-5&&relative<1e-4,`Audio error max=${maximum} relative=${relative}`);
  return {maximum,relative,maxAdjacentDifferenceError:boundary};
}
self.onmessage=async({data:request})=>{
  let renderer;
  try {
    const attempts=[];
    async function acquireAdapter() {
      const start=performance.now();let adapter;
      do {
        adapter=await navigator.gpu?.requestAdapter({powerPreference:'high-performance'});
        attempts.push(adapter?{vendor:adapter.info.vendor,architecture:adapter.info.architecture,description:adapter.info.description}:null);
        if(adapter)break;await new Promise(resolve=>setTimeout(resolve,500));
      }while(performance.now()-start<3000);
      check(adapter,'No WebGPU adapter');check(adapter.info.vendor.toLowerCase().includes('nvidia'),'NVIDIA required');
      return adapter;
    }
    const config=JSON.parse(new TextDecoder().decode(await bytes('./evolving.json')));
    const input=new Float32Array(await bytes('./input.f32'));
    const shader=new TextDecoder().decode(await bytes('./evolving_grains.wgsl'));
    const plans=new Map(),references=new Map(),continuous=new Map();
    for(const row of config.rows) {
      plans.set(row.voices,new Float32Array(await bytes(`./plan-${row.voices}.f32`)));
      references.set(row.voices,new Float64Array(await bytes(`./reference-${row.voices}.f64`)));
      continuous.set(row.voices,new Float64Array(await bytes(`./continuous-${row.voices}.f64`)));
    }
    let cases=config.rows.flatMap(row=>[64,512,2048].map(frames=>({row,frames,fault:null})));
    if(request.reverse)cases.reverse();
    const results=[];
    for(const test of [...cases,...['readback','device-loss','timeout'].map(fault=>({row:config.rows[0],frames:512,fault}))]) {
      const {row,frames,fault}=test;const prepare=performance.now();
      renderer=await EvolvingRenderer.create(await acquireAdapter(),input,config,shader);
      const preparationMs=performance.now()-prepare;
      const plan=plans.get(row.voices),pcm=new Float32Array(config.total*2),timings=[],cpuMs=[];
      let gpuBatches=0,cpuBatches=0;
      for(let clock=0;clock<config.total;clock+=frames) {
        const chunk=plan.subarray(clock/32*row.voices*12,(clock+frames)/32*row.voices*12);
        const result=await renderer.render(chunk,row.voices,frames,clock,clock===1536?fault:null);
        check(!result.cancelled&&result.start===clock&&result.end===clock+frames,'Clock discontinuity');
        pcm.set(result.pcm,clock*2);timings.push(result.elapsedMs);
        if(result.mode==='gpu')gpuBatches++;else cpuBatches++;
        cpuMs.push(row.segmentRenderMs.slice(clock/32,(clock+frames)/32).reduce((a,b)=>a+b,0));
      }
      check(fault?renderer.fallbacks===1&&cpuBatches>0:renderer.fallbacks===0&&gpuBatches===config.total/frames,'Unexpected fallback');
      const error=compare(pcm,references.get(row.voices)),continuousError=compare(pcm,continuous.get(row.voices));
      const overlapError=Math.abs(renderer.overlap-row.finalOverlap);
      check(overlapError<1e-3,'Overlap continuity failed');
      const result={voices:row.voices,frames,fault,gpuBatches,cpuBatches,preparationMs,endToEndMs:timings,
        nativeRenderMs:cpuMs,nominalBatchMs:frames/48,deadlineMisses:timings.filter(t=>t>frames/48).length,
        error,continuousError,overlapError,fallbackReason:renderer.fallbackReason??null};
      renderer.destroy();renderer=null;results.push(result);postMessage({type:'row',result,pcm},[pcm.buffer]);
    }
    // A cancelled readback must not commit audio or the old overlap state.
    renderer=await EvolvingRenderer.create(await acquireAdapter(),input,config,shader);
    const plan=plans.get(32).subarray(0,512/32*32*12);
    const pending=renderer.render(plan,32,512,0);
    let busyRejected=false;try{await renderer.render(plan,32,512,0);}catch{busyRejected=true;}
    renderer.cancel();const stale=await pending;
    check(stale.cancelled&&renderer.clock===0&&renderer.overlap===0&&busyRejected,'Cancellation/ownership');
    const restarted=await renderer.render(plan,32,512,0);
    compare(restarted.pcm,references.get(32).subarray(0,1024));
    let duplicateRejected=false;try{await renderer.render(plan,32,512,0);}catch{duplicateRejected=true;}
    check(duplicateRejected,'Duplicate clock was accepted');
    // The descriptor passed by the caller is copied before asynchronous work.
    renderer.cancel();const mutable=plan.slice(),owned=renderer.render(mutable,32,512,0);mutable.fill(0);
    compare((await owned).pcm,references.get(32).subarray(0,1024));
    renderer.cancel();const closing=renderer.render(plan,32,512,0);renderer.destroy();
    check((await closing).cancelled,'Destroyed batch committed');renderer=null;
    const unavailable=[];
    for(const shaderFailure of [false,true]) {
      renderer=await EvolvingRenderer.create(shaderFailure?await acquireAdapter():null,input,config,shaderFailure?'invalid WGSL':shader);
      check(renderer.mode==='cpu'&&renderer.fallbacks===1,'Preparation fallback');
      const pcm=(await renderer.render(plan,32,512,0)).pcm;
      unavailable.push({shaderFailure,reason:renderer.fallbackReason,error:compare(pcm,references.get(32).subarray(0,1024))});
      renderer.destroy();renderer=null;
    }
    // Test the prepared CPU implementation without a GPU, at maximum pool size.
    const cpuPlan=plans.get(1024).subarray(0,512/32*1024*12);
    const begin=performance.now(),cpu=cpuBatch(input,cpuPlan,1024,512,0,config);
    const cpuFallbackMs=performance.now()-begin;
    const cpuFallbackError=compare(cpu.pcm,references.get(1024).subarray(0,1024));
    postMessage({type:'complete',result:{adapter:attempts.at(-1),attempts,rows:results.length,
      ownership:{busyRejected,duplicateRejected,cancelled:true,callerMutationIsolated:true,inflightDestroy:true},
      preparationFallback:unavailable,maxPoolCpuFallback:{ms:cpuFallbackMs,error:cpuFallbackError},scope:'Offline evolving descriptors, not the production scheduler or a live device'}});
  }catch(error){postMessage({type:'error',message:String(error.stack||error)});}
  finally{renderer?.destroy();}
};
