import {GPUFieldSolver, type GPUField} from "../web/gpu-solver";
function check(value: unknown, message: string): asserts value {
  if(!value)throw new Error(message);
}
interface Fixture {
  name:string; grid:number; seed:number; steps:number; pressure:number; diffusion:number;
  controls:number[]; initial:number[]; planes:number[]; diagnostics:number[];
  interventions:number; time:number;
}
function errors(actual:ArrayLike<number>, expected:ArrayLike<number>) {
  check(actual.length===expected.length,"Field lengths differ");
  let max=0,sum=0,power=0;
  for(let i=0;i<actual.length;i++) {
    const delta=actual[i]!-expected[i]!;
    check(Number.isFinite(delta),"Nonfinite field");
    max=Math.max(max,Math.abs(delta));sum+=delta*delta;power+=expected[i]!*expected[i]!;
  }
  return {max,rms:Math.sqrt(sum/actual.length),relativeRms:Math.sqrt(sum/Math.max(power,1e-30))};
}
const identical=(a:Float32Array,b:Float32Array)=>a.length===b.length&&a.every((v,i)=>v===b[i]);
async function main() {
  const fixtures=await (await fetch("./gpu-reference.json")).json() as Fixture[];
  const results=[];
  let adapterInfo: Readonly<Record<string,string>> | undefined;
  for(const f of fixtures) {
    const profile={grid:f.grid,seed:f.seed,fluidHz:60,particleHz:240,
      pressureIterations:f.pressure,viscosityIterations:f.diffusion};
    const preparationStart=performance.now();
    const solver=await GPUFieldSolver.create(profile);
    const preparationMs=performance.now()-preparationStart;
    adapterInfo=solver.adapterInfo;
    try {
      solver.reset(Float32Array.from(f.initial));
      let result:GPUField|undefined;
      const times=[];
      for(let step=0;step<f.steps;step++) {result=await solver.step(f.controls);times.push(solver.lastStepMs);}
      check(result,"Missing field");
      const count=f.grid*f.grid;
      const velocity=errors(result.planes.subarray(0,2*count),f.planes.slice(0,2*count));
      const derivatives=errors(result.planes.subarray(2*count),f.planes.slice(2*count));
      const diagnostics=errors([1,4,5,6,7,8].map(i=>result!.diagnostics[i]!),f.diagnostics);
      // Fixed short-horizon f32 limits, including the combined extrema.
      // Derivative cancellation amplifies f32 error at higher resolutions.
      check(velocity.max<0.0001,"Velocity mismatch "+JSON.stringify({f:f.name,n:f.grid,velocity}));
      check(derivatives.max<0.005,"Derivative mismatch "+JSON.stringify({f:f.name,n:f.grid,derivatives}));
      check(diagnostics.max<0.003,"Diagnostic mismatch "+JSON.stringify({f:f.name,n:f.grid,diagnostics}));
      check(result.interventions===f.interventions,"Intervention mismatch");
      check(result.sequence===f.steps && Math.abs(result.time-f.time)<1e-12,"Clock mismatch");
      const saved=result.planes.slice();
      const frozen=[...f.controls];frozen[21]=1;
      const held=await solver.step(frozen);
      check(identical(saved,held.planes)&&held.sequence===result.sequence,"Freeze changed field/clock");
      frozen[21]=0;frozen[17]=0;
      check(identical(saved,(await solver.step(frozen)).planes),"Zero flow changed field");
      // Reset reproduces this device's arithmetic; snapshot copies are owned.
      result.planes.fill(123);
      solver.reset(Float32Array.from(f.initial));
      for(let step=0;step<f.steps;step++)result=await solver.step(f.controls);
      check(identical(saved,result.planes),"Reset replay differs");
      check(solver.state==="free","Readback did not release slot");
      const record={name:f.name,grid:f.grid,steps:f.steps,preparationMs,velocity,derivatives,diagnostics,
        interventions:result.interventions,meanStepMs:times.reduce((a,b)=>a+b,0)/times.length,
        maxStepMs:Math.max(...times)};
      results.push(record);
      postMessage({type:"progress",name:f.name,grid:f.grid});
    } finally {solver.destroy();}
  }
  const f=fixtures.find(f=>f.name==="normal")!;
  const profile={grid:16,seed:12345,fluidHz:60,particleHz:240,pressureIterations:64,viscosityIterations:16};
  const solver=await GPUFieldSolver.create(profile);
  try {
    const inflight=solver.step(f.controls);
    let rejected=false;
    try {await solver.step(f.controls);}catch{rejected=true;}
    check(rejected,"Concurrent step must not submit");
    rejected=false;try{solver.reset();}catch{rejected=true;}
    check(rejected,"Reset must not recycle an in-flight buffer");
    await inflight;
    for(const bad of [NaN,Infinity,-Infinity]) {
      const invalid=[...f.controls];invalid[9]=bad;
      rejected=false;try{await solver.step(invalid);}catch{rejected=true;}
      check(rejected&&solver.state==="free","Invalid command must not reserve staging");
    }
    for(const initial of [new Float32Array(1),new Float32Array(512).fill(Infinity)]) {
      rejected=false;try{solver.reset(initial);}catch{rejected=true;}
      check(rejected&&solver.state==="free","Invalid initial field must not mutate resources");
    }
    // Simulate provider teardown/device loss during asynchronous readback.
    const pending=solver.step(f.controls);
    solver.destroy();
    rejected=false;try{await pending;}catch{rejected=true;}
    check(rejected&&solver.state==="closed","Lost device must not publish a field");
    rejected=false;try{await solver.step(f.controls);}catch{rejected=true;}
    check(rejected,"Destroyed provider must remain closed");
    solver.destroy();
  } finally {solver.destroy();}
  // Fault injection only: destroy the underlying device without invoking the
  // solver's normal close path, exercising the asynchronous lost notification.
  const lostSolver=await GPUFieldSolver.create(profile);
  try {
    const device=Reflect.get(lostSolver,"device") as GPUDevice;
    device.destroy(); await device.lost;
    check(lostSolver.failure,"Device loss was not reported");
    let rejected=false;try{await lostSolver.step(f.controls);}catch{rejected=true;}
    check(rejected,"Lost device must not accept a solve");
  } finally {lostSolver.destroy();}
  let rejected=false;
  try{await GPUFieldSolver.create(profile,"this is not WGSL");}catch{rejected=true;}
  check(rejected,"Invalid shader must fail preparation");
  const original=navigator.gpu;
  Object.defineProperty(navigator,"gpu",{value:undefined,configurable:true});
  try {
    rejected=false;try{await GPUFieldSolver.create(profile);}catch{rejected=true;}
    check(rejected,"Missing GPU must fail explicitly without a CPU backend change");
  } finally {Object.defineProperty(navigator,"gpu",{value:original,configurable:true});}
  return {scope:"Worker WGSL numerical foundation; not audio integration or hardware performance qualification",
    adapterInfo,fixtures:results,checks:["C numerical reference","all grids","odd iteration budgets",
      "zero-viscosity bypass","freeze/zero-flow","reset exact replay","owned snapshots",
      "single-slot pressure","reset during readback","nonfinite controls","destroy during readback",
      "device-loss notification","shader failure","missing WebGPU"]};
}
void main().then(result=>postMessage({type:"done",result}),
  error=>postMessage({type:"error",message:error instanceof Error?error.stack:String(error)}));
