/** Experimental worker-only WGSL solver. GPUProvider owns audio publication.
 * One prepared readback slot: free -> in-flight -> mapped -> free. Concurrent
 * steps are rejected before submission; no device work belongs on audio.
 */
import {control as C} from "./naviergrain-schema";
import schema from "../schema/naviergrain-v1.json";
export interface GPUProfile {
  grid: number; seed: number; fluidHz: number; particleHz: number;
  pressureIterations: number; viscosityIterations: number;
}
export interface GPUField {
  planes: Float32Array; diagnostics: Float32Array;
  time: number; sequence: number; interventions: number;
}
// The installed TypeScript DOM libraries include WebGPU interfaces but omit
// these standard namespace values. They are supplied by the browser, not us.
declare const GPUBufferUsage: Readonly<{MAP_READ: number; COPY_SRC: number;
  COPY_DST: number; UNIFORM: number; STORAGE: number}>;
declare const GPUMapMode: Readonly<{READ: number}>;
type SlotState = "free" | "in-flight" | "mapped" | "closed";
export class GPUFieldSolver {
  private readonly stateBuffer: GPUBuffer;
  private readonly settingsBuffer: GPUBuffer;
  private readonly passBuffer: GPUBuffer;
  private readonly staging: GPUBuffer;
  private readonly pipeline: GPUComputePipeline;
  private readonly passes: number[][] = [];
  private slot: SlotState = "free";
  private loss: string | undefined;
  private readonly phases: number[];
  private readonly settings = new Float32Array(32);
  private last: GPUField;
  readonly adapterInfo: Readonly<Record<string, string>>;
  private readonly onError = (event: GPUUncapturedErrorEvent) => {
    this.loss ??= event.error.message;
  };

  static async create(profile: GPUProfile, shader?: string): Promise<GPUFieldSolver> {
    profile = Object.freeze({...profile});
    if (!globalThis.isSecureContext || !navigator.gpu)
      throw new Error("Worker WebGPU is unavailable; use the CPU provider");
    if (![16,32,64].includes(profile.grid) ||
        !Number.isInteger(profile.seed) || profile.seed < 0 || profile.seed > 16777215 ||
        !Number.isFinite(profile.fluidHz) || profile.fluidHz < 30 || profile.fluidHz > 240 ||
        !Number.isFinite(profile.particleHz) || profile.particleHz < 60 || profile.particleHz > 1000 ||
        !Number.isInteger(profile.pressureIterations) || profile.pressureIterations < 16 || profile.pressureIterations > 256 ||
        !Number.isInteger(profile.viscosityIterations) || profile.viscosityIterations < 4 || profile.viscosityIterations > 64)
      throw new Error("Invalid GPU solver profile");
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error("No WebGPU adapter; use the CPU provider");
    const device = await adapter.requestDevice();
    try {
      if (shader === undefined) {
        const response = await fetch(new URL("./solver.wgsl", import.meta.url));
        if (!response.ok) throw new Error("Missing WGSL solver asset");
        shader = await response.text();
      }
      const module = device.createShaderModule({label:"Naviergrain MAC solver", code:shader});
      const info = await module.getCompilationInfo();
      const errors = info.messages.filter(item => item.type === "error");
      if (errors.length) throw new Error(errors.map(item => item.message).join("\n"));
      device.pushErrorScope("validation");
      const pipeline = await device.createComputePipelineAsync({
        layout:"auto", compute:{module,entryPoint:"main"},
      });
      const solver = new GPUFieldSolver(device, pipeline, profile, adapter.info);
      const error = await device.popErrorScope();
      if (error) { solver.destroy(); throw new Error(error.message); }
      return solver;
    } catch (error) {device.destroy(); throw error;}
  }

