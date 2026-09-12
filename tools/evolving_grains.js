/* Offline experiment, not an AudioWorklet. Immutable 32-sample descriptors
 * allow exactly one committed batch; cancellation invalidates its epoch. */
export function cpuBatch(data, plan, voices, frames, overlap, config) {
  const result=new Float64Array(frames*2),power=new Float64Array(frames);
  const n=config.sourceLength;
  function convolve(band, integer, fraction, loop) {
    const f=fraction*128,phase=Math.min(127,Math.floor(f)),blend=f-phase;
    const radius=data[band*2],taps=radius*2,a=data[band*2+1]+phase*taps;
    let sum=0;
    for(let t=0;t<taps;t++) {
      let index=integer-radius+1+t;
      if(loop)index=((index%n)+n)%n;
      if(index>=0 && index<n)sum+=data[66+index]*(data[a+t]+blend*(data[a+t+taps]-data[a+t]));
    }
    return sum;
  }
  for(let seg=0;seg<frames/32;seg++)for(let grain=0;grain<voices;grain++) {
    const p=(seg*voices+grain)*12;
    let position=plan[p]+plan[p+1],pitch=plan[p+2],pan=plan[p+3];
    const first=plan[p+8],count=plan[p+9],loop=!!plan[p+10],length=plan[p+7];
    for(let j=0;j<count;j++) {
      pitch+=config.smooth10*(plan[p+4]-pitch);pan+=config.smooth10*(plan[p+5]-pan);
      const age=plan[p+6]+j,env=age===0||age===length-1?0:.5-.5*Math.cos(2*Math.PI*age/(length-1));
      const location=Math.max(0,Math.min(32,pitch*16)),band=Math.floor(location),blend=location-band;
      let sample=0;
      if(loop || (position>=0 && position<=n-1)) {
        const integer=Math.floor(position),fraction=position-integer;
        sample=convolve(band,integer,fraction,loop);
        if(blend>0 && band<32)sample+=blend*(convolve(band+1,integer,fraction,loop)-sample);
        if(!loop){const ramp=Math.max(0,Math.min(1,Math.min(position,n-1-position)/96));sample*=.5-.5*Math.cos(Math.PI*ramp);}
      }
      const frame=seg*32+first+j;
      result[2*frame]+=sample*env*Math.cos(.5*Math.PI*pan);
      result[2*frame+1]+=sample*env*Math.sin(.5*Math.PI*pan);power[frame]+=env*env;
      position+=2**pitch;if(loop)position%=n;
    }
  }
  for(let frame=0;frame<frames;frame++) {
    overlap+=config.smooth50*(power[frame]-overlap);
    const gain=.15/Math.sqrt(Math.max(1,overlap));result[2*frame]*=gain;result[2*frame+1]*=gain;
  }
  return {pcm:Float32Array.from(result),overlap};
}
export class EvolvingRenderer {
  static async create(adapter, data, config, shader) {
    const renderer=new EvolvingRenderer();
    renderer.config={...config};renderer.data=data.slice();renderer.epoch=1;renderer.clock=0;
    renderer.overlap=0;renderer.busy=false;renderer.closed=false;renderer.mode='gpu';renderer.fallbacks=0;
    renderer.failure=null;renderer.buffers=[];
    try {
      if(!adapter)throw new Error('WebGPU unavailable');
      const device=await adapter.requestDevice();renderer.device=device;
      device.addEventListener('uncapturederror',event=>{renderer.failure=event.error.message;});
      const module=device.createShaderModule({code:shader});
      const info=await module.getCompilationInfo();
      if(info.messages.some(m=>m.type==='error'))throw new Error(info.messages.map(m=>`${m.lineNum}:${m.linePos}: ${m.message}`).join('\n'));
      const layout=device.createBindGroupLayout({entries:Array.from({length:6},(_,binding)=>({binding,
        visibility:GPUShaderStage.COMPUTE,buffer:{type:binding===0?'uniform':binding<3?'read-only-storage':'storage'}}))});
      const pipelineLayout=device.createPipelineLayout({bindGroupLayouts:[layout]});
      renderer.pipelines=await Promise.all(['grains','reduce','finish'].map(entryPoint=>device.createComputePipelineAsync({layout:pipelineLayout,compute:{module,entryPoint}})));
      const buffer=(size,usage)=>{const b=device.createBuffer({size,usage});renderer.buffers.push(b);return b;};
      const rw=GPUBufferUsage.STORAGE;
      renderer.settings=buffer(32,GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST);
      const resident=buffer(data.byteLength,rw|GPUBufferUsage.COPY_DST);
      renderer.plans=buffer(2048/32*1024*12*4,rw|GPUBufferUsage.COPY_DST);
      const partial=buffer(2048*1024*16,rw),mixed=buffer(2048*16,rw);
      renderer.output=buffer((2048*2+2)*4,rw|GPUBufferUsage.COPY_SRC);
      renderer.staging=buffer((2048*2+2)*4,GPUBufferUsage.MAP_READ|GPUBufferUsage.COPY_DST);
      renderer.bind=device.createBindGroup({layout,entries:[renderer.settings,resident,renderer.plans,partial,mixed,renderer.output].map((b,binding)=>({binding,resource:{buffer:b}}))});
      device.queue.writeBuffer(resident,0,data);await device.queue.onSubmittedWorkDone();
      return renderer;
    } catch(error){renderer.useCpu(error);return renderer;}
  }
  useCpu(error) {
    this.mode='cpu';this.fallbacks++;this.fallbackReason=String(error);
    for(const b of this.buffers)b.destroy();this.device?.destroy();
  }
  cancel() {this.epoch++;this.clock=0;this.overlap=0;}
  destroy() {this.closed=true;this.epoch++;for(const b of this.buffers ?? [])b.destroy();this.device?.destroy();}
  async render(plan, voices, frames, start, fault) {
    if(this.closed||this.busy||start!==this.clock)throw new Error('Closed, busy, or noncontiguous batch');
    if(!Number.isInteger(voices)||voices<1||voices>1024||!Number.isInteger(frames)||frames<32||frames>2048||frames%32||plan.length!==frames/32*voices*12)throw new Error('Invalid batch shape');
    const begin=performance.now(),owned=plan.slice(),epoch=this.epoch;this.busy=true;
    let result;
    try {
      if(this.mode==='gpu') {
        try {
          if(fault==='device-loss')this.device.destroy();
          const uniforms=new ArrayBuffer(32);
          new Uint32Array(uniforms,0,4).set([voices,frames,this.config.sourceLength,0]);
          new Float32Array(uniforms,16,4).set([this.config.smooth10,this.config.smooth50,this.overlap,.15]);
          this.device.queue.writeBuffer(this.settings,0,uniforms);this.device.queue.writeBuffer(this.plans,0,owned);
          const encoder=this.device.createCommandEncoder();
          for(let k=0;k<3;k++) {
            const pass=encoder.beginComputePass();pass.setPipeline(this.pipelines[k]);pass.setBindGroup(0,this.bind);
            pass.dispatchWorkgroups(k===0?Math.ceil(voices*(frames/32)/64):k===1?frames:1);pass.end();
          }
          encoder.copyBufferToBuffer(this.output,0,this.staging,0,(2*frames+2)*4);
          this.device.queue.submit([encoder.finish()]);
          let timer;
          try {
            const mapped=this.staging.mapAsync(GPUMapMode.READ);
            // Five seconds is an offline liveness bound, NOT an audio deadline.
            // The injected never-ready mapping exercises the same timeout path.
            await Promise.race([fault==='timeout'?mapped.then(()=>new Promise(()=>{})):mapped,
              new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('Readback timeout')),fault==='timeout'?5:5000);})]);
          } finally {clearTimeout(timer);}
          try {
            const data=new Float32Array(this.staging.getMappedRange());
            result={pcm:data.slice(0,frames*2),overlap:data[frames*2]};
          } finally {this.staging.unmap();}
          if(fault==='readback')throw new Error('Injected rejected readback');
          if(this.failure)throw new Error(this.failure);
          if(!Number.isFinite(result.overlap)||!result.pcm.every(Number.isFinite))throw new Error('Invalid GPU result');
        } catch(error) {
          if(this.closed||epoch!==this.epoch)return {cancelled:true};
          this.useCpu(error);
        }
      }
      if(this.closed||epoch!==this.epoch)return {cancelled:true};
      if(this.mode==='cpu')result=cpuBatch(this.data,owned,voices,frames,this.overlap,this.config);
      if(!Number.isFinite(result.overlap)||!result.pcm.every(Number.isFinite))throw new Error('Invalid CPU result');
      this.overlap=result.overlap;this.clock+=frames;
      return {...result,start,end:this.clock,epoch,mode:this.mode,elapsedMs:performance.now()-begin};
    } finally {this.busy=false;}
  }
}
