#include <metal_stdlib>
using namespace metal;
// Packed real-scheduler grain playback; same reduction order as CUDA/WGSL.
constant float pi = 3.14159265358979323846f;
static float sine(float angle) {
  float x=angle-floor(angle/(2*pi)+0.5f)*(2*pi);
  if(x>0.5f*pi)x=pi-x;
  if(x< -0.5f*pi)x= -pi-x;
  float z=x*x;
  return x*(1+z*(-1.0f/6+z*(1.0f/120+z*(-1.0f/5040+
    z*(1.0f/362880+z*(-1.0f/39916800+z/6227020800.0f))))));
}
static float cosine(float x) { return sine(x+0.5f*pi); }
static float value(const device uint *p, unsigned i) {return as_type<float>(p[i]);}
static float polynomial(const device uint *p,unsigned i,float t) {
  return value(p,i)+t*(value(p,i+1)+t*value(p,i+2));
}
static int wrap(int i,int n) {return ((i%n)+n)%n;}
static float convolve(const device uint *r,unsigned band,int integer,float fraction) {
  float frac=fraction*128;
  unsigned phase=min(127u,(unsigned)frac);
  float blend=frac-(float)phase;
  unsigned radius=r[16+2*band],taps=2*radius,a=r[17+2*band]+phase*taps;
  int n=(int)r[2];float sum=0;
  for(unsigned t=0;t<taps;++t) {
    int i=integer-(int)radius+1+(int)t;
    if(r[3])i=wrap(i,n);
    if(i>=0&&i<n)sum+=value(r,82+(unsigned)i)*
      (value(r,a+t)+blend*(value(r,a+t+taps)-value(r,a+t)));
  }
  return sum;
}
kernel void grains(const device uint *r [[buffer(0)]], const device uint *p [[buffer(1)]], device float2 *partial [[buffer(2)]], uint id [[thread_position_in_grid]]) {
  if(id>=p[3])return;
  unsigned index=(p[1]==2?p[10]/4:48)+id*16;
  unsigned slot=p[index],first=p[index+1],count=p[index+2];
  int base=(int)p[index+3],n=(int)r[2];bool loop=r[3]!=0;
  for(unsigned f=0;f<count;++f) {
    float t=(float)f/(float)max(1u,count-1);
    float residual=polynomial(p,index+4,t),floored=floor(residual);
    int integer=base+(int)floored;
    float fraction=residual-floored,increment=polynomial(p,index+7,t);
    float pan=polynomial(p,index+10,t),envelope=polynomial(p,index+13,t),sample=0;
    if(loop)integer=wrap(integer,n);
    if(r[1]==2&&r[6]==1) {
      sample=increment<.5f*(float)n?cosine(2*pi*((float)integer+fraction)/(float)n):0;
    } else if(loop||(integer>=0&&(integer<n-1||(integer==n-1&&fraction==0)))) {
      float range=value(r,5),location=range>0?
        min(32.0f,max(0.0f,log(max(1.0f,increment))*32/range)):0;
      unsigned band=(unsigned)floor(location);float blend=location-(float)band;
      sample=convolve(r,band,integer,fraction);
      if(blend>0&&band<32)sample+=blend*(convolve(r,band+1,integer,fraction)-sample);
      if(!loop) {
        float edge=min(max(1.0f,value(r,4)),max(1.0f,0.5f*(float)(n-1)));
        float distance=min((float)integer+fraction,(float)(n-1-integer)-fraction);
        float ramp=min(1.0f,max(0.0f,distance/edge));
        sample*=0.5f-0.5f*cosine(pi*ramp);
      }
    }
    sample*=envelope;
    partial[(first+f)*p[4]+slot]=float2(sample*cosine(0.5f*pi*pan),
                                           sample*sine(0.5f*pi*pan));
  }
}
kernel void reduce(const device uint *p [[buffer(0)]], const device float2 *partial [[buffer(1)]], device float2 *output [[buffer(2)]], uint frame [[threadgroup_position_in_grid]], uint lane [[thread_index_in_threadgroup]]) {
  threadgroup float2 sums[64];
  float2 sum=float2(0,0);
  for(uint slot=lane;slot<p[4];slot+=64)sum+=partial[frame*p[4]+slot];
  sums[lane]=sum;threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint stride=32;stride;stride/=2) {
    if(lane<stride)sums[lane]+=sums[lane+stride];
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if(!lane)output[frame]=sums[0]*value(p,16+frame);
}
