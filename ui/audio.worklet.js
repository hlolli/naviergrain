class FluidGrainOutput extends AudioWorkletProcessor {
  constructor() {
    super();this.slots=new Array(4);this.read=0;this.write=0;this.offset=0;this.frames=0;
    this.waiting=true;this.fade=0;this.last=[0,0];this.underruns=0;this.played=0;this.draining=false;this.ended=false;
    this.port.onmessage=event=>{
      if(event.data.type==='connect') {
        this.source=event.data.port;this.source.onmessage=e=>{
          if(e.data.type==='pcm'){
            if(this.write-this.read>=4){this.port.postMessage({type:'error',message:'Audio queue overflow'});return;}
            this.slots[this.write++%4]=new Float32Array(e.data.audio);this.frames+=512;
          }else if(e.data.type==='drain')this.draining=true;
        };this.source.start();this.source.postMessage({type:'credit',count:4});
      }
    };
  }
  process(inputs,outputs) {
    const out=outputs[0];if(out.length<2)return true;
    if(this.waiting&&(this.frames>=1024||(this.draining&&this.frames>0))){this.waiting=false;this.fade=32;}
    for(let i=0;i<out[0].length;++i) {
      if(!this.waiting&&this.frames>0) {
        const block=this.slots[this.read%4];let gain=this.fade?(33-this.fade--)/32:1;
        if(this.draining&&this.frames<64)gain*=this.frames/64;
        for(let c=0;c<2;++c)out[c][i]=this.last[c]=block[this.offset*2+c]*gain;
        ++this.offset;--this.frames;++this.played;
        if(this.offset===512){this.slots[this.read++%4]=null;this.offset=0;this.source.postMessage({type:'credit',count:1});}
      }else {
        if(!this.waiting){this.waiting=true;this.fade=32;if(!this.draining)++this.underruns;}
        const gain=this.fade?--this.fade/32:0;for(let c=0;c<2;++c)out[c][i]=this.last[c]*gain;
      }
    }
    if(this.draining&&!this.frames&&!this.ended){this.ended=true;this.port.postMessage({type:'ended',played:this.played,underruns:this.underruns});}
    if(currentFrame%4096===0)this.port.postMessage({type:'status',played:this.played,underruns:this.underruns});
    return true;
  }
}
registerProcessor('fluidgrain-output',FluidGrainOutput);
