import assert from 'node:assert/strict';
import {Spectrogram} from '../ui/spectrogram.js';
// Model source-over alpha while scrolling the canvas onto itself. Constant
// input must retain the same brightness as it moves left through history.
const alpha=new Float64Array(512);
const context={
  clearRect(){alpha.fill(0);},
  drawImage(){for(let x=0;x<511;x++){const a=alpha[x+1];alpha[x]=this.globalCompositeOperation==='copy'?a:a+alpha[x]*(1-a);}},
  createImageData(w,h){return {data:new Uint8ClampedArray(w*h*4)};},
  putImageData(image,x){alpha[x]=image.data[3]/255;}
};
globalThis.document={documentElement:{},createElement:()=>({getContext:()=>({fillRect(){},getImageData(){return {data:[40,180,190,255]};}})})};
globalThis.getComputedStyle=()=>({getPropertyValue:()=>''});
const scope=new Spectrogram({getContext:()=>context});
for(let i=0;i<80;i++)scope.accept(new Float64Array(96).fill(-80));
assert.equal(alpha[500],alpha[511],'Scrolling must not accumulate opacity in quiet bands');
console.log('Spectrogram: constant quiet bands retain their opacity while scrolling');
