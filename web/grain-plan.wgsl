// Real C-scheduler compact segments. Resources and packet are little-endian
// 32-bit words; phase integer stays separate from its fractional polynomial.
@group(0) @binding(0) var<storage,read> r:array<u32>;
@group(0) @binding(1) var<storage,read> p:array<u32>;
@group(0) @binding(2) var<storage,read_write> partial:array<vec2<f32>>;
@group(0) @binding(3) var<storage,read_write> output:array<vec2<f32>>;
const PI=3.141592653589793;
fn sine(angle:f32)->f32 {
 var x=angle-floor(angle/(2.0*PI)+0.5)*(2.0*PI);
 if(x>0.5*PI){x=PI-x;}if(x< -0.5*PI){x= -PI-x;}
 let z=x*x;return x*(1.0+z*(-1.0/6.0+z*(1.0/120.0+z*(-1.0/5040.0+z*(1.0/362880.0+z*(-1.0/39916800.0+z/6227020800.0))))));
}
fn cosine(x:f32)->f32{return sine(x+0.5*PI);}
fn value(index:u32)->f32{return bitcast<f32>(r[index]);}
fn polynomial(index:u32,t:f32)->f32{return bitcast<f32>(p[index])+t*(bitcast<f32>(p[index+1u])+t*bitcast<f32>(p[index+2u]));}
fn convolve(band:u32,integer:i32,fraction:f32)->f32 {
 let frac=fraction*128.0;let phase=min(127u,u32(frac));let blend=frac-f32(phase);
 let radius=r[16u+2u*band];let taps=2u*radius;let a=r[17u+2u*band]+phase*taps;
 let n=i32(r[2]);var sum=0.0;
 for(var t=0u;t<taps;t++) {
  var index=integer-i32(radius)+1+i32(t);
  if(r[3]!=0u){index=((index%n)+n)%n;}
  if(index>=0 && index<n){sum+=value(82u+u32(index))*(value(a+t)+blend*(value(a+t+taps)-value(a+t)));}
 }
 return sum;
}
@compute @workgroup_size(64)
fn grains(@builtin(global_invocation_id) id:vec3<u32>) {
 if(id.x>=p[3]){return;}
 let index=select(48u,p[10]/4u,p[1]==2u)+id.x*16u;let slot=p[index];let first=p[index+1u];let count=p[index+2u];let base=bitcast<i32>(p[index+3u]);
 for(var f=0u;f<count;f++) {
  let t=f32(f)/f32(max(1u,count-1u));let residual=polynomial(index+4u,t);
  let carry=i32(floor(residual));var integer=base+carry;let fraction=residual-floor(residual);
  let increment=polynomial(index+7u,t);let pan=polynomial(index+10u,t);let envelope=polynomial(index+13u,t);
  var sample=0.0;let n=i32(r[2]);let looping=r[3]!=0u;
  if(looping){integer=((integer%n)+n)%n;}
  if(r[1]==2u && r[6]==1u) {
   if(increment<0.5*f32(n)){sample=cosine(2.0*PI*(f32(integer)+fraction)/f32(n));}
  } else if(looping || (integer>=0 && (integer<n-1 || (integer==n-1 && fraction==0.0)))) {
   var location=0.0;let logRange=value(5u);
   if(logRange>0.0){location=clamp(log(max(1.0,increment))*32.0/logRange,0.0,32.0);}
   let band=u32(floor(location));let blend=location-f32(band);
   sample=convolve(band,integer,fraction);
   if(blend>0.0 && band<32u){sample+=blend*(convolve(band+1u,integer,fraction)-sample);}
   if(!looping) {
    let edge=min(max(1.0,value(4u)),max(1.0,0.5*f32(n-1)));let distance=min(f32(integer)+fraction,f32(n-1-integer)-fraction);
    let ramp=clamp(distance/edge,0.0,1.0);sample*=0.5-0.5*cosine(PI*ramp);
   }
  }
  partial[(first+f)*p[4]+slot]=sample*envelope*vec2<f32>(cosine(0.5*PI*pan),sine(0.5*PI*pan));
 }
}
var<workgroup> sums:array<vec2<f32>,64>;
@compute @workgroup_size(64)
fn reduce(@builtin(workgroup_id) group:vec3<u32>,@builtin(local_invocation_index) lane:u32) {
 let frame=group.x;var sum=vec2<f32>(0.0);
 for(var slot=lane;slot<p[4];slot+=64u){sum+=partial[frame*p[4]+slot];}
 sums[lane]=sum;workgroupBarrier();
 for(var stride=32u;stride>0u;stride/=2u){if(lane<stride){sums[lane]+=sums[lane+stride];}workgroupBarrier();}
 if(lane==0u){output[frame]=sums[0]*bitcast<f32>(p[16u+frame]);}
}
