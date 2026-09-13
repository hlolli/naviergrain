export class BrowserRuntime {
  constructor(onSnapshot,onState){this.onSnapshot=onSnapshot;this.onState=onState;this.running=false;this.counts={played:0,underruns:0};}
  async start(controls) {
    if(this.stopping)await this.stopping;
    if(this.running)return;
    this.context=new AudioContext({sampleRate:48000,latencyHint:'interactive'});
    // Resume in the Play button's gesture, before fetching or compiling WASM.
    const resumed=this.context.resume();
    this.counts={played:0,underruns:0};
    try {
      await this.context.audioWorklet.addModule('audio.worklet.js');
      this.node=new AudioWorkletNode(this.context,'naviergrain-output',{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[2]});
      const channel=new MessageChannel();
      this.worker=new Worker('live.worker.js',{type:'module'});
      await new Promise((resolve,reject)=>{
        const timeout=setTimeout(()=>reject(new Error('Audio preparation timed out.')),15000);
        this.worker.onerror=e=>{clearTimeout(timeout);reject(new Error(e.message));};
        this.worker.onmessage=e=>{
          const m=e.data;
          if(m.type==='ready'){clearTimeout(timeout);resolve();}
          if(m.type==='error'){
            clearTimeout(timeout);reject(new Error(m.message));
            if(this.running){this.onState(m.message,true);void this.stop();}
          }
          if(m.type==='view'){this.pending=false;this.onSnapshot({...m,...this.counts,running:this.running});}
        };
        this.worker.postMessage({type:'init',sampleRate:this.context.sampleRate,controls,port:channel.port1},[channel.port1]);
      });
      this.node.port.onmessage=e=>{
        if(e.data.type==='status'||e.data.type==='ended')this.counts={played:e.data.played,underruns:e.data.underruns};
        if(e.data.type==='ended')this.drained?.();
        if(e.data.type==='error'){this.onState(e.data.message,true);void this.stop();}
      };
      this.node.port.postMessage({type:'connect',port:channel.port2},[channel.port2]);
      this.node.connect(this.context.destination);await resumed;this.running=true;
      this.context.onstatechange=()=>{
        if(this.context?.state==='suspended'&&this.running)this.onState('Audio is suspended. Press Resume audio to continue.');
      };
    }catch(error){await this.close();throw error;}
  }
  async resume(){await this.context?.resume();}
  control(index,value){this.worker?.postMessage({type:'control',index,value});}
  view(){if(this.running&&!this.pending){this.pending=true;this.worker.postMessage({type:'view'});}}
  async stop() {
    if(this.stopping)return this.stopping;
    this.stopping=(async()=>{
      if(this.running&&this.context?.state==='running')await new Promise(resolve=>{
        const timeout=setTimeout(resolve,2000);this.drained=()=>{clearTimeout(timeout);resolve();};
        this.worker?.postMessage({type:'drain'});
      });
      await this.close();this.stopping=null;
    })();return this.stopping;
  }
  async close(){this.running=false;this.pending=false;this.worker?.terminate();this.worker=null;this.node?.disconnect();this.node=null;
    if(this.context){this.context.onstatechange=null;await this.context.close();this.context=null;}}
}
