// Fixed grain snapshot only. Each group renders one output sample; 64 lanes
// share grains, then reduce their stereo sums. No fluid solver or CPU standby.
struct Settings { voices: u32, frames: u32, sourceLength: u32, unused: u32 }
@group(0) @binding(0) var<uniform> settings: Settings;
@group(0) @binding(1) var<storage, read> data: array<f32>;
@group(0) @binding(2) var<storage, read> voices: array<f32>;
@group(0) @binding(3) var<storage, read_write> output: array<vec2<f32>>;
var<workgroup> sums: array<vec2<f32>,64>;
fn convolve(band: u32, integer: i32, phase: u32, mix: f32) -> f32 {
  let radius=u32(data[2u*band]); let taps=2u*radius;
  let a=u32(data[2u*band+1u])+phase*taps;
  let n=i32(settings.sourceLength);
  var index=((integer-i32(radius)+1)%n+n)%n;
  var result=0.0;
  for(var t=0u;t<taps;t++) {
    result+=data[66u+u32(index)]*(data[a+t]+mix*(data[a+t+taps]-data[a+t]));
    index++;if(index==n){index=0;}
  }
  return result;
}
@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) group: vec3<u32>, @builtin(local_invocation_index) lane: u32) {
  let frame=group.x;var stereo=vec2<f32>(0.0);
  for(var grain=lane;grain<settings.voices;grain+=64u) {
    let v=grain*8u;
    let position=voices[v]+f32(frame)*voices[v+1u];
    let integer=i32(floor(position));
    let frac=(position-floor(position))*128.0;
    let phase=min(127u,u32(frac));
    let band=u32(voices[v+2u]);let blend=voices[v+3u];
    var sample=convolve(band,integer,phase,frac-f32(phase));
    if(blend>0.0 && band<32u){
      sample+=blend*(convolve(band+1u,integer,phase,frac-f32(phase))-sample);
    }
    let envelope=data[66u+settings.sourceLength+u32(voices[v+6u])+frame];
    stereo+=sample*envelope*vec2<f32>(voices[v+4u],voices[v+5u]);
  }
  sums[lane]=stereo;workgroupBarrier();
  for(var stride=32u;stride>0u;stride/=2u) {
    if(lane<stride){sums[lane]+=sums[lane+stride];}workgroupBarrier();
  }
  if(lane==0u){output[frame]=sums[0]/sqrt(f32(settings.voices));}
}
