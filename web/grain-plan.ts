/** Private offline Csound plan host. Csound performance calls service commands;
 * only capture advances instrument time. No AudioWorklet contract. */
import {PreparedChannels, reserveHostScratch} from './channels';
import {browserConfig, type CsoundApi, type Settings} from './engine';
import {controlDefaults, stat} from './fluidgrain-schema';
const diagnosticNames = ['voice_drops', 'cap_drops', 'numeric_interventions'] as const;
export interface GrainBatch { readonly ticket: bigint; readonly start: bigint; readonly frames: number; readonly packet: Uint8Array; readonly records: number; readonly peakVoices: number; }
export class GrainPlanHost {
  private handle: number;
  private address = 0;
  private buffer: ArrayBuffer;
  private channels: PreparedChannels;
  private offsets = {packet: 0, output: 0};
  private pending?: GrainBatch;
  private closed = false;
  readonly resources: Uint8Array;
  readonly capacity: number;
  readonly packetCapacity: number;
  constructor(private readonly api: CsoundApi, settings: Settings, cfg = browserConfig(), readonly maxFrames = 32) {
    if (!Number.isInteger(maxFrames) || maxFrames<1 || maxFrames>512 || settings.controls.length !== controlDefaults.length || !settings.controls.every(Number.isFinite) ||
        settings.source.length < 4 || settings.source.length > 1440000 || !settings.source.every(Number.isFinite) ||
        !Number.isFinite(settings.sourceRate) || settings.sourceRate < 8000 || settings.sourceRate > 192000)
      throw new Error('Invalid grain plan settings');
    this.handle = api.csoundCreate();
    this.buffer = api.getMemory().buffer;
    try {
      const csd = `<CsoundSynthesizer>
<CsOptions>
-n -d -m0
</CsOptions>
<CsInstruments>
sr=48000
ksmps=32
nchnls=2
0dbfs=1
giSource ftgen 1, 0, -${settings.source.length}, -2, 0
instr 1
iConfig[] fillarray ${cfg.join(',')}
kControl[] fillarray ${settings.controls.join(',')}
${settings.controls.map((_,i)=>`kControl[${i}] chnget "pg.c${i}"`).join('\n')}
kAddress, kStats[] fluidgrain_plan giSource, ${settings.sourceRate}, iConfig, kControl, ${maxFrames}
chnset kAddress, "pg.address"
${diagnosticNames.map(name=>`chnset kStats[${stat[name]}], "pg.${name}"`).join('\n')}
endin
</CsInstruments>
<CsScore>
i 1 0 -1
f 0 z
</CsScore>
</CsoundSynthesizer>`;
      if (api.csoundCompileCSD(this.handle, csd) || api.csoundStart(this.handle)) throw new Error('Plan host start failed');
      api.csoundTableCopyIn(this.handle,1,settings.source);
      for(let i=0;i<settings.controls.length;i++)api.csoundSetControlChannel(this.handle,`pg.c${i}`,settings.controls[i]!);
      if(api.csoundPerformKsmps(this.handle))throw new Error('Plan preparation failed');
      this.address=api.csoundGetControlChannel(this.handle,'pg.address');
      const exports=api.wasm?.exports;if(!exports)throw new Error('Pinned host exports unavailable');
      this.channels=new PreparedChannels(api.getMemory(),exports,this.handle,[...settings.controls.map((_,i)=>`pg.c${i}`), ...diagnosticNames.map(name=>`pg.${name}`)]);
      reserveHostScratch(exports);this.channels.reanchorOffline();this.buffer=api.getMemory().buffer;
      if(!Number.isSafeInteger(this.address)||this.address%8||this.address<8||this.address+128>this.buffer.byteLength)
        throw new Error('Invalid plan mailbox address');
      const h=this.header();
      if(h[0]!==0x4d504746||h[1]!==2||this.address+h[2]!>this.buffer.byteLength)throw new Error('Plan mailbox ABI mismatch');
      if(!(h[21]! & 1))throw new Error('Plan host lacks CPU-only capture; rebuild instrument assets');
      this.offsets={packet:h[10]!,output:h[11]!};
      this.resources=new Uint8Array(this.buffer,this.address+h[8]!,h[9]!).slice();
      this.capacity=h[19]!;this.packetCapacity=h[20]!;
      if(h[18]!==maxFrames || this.capacity<1 || this.capacity>4096 ||
          this.packetCapacity!==64*Math.ceil((64+4*Math.max(32,maxFrames))/64)+this.capacity*maxFrames*64 ||
          this.offsets.packet+this.packetCapacity!==h[2])throw new Error('Invalid plan capacity');
    } catch(error) {api.csoundDestroy(this.handle);this.closed=true;throw error;}
  }
  private header(): Uint32Array {return new Uint32Array(this.buffer,this.address,32);}
  private perform(action: number): void {
    if(this.closed||this.buffer!==this.api.getMemory().buffer)throw new Error('Closed or detached plan host');
    this.header()[3]=action;
    if(this.api.csoundPerformKsmps(this.handle)||this.buffer!==this.api.getMemory().buffer)throw new Error('Plan performance failed or grew heap');
    if(this.header()[4])throw new Error('Plan command rejected');
  }
  diagnostics(): Record<typeof diagnosticNames[number], number> {
    if(this.closed || this.buffer!==this.api.getMemory().buffer)throw new Error('Closed or detached plan host');
    return {voice_drops:this.channels.get('pg.voice_drops'),
      cap_drops:this.channels.get('pg.cap_drops'),
      numeric_interventions:this.channels.get('pg.numeric_interventions')};
  }
  controls(values: number[]): void {
    if(this.closed||this.pending||values.length!==controlDefaults.length||!values.every(Number.isFinite))throw new Error('Invalid control boundary');
    values.forEach((value,i)=>this.channels.set(`pg.c${i}`,value));
  }
  capture(frames=32, pack=true): GrainBatch {
    if(this.closed||this.pending||!Number.isInteger(frames)||frames<1||frames>this.maxFrames)throw new Error('Invalid capture boundary');
    this.header()[5]=frames;this.perform(pack?1:4);const h=this.header();
    const ticket=BigInt(h[12]!)|(BigInt(h[13]!)<<32n),start=BigInt(h[16]!)|(BigInt(h[17]!)<<32n);
    const batch={ticket,start,frames,records:h[22]!,peakVoices:h[23]!,packet:new Uint8Array(this.buffer,this.address+this.offsets.packet,h[7]!).slice()};
    this.pending=batch;return batch;
  }
  private select(batch: GrainBatch): void {
    if(this.closed||this.pending!==batch)throw new Error('Stale or foreign grain batch');
    this.header()[14]=Number(batch.ticket&0xffffffffn);this.header()[15]=Number(batch.ticket>>32n);
  }
  fallback(batch: GrainBatch): Float64Array {
    this.select(batch);this.perform(2);
    return new Float64Array(this.buffer,this.address+this.offsets.output,batch.frames*2).slice();
  }
  commit(batch: GrainBatch, pcm: Float64Array | Float32Array): Float64Array {
    this.select(batch);
    if(pcm.length!==batch.frames*2||!pcm.every(Number.isFinite))throw new Error('Invalid complete grain output');
    new Float64Array(this.buffer,this.address+this.offsets.output,pcm.length).set(pcm);
    this.perform(3);this.pending=undefined;
    return new Float64Array(this.buffer,this.address+this.offsets.output,pcm.length).slice();
  }
  destroy(): void {if(!this.closed){this.closed=true;this.pending=undefined;this.api.csoundDestroy(this.handle);}}
}