  private constructor(private readonly device: GPUDevice, pipeline: GPUComputePipeline,
                      readonly profile: GPUProfile, info: GPUAdapterInfo) {
    this.profile = Object.freeze({...profile});
    this.adapterInfo = Object.freeze({vendor:info.vendor,architecture:info.architecture,
      device:info.device,description:info.description});
    this.pipeline = pipeline;
    const count = profile.grid ** 2;
    this.stateBuffer = device.createBuffer({size:(16*count+16)*4,
      usage:GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_SRC|GPUBufferUsage.COPY_DST});
    this.settingsBuffer = device.createBuffer({size:128,usage:GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST});
    // Fixed uniform offsets prevent queue writes from making every dispatch
    // see the last opcode. All descriptors and bind groups are prepared once.
    const add = (op:number,a=0,b=0) => this.passes.push([profile.grid,op,a,b]);
    add(0);
    for (const plane of [4,5]) {
      add(1,plane,8);
      let from=plane, to=10;
      for(let pass=0;pass<profile.viscosityIterations;pass++) {
        add(2,from,to); [from,to]=[to,from];
      }
      if(from!==plane) add(1,from,plane);
    }
    add(3);
    for(let basis=0;basis<3;basis++) {add(4,basis);add(5);add(6,basis);add(7);}
    add(8);add(9);add(10);add(11);add(12);add(13);add(14,8);add(15,8);
    let from=9,to=10;
    for(let pass=0;pass<profile.pressureIterations;pass++) {
      add(16,from,to);[from,to]=[to,from];
    }
    if(from!==9) add(1,from,9);
    add(14,9);add(15,9);add(17);add(18);add(19);add(8);add(20);add(21);
    this.passBuffer=device.createBuffer({size:this.passes.length*256,usage:GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST});
    const descriptors=new Uint32Array(this.passes.length*64);
    this.passes.forEach((pass,index)=>descriptors.set(pass,index*64));
    device.queue.writeBuffer(this.passBuffer,0,descriptors);
    // Each descriptor has its own fixed bind group; this avoids reliance on
    // dynamic offsets in an auto pipeline and is bounded at preparation.
    this.passBindings=this.passes.map((_,index)=>device.createBindGroup({
      layout:pipeline.getBindGroupLayout(0),entries:[
        {binding:0,resource:{buffer:this.passBuffer,offset:index*256,size:16}},
        {binding:1,resource:{buffer:this.settingsBuffer}},
        {binding:2,resource:{buffer:this.stateBuffer}},
      ],
    }));
    this.staging=device.createBuffer({size:(4*count+16)*4,usage:GPUBufferUsage.MAP_READ|GPUBufferUsage.COPY_DST});
    let state=(profile.seed^0xd1b54a35)>>>0;
    this.phases=new Array<number>(6);
    // Seed extraction is integer-identical to the C forcing-only stream.
    for(let i=0;i<6;i++){
      state=(Math.imul(state,747796405)+2891336453)>>>0;
      const word=Math.imul((state>>>((state>>>28)+4))^state,277803737)>>>0;
      this.phases[i]=2*Math.PI*((((word>>>22)^word)>>>0)+0.5)/4294967296;
    }
    this.last={planes:new Float32Array(4*count),diagnostics:new Float32Array(16),time:0,sequence:0,interventions:0};
    device.addEventListener("uncapturederror",this.onError);
    void device.lost.then(info=>{if(this.slot!=="closed")this.loss=info.message||"GPU device lost";});
  }
  private readonly passBindings: GPUBindGroup[];

