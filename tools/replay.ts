/** Replay a bounded offline archive with the pinned host; no solver/GPU needed.
 * Usage: bun tools/replay.ts --input capture.fgreplay.json --output replay.wav */
import {readFile, writeFile, stat} from "node:fs/promises";
import {parseArgs} from "node:util";
import {resolve} from "node:path";
import {ReplayPlayer, sha256} from "../web/replay";
import {encodeWav, type CsoundApi} from "../web/engine";
const {values} = parseArgs({options:{input:{type:"string"},output:{type:"string"},
  assets:{type:"string",default:"build/web"}}});
if(!values.input || !values.output)throw new Error("Pass --input capture.fgreplay.json --output replay.wav");
if(resolve(values.input)===resolve(values.output))throw new Error("Input and output must differ");
if((await stat(values.input)).size>64*1024*1024)throw new Error("Replay exceeds 64 MiB");
const assets=resolve(values.assets!);
const manifest=await readFile(resolve(assets,"provenance.json"));
const provenance=JSON.parse(manifest.toString());
// Verify actual runtime bytes, not merely the provenance file's claims.
const entry=await readFile(resolve(assets,"csound.js"));
const plugin=await readFile(resolve(assets,"fluidgrain.wasm"));
if(await sha256(entry)!==provenance.browserEntrySha256 || await sha256(plugin)!==provenance.wasmSha256)
  throw new Error("Runtime assets differ from the recorded build");
for(const [name,expected] of Object.entries(provenance.applicationSha256)) {
  if(await sha256(await readFile(resolve(assets,name)))!==expected)throw new Error("Changed application asset: "+name);
}
// Execute the archived build's replay module, not whatever source happens to
// be checked out now. It bundles the matching engine and clock semantics.
const {ReplayPlayer: BuiltPlayer} = await import(resolve(assets,"replay.js")) as {ReplayPlayer:typeof ReplayPlayer};
const marker="const Csound = kd; const libcsound = __lcs__; export { Csound, libcsound }; export default Csound;";
if(!entry.toString().includes(marker))throw new Error("Pinned browser entry changed");
Object.defineProperty(globalThis,"window",{value:{atob:globalThis.atob.bind(globalThis),
  btoa:globalThis.btoa.bind(globalThis),webkitAudioContext:undefined},configurable:true});
let player: ReplayPlayer | undefined;
try {
  const factory=new Function(entry.toString().replace(marker,"return __lcs__;"))() as
    (options:{withPlugins:ArrayBuffer[]})=>Promise<CsoundApi>;
  const api=await factory({withPlugins:[plugin.slice().buffer]});
  player=await BuiltPlayer.create(api,await readFile(resolve(assets,"prepared.inc"),"utf8"),
    await readFile(values.input),await sha256(manifest));
  const pcm=new Float64Array(player.frames*2);
  for(let offset=0;offset<pcm.length;offset+=128)pcm.set(player.render(),offset);
  await player.verify();
  // Never publish an output before the acceptance/control and PCM checks pass.
  await writeFile(values.output,new Uint8Array(encodeWav(pcm.subarray(0,player.outputFrames*2))),{flag:"wx"});
  console.log(JSON.stringify({frames:player.outputFrames,exactPCM:true,solver:false,output:resolve(values.output)}));
}finally{player?.destroy();Reflect.deleteProperty(globalThis,"window");}
