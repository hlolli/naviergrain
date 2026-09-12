import {ReplayPlayer,sha256} from "../web/replay";
import {encodeWav,type CsoundApi} from "../web/engine";
Object.defineProperty(globalThis,"window",{value:{atob:globalThis.atob.bind(globalThis),
  btoa:globalThis.btoa.bind(globalThis),webkitAudioContext:undefined}});
Object.defineProperty(navigator,"gpu",{value:undefined});
onmessage=async ({data}:MessageEvent<Uint8Array>)=>{
  let player:ReplayPlayer|undefined;
  try {
    const host=new URL("./csound.js",import.meta.url).href;
    const {libcsound}=await import(host) as {libcsound(o:{withPlugins:ArrayBuffer[]}):Promise<CsoundApi>};
    const api=await libcsound({withPlugins:[await (await fetch("./fluidgrain.wasm")).arrayBuffer()]});
    player=await ReplayPlayer.create(api,await (await fetch("./prepared.inc")).text(),data,
      await sha256(new Uint8Array(await (await fetch("./provenance.json")).arrayBuffer())));
    const pcm=new Float64Array(player.frames*2);
    for(let offset=0;offset<pcm.length;offset+=128)pcm.set(player.render(),offset);
    await player.verify();
    const wav=encodeWav(pcm.subarray(0,player.outputFrames*2));
    postMessage({sha256:await sha256(new Uint8Array(wav)),bytes:wav.byteLength,withoutGPU:!navigator.gpu});
  }catch(error){postMessage({error:String(error)});}finally{player?.destroy();}
};