  get state(): SlotState { return this.slot; }
  get failure(): string | undefined {return this.loss;}
  private checkFree(): void {
    if(this.slot!=="free")throw new Error("GPU readback slot is "+this.slot);
    if(this.loss)throw new Error(this.loss);
  }
  /** Stopped-provider reset only. An in-flight step cannot be reset/reused. */
  reset(initial?: Float32Array): void {
    this.checkFree();
    const count=this.profile.grid**2;
    if(initial && (initial.length!==2*count || !initial.every(Number.isFinite)))
      throw new Error("Invalid initial MAC velocity");
    const data=new Float32Array(16*count+16);
    if(initial)data.set(initial);
    this.device.queue.writeBuffer(this.stateBuffer,0,data);
    this.last={planes:new Float32Array(4*count),diagnostics:new Float32Array(16),time:0,sequence:0,interventions:0};
    if(initial)this.last.planes.set(initial);
  }
  /** Caller owns controls and receives an owned snapshot, never a mapped view. */
  async step(controls: readonly number[]): Promise<GPUField> {
    this.checkFree();
    if(controls.length!==24 || controls.some((v,i)=>!Number.isFinite(v) ||
        v<schema.control[i]!.min || v>schema.control[i]!.max ||
        (schema.control[i]!.integer && !Number.isInteger(v))))
      throw new Error("GPU controls must be finite, sanitized schema values");
    if(controls[C.freeze] || controls[C.flow_speed]===0)
      return {...this.last,planes:this.last.planes.slice(),diagnostics:this.last.diagnostics.slice()};
    this.slot="in-flight";
    const started=performance.now();
    try {
      const p=this.profile, dt=controls[C.flow_speed]!/p.fluidHz;
      this.settings.set([dt,controls[C.viscosity]!*dt*p.grid*p.grid,
        controls[C.drive]!,controls[C.swirl]!,controls[C.turbulence]!,controls[C.eddy_size]!,
        controls[C.strain_drive]!,controls[C.confinement]!,
        1.5*p.particleHz/(p.grid*controls[C.flow_speed]!),this.last.time]);
      this.settings.set(this.phases,12);
      this.device.queue.writeBuffer(this.settingsBuffer,0,this.settings);
      const encoder=this.device.createCommandEncoder();
      const pass=encoder.beginComputePass();
      pass.setPipeline(this.pipeline);
      this.passes.forEach((descriptor,index)=>{
        // Zero viscosity is an exact skip, not a Jacobi identity approximation.
        const op=descriptor[1]!;
        const diffusionEnd=1+2*(1+p.viscosityIterations+(p.viscosityIterations%2));
        if(controls[C.viscosity]===0 && index>0 && index<diffusionEnd)return;
        pass.setBindGroup(0,this.passBindings[index]!);
        pass.dispatchWorkgroups([6,11,14,18,20].includes(op)?1:Math.ceil(p.grid*p.grid/64));
      });
      pass.end();
      encoder.copyBufferToBuffer(this.stateBuffer,0,this.staging,0,4*p.grid*p.grid*4);
      encoder.copyBufferToBuffer(this.stateBuffer,16*p.grid*p.grid*4,this.staging,4*p.grid*p.grid*4,64);
      this.device.queue.submit([encoder.finish()]);
      await this.staging.mapAsync(GPUMapMode.READ);
      if(this.state==="closed")throw new Error("GPU solver closed during readback");
      this.slot="mapped";
      const data=new Float32Array(this.staging.getMappedRange()).slice();
      this.staging.unmap();
      if(this.loss)throw new Error(this.loss);
      const diagnostics=data.slice(4*p.grid*p.grid);
      if(diagnostics[3]!==1 || !data.every(Number.isFinite))
        throw new Error("GPU numerical step rejected; published field retained");
      this.last={planes:data.slice(0,4*p.grid*p.grid),diagnostics,
        time:this.last.time+dt,sequence:this.last.sequence+1,
        interventions:this.last.interventions+diagnostics[2]!};
      this.lastStepMs=performance.now()-started;
      return {...this.last,planes:this.last.planes.slice(),diagnostics:diagnostics.slice()};
    } finally {
      if(this.state!=="closed") {if(this.staging.mapState==="mapped")this.staging.unmap();this.slot="free";}
    }
  }
  lastStepMs=0;
  destroy(): void {
    if(this.slot==="closed")return;
    this.slot="closed";
    this.device.removeEventListener("uncapturederror",this.onError);
    this.staging.destroy();this.stateBuffer.destroy();this.settingsBuffer.destroy();this.passBuffer.destroy();
    this.device.destroy();
  }
}
