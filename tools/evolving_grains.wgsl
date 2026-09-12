// Test-only: one invocation evolves one grain over 32 samples; a separate
// sample reduction and serial overlap pass preserve the audio clock.
struct Settings { voices:u32, frames:u32, sourceLength:u32, unused:u32,
  smooth10:f32, smooth50:f32, overlap:f32, gain:f32 }
@group(0) @binding(0) var<uniform> s:Settings;
@group(0) @binding(1) var<storage,read> data:array<f32>;
@group(0) @binding(2) var<storage,read> plans:array<f32>;
@group(0) @binding(3) var<storage,read_write> partial:array<vec4<f32>>;
@group(0) @binding(4) var<storage,read_write> mixed:array<vec4<f32>>;
@group(0) @binding(5) var<storage,read_write> output:array<f32>;
const PI=3.141592653589793;
fn sine(angle:f32)->f32 {
  var x=angle-floor(angle/(2.0*PI)+0.5)*(2.0*PI);
  if(x>0.5*PI){x=PI-x;}if(x< -0.5*PI){x= -PI-x;}
  let z=x*x;
  return x*(1.0+z*(-1.0/6.0+z*(1.0/120.0+z*(-1.0/5040.0+
    z*(1.0/362880.0+z*(-1.0/39916800.0+z/6227020800.0))))));
}
fn cosine(x:f32)->f32{return sine(x+0.5*PI);}
fn convolve(band:u32,integer:i32,fraction:f32,looping:bool)->f32 {
  let f=fraction*128.0;let phase=min(127u,u32(f));let blend=f-f32(phase);
  let radius=u32(data[2u*band]);let taps=2u*radius;
  let a=u32(data[2u*band+1u])+phase*taps;let n=i32(s.sourceLength);
  var index=integer-i32(radius)+1;var result=0.0;
  for(var t=0u;t<taps;t++) {
    var address=index;
    if(looping){address=((address%n)+n)%n;}
    if(address>=0 && address<n) {
      result+=data[66u+u32(address)]*(data[a+t]+blend*(data[a+t+taps]-data[a+t]));
    }
    index++;
  }
  return result;
}
@compute @workgroup_size(64)
fn grains(@builtin(global_invocation_id) id:vec3<u32>) {
  let grain=id.x%s.voices;let segment=id.x/s.voices;
  if(segment>=s.frames/32u){return;}
  let p=(segment*s.voices+grain)*12u;
  var integer=i32(plans[p]);var fraction=plans[p+1u];
  var pitch=plans[p+2u];var pan=plans[p+3u];
  let first=u32(plans[p+8u]);let count=u32(plans[p+9u]);
  let looping=plans[p+10u]>0.0;let length=plans[p+7u];
  for(var frame=0u;frame<32u;frame++) {
    var value=vec4<f32>(0.0);
    if(frame>=first && frame<first+count) {
      pitch+=s.smooth10*(plans[p+4u]-pitch);pan+=s.smooth10*(plans[p+5u]-pan);
      let age=plans[p+6u]+f32(frame-first);
      var env=0.0;if(age>0.0 && age<length-1.0){env=0.5-0.5*cosine(2.0*PI*age/(length-1.0));}
      let location=clamp(max(0.0,pitch)*16.0,0.0,32.0);
      let band=u32(floor(location));let blend=location-f32(band);
      var sample=0.0;
      let position=f32(integer)+fraction;
      if(looping || (position>=0.0 && position<=f32(s.sourceLength-1u))) {
        sample=convolve(band,integer,fraction,looping);
        if(blend>0.0 && band<32u){sample+=blend*(convolve(band+1u,integer,fraction,looping)-sample);}
        if(!looping){let ramp=clamp(min(position,f32(s.sourceLength-1u)-position)/96.0,0.0,1.0);sample*=0.5-0.5*cosine(PI*ramp);}
      }
      value=vec4<f32>(sample*env*cosine(0.5*PI*pan),sample*env*sine(0.5*PI*pan),env*env,0.0);
      fraction+=exp2(pitch);let step=i32(floor(fraction));fraction-=f32(step);integer+=step;
      if(looping){integer=integer%i32(s.sourceLength);}
    }
    partial[(segment*32u+frame)*s.voices+grain]=value;
  }
}
var<workgroup> sums:array<vec4<f32>,64>;
@compute @workgroup_size(64)
fn reduce(@builtin(workgroup_id) group:vec3<u32>,@builtin(local_invocation_index) lane:u32) {
  let frame=group.x;var v=vec4<f32>(0.0);
  for(var grain=lane;grain<s.voices;grain+=64u){v+=partial[frame*s.voices+grain];}
  sums[lane]=v;workgroupBarrier();
  for(var stride=32u;stride>0u;stride/=2u){if(lane<stride){sums[lane]+=sums[lane+stride];}workgroupBarrier();}
  if(lane==0u){mixed[frame]=sums[0];}
}
@compute @workgroup_size(1)
fn finish() {
  var overlap=s.overlap;
  for(var frame=0u;frame<s.frames;frame++) {
    let v=mixed[frame];overlap+=s.smooth50*(v.z-overlap);
    let gain=s.gain/sqrt(max(1.0,overlap));
    output[2u*frame]=v.x*gain;output[2u*frame+1u]=v.y*gain;
  }
  output[2u*s.frames]=overlap;
}
