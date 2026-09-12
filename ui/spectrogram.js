function rgb(token){const c=document.createElement('canvas');c.width=c.height=1;const ctx=c.getContext('2d');ctx.fillStyle=getComputedStyle(document.documentElement).getPropertyValue(token).trim();ctx.fillRect(0,0,1,1);return ctx.getImageData(0,0,1,1).data;}
export class Spectrogram {
  constructor(canvas){this.canvas=canvas;this.context=canvas.getContext('2d');canvas.width=512;canvas.height=96;this.tint=rgb('--color-accent');this.light=rgb('--color-tube-light');this.clear();}
  clear(){this.context.clearRect(0,0,this.canvas.width,this.canvas.height);}
  accept(bands){
    if(bands.length!==96||!Array.from(bands).every(Number.isFinite))return;
    const c=this.context,w=this.canvas.width,h=this.canvas.height;
    // Replace history pixels. Source-over would repeatedly add their alpha,
    // turning quiet bands opaque and retaining energy after it has stopped.
    c.globalCompositeOperation='copy';c.drawImage(this.canvas,1,0,w-1,h,0,0,w-1,h);c.globalCompositeOperation='source-over';
    const column=c.createImageData(1,h);
    for(let i=0;i<h;++i){const level=Math.max(0,Math.min(1,(bands[h-1-i]+90)/75));
      for(let channel=0;channel<3;++channel)column.data[i*4+channel]=Math.round(this.tint[channel]*(1-level)+this.light[channel]*level);
      column.data[i*4+3]=Math.round(255*level);
    }
    c.putImageData(column,w-1,0);
  }
}
