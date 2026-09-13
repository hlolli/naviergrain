let wasm,live=0,port,credits=0,draining=false,scheduled=false;
function fail(error){postMessage({type:'error',message:String(error?.message||error)});draining=true;port?.postMessage({type:'drain'});}
// One queued task per batch; credits bound total work and leave room for
// controls. MessageChannel avoids nested timer delays in a busy audio worker.
const tasks=new MessageChannel();
function pump() {
  if(scheduled||draining||credits===0||!live)return;
  scheduled=true;tasks.port2.postMessage(0);
}
tasks.port1.onmessage=()=>{
  scheduled=false;if(draining||!credits)return;
  try {
    if(!wasm.ng_live_render(live,512))throw new Error('The audio engine stopped. Restart playback.');
    const audio=Float32Array.from(new Float64Array(wasm.memory.buffer,wasm.ng_live_audio(live),1024),v=>Math.max(-1,Math.min(1,v)));
    --credits;port.postMessage({type:'pcm',audio:audio.buffer},[audio.buffer]);pump();
  }catch(error){fail(error);}
};
onmessage=async event=>{
  const m=event.data;
  try {
    if(m.type==='init') {
      const response=await fetch('naviergrain-live.wasm');if(!response.ok)throw new Error('The WASM engine is missing. Build the browser runtime first.');
      const module=await WebAssembly.compile(await response.arrayBuffer());
      // This engine needs no host clocks, files, random source or WASI shims.
      wasm=(await WebAssembly.instantiate(module,{})).exports;
      live=wasm.ng_live_create(m.sampleRate,-1);if(!live)throw new Error('Could not prepare the audio engine.');
      for(let i=0;i<m.controls.length;++i)if(!wasm.ng_live_control(live,i,m.controls[i]))throw new Error('Invalid initial control.');
      port=m.port;port.onmessage=event=>{if(event.data.type==='credit'){credits+=event.data.count;if(credits>4){fail('Audio credit overflow');return;}pump();}};port.start();
      postMessage({type:'ready'});
    }else if(m.type==='control'&&live) {
      if(!wasm.ng_live_control(live,m.index,m.value))throw new Error('Control is out of range.');
    }else if(m.type==='view'&&live) {
      const pointer=wasm.ng_live_particles(live),header=new Float64Array(wasm.memory.buffer,pointer,10);
      const particles=Float64Array.from(new Float64Array(wasm.memory.buffer,pointer,10+header[2]*16));
      const stats=Float64Array.from(new Float64Array(wasm.memory.buffer,wasm.ng_live_stats(live),20));
      const spectrum=Float64Array.from(new Float64Array(wasm.memory.buffer,wasm.ng_live_spectrum(live),96));
      const audio=new Float64Array(wasm.memory.buffer,wasm.ng_live_audio(live),1024);
      const wave=Float64Array.from({length:128},(_,i)=>(audio[i*8]+audio[i*8+1])*.5);
      postMessage({type:'view',particles,stats,spectrum,wave,gpu:false},[particles.buffer,stats.buffer,spectrum.buffer,wave.buffer]);
    }else if(m.type==='drain') {draining=true;port?.postMessage({type:'drain'});}
  }catch(error){fail(error);}
};
