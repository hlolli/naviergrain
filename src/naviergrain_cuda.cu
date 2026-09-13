#include "naviergrain_cuda.h"
#include <cuda_runtime.h>
#include <cmath>
#include <cstring>
#include <new>

/* Native port of web/grain-plan.wgsl, using the same packed coefficients,
 * phase decomposition, slot ownership and deterministic stereo reduction. */
static constexpr float pi = 3.14159265358979323846f;
__device__ static float sine(float angle) {
  float x=angle-floorf(angle/(2*pi)+0.5f)*(2*pi);
  if(x>0.5f*pi)x=pi-x;
  if(x< -0.5f*pi)x= -pi-x;
  float z=x*x;
  return x*(1+z*(-1.0f/6+z*(1.0f/120+z*(-1.0f/5040+
    z*(1.0f/362880+z*(-1.0f/39916800+z/6227020800.0f))))));
}
__device__ static float cosine(float x) { return sine(x+0.5f*pi); }
__device__ static float value(const uint32_t *p, unsigned i) {return __uint_as_float(p[i]);}
__device__ static float polynomial(const uint32_t *p,unsigned i,float t) {
  return value(p,i)+t*(value(p,i+1)+t*value(p,i+2));
}
__device__ static int wrap(int i,int n) {return ((i%n)+n)%n;}
__device__ static float convolve(const uint32_t *r,unsigned band,int integer,float fraction) {
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
__global__ static void grains(const uint32_t *r,const uint32_t *p,float2 *partial) {
  unsigned id=blockIdx.x*blockDim.x+threadIdx.x;
  if(id>=p[3])return;
  unsigned index=(p[1]==2?p[10]/4:48)+id*16;
  unsigned slot=p[index],first=p[index+1],count=p[index+2];
  int base=(int)p[index+3],n=(int)r[2];bool loop=r[3]!=0;
  for(unsigned f=0;f<count;++f) {
    float t=(float)f/(float)max(1u,count-1);
    float residual=polynomial(p,index+4,t),floored=floorf(residual);
    int integer=base+(int)floored;
    float fraction=residual-floored,increment=polynomial(p,index+7,t);
    float pan=polynomial(p,index+10,t),envelope=polynomial(p,index+13,t),sample=0;
    if(loop)integer=wrap(integer,n);
    if(r[1]==2&&r[6]==1) {
      sample=increment<.5f*(float)n?cosine(2*pi*((float)integer+fraction)/(float)n):0;
    } else if(loop||(integer>=0&&(integer<n-1||(integer==n-1&&fraction==0)))) {
      float range=value(r,5),location=range>0?
        fminf(32,fmaxf(0,logf(fmaxf(1,increment))*32/range)):0;
      unsigned band=(unsigned)floorf(location);float blend=location-(float)band;
      sample=convolve(r,band,integer,fraction);
      if(blend>0&&band<32)sample+=blend*(convolve(r,band+1,integer,fraction)-sample);
      if(!loop) {
        float edge=fminf(fmaxf(1,value(r,4)),fmaxf(1,0.5f*(float)(n-1)));
        float distance=fminf((float)integer+fraction,(float)(n-1-integer)-fraction);
        float ramp=fminf(1,fmaxf(0,distance/edge));
        sample*=0.5f-0.5f*cosine(pi*ramp);
      }
    }
    sample*=envelope;
    partial[(first+f)*p[4]+slot]=make_float2(sample*cosine(0.5f*pi*pan),
                                           sample*sine(0.5f*pi*pan));
  }
}
__global__ static void reduce(const uint32_t *p,const float2 *partial,float2 *output) {
  __shared__ float2 sums[64];
  unsigned frame=blockIdx.x,lane=threadIdx.x;float2 sum=make_float2(0,0);
  for(unsigned slot=lane;slot<p[4];slot+=64) {
    float2 x=partial[frame*p[4]+slot];sum.x+=x.x;sum.y+=x.y;
  }
  sums[lane]=sum;__syncthreads();
  for(unsigned stride=32;stride;stride/=2) {
    if(lane<stride){sums[lane].x+=sums[lane+stride].x;sums[lane].y+=sums[lane+stride].y;}
    __syncthreads();
  }
  if(!lane) {float gain=value(p,16+frame);output[frame]=make_float2(sums[0].x*gain,sums[0].y*gain);}
}
struct NGCuda {
  int device;unsigned capacity,max_frames;size_t packet_capacity;
  cudaStream_t stream=nullptr;
  uint32_t *resources=nullptr,*packet=nullptr;
  float2 *partial=nullptr,*output=nullptr,*host=nullptr;
};
extern "C" void ng_cuda_destroy(NGCuda *g) {
  if(!g)return;
  /* Best effort cleanup also covers partially prepared and lost devices. */
  if(cudaSetDevice(g->device)==cudaSuccess) {
    if(g->stream)cudaStreamSynchronize(g->stream);
    if(g->resources)cudaFree(g->resources);
    if(g->packet)cudaFree(g->packet);
    if(g->partial)cudaFree(g->partial);
    if(g->output)cudaFree(g->output);
    if(g->stream)cudaStreamDestroy(g->stream);
  }
  delete[] g->host;delete g;
}
extern "C" NGCuda *ng_cuda_create(const void *resources,size_t bytes,
                                  uint32_t capacity,uint32_t frames,int device) {
  if(!resources||bytes<328||bytes%4||!capacity||capacity>4096||!frames||frames>512||device<0)return nullptr;
  uint32_t header[4];std::memcpy(header,resources,sizeof(header));
  if(header[0]!=0x5247474eu||(header[1]!=1&&header[1]!=2)||!header[2])return nullptr;
  if(cudaSetDevice(device)!=cudaSuccess)return nullptr;
  auto *g=new(std::nothrow) NGCuda;
  if(!g)return nullptr;
  g->device=device;g->capacity=capacity;g->max_frames=frames;
  g->packet_capacity=((64+4*(size_t)frames+63)&~(size_t)63)+
    (size_t)capacity*frames*64+192;
  g->host=new(std::nothrow) float2[frames];
  if(!g->host||
     cudaStreamCreateWithFlags(&g->stream,cudaStreamNonBlocking)!=cudaSuccess||
     cudaMalloc(&g->resources,bytes)!=cudaSuccess||
     cudaMalloc(&g->packet,g->packet_capacity)!=cudaSuccess||
     cudaMalloc(&g->partial,(size_t)capacity*frames*sizeof(float2))!=cudaSuccess||
     cudaMalloc(&g->output,frames*sizeof(float2))!=cudaSuccess||
     cudaMemcpyAsync(g->resources,resources,bytes,cudaMemcpyHostToDevice,g->stream)!=cudaSuccess||
     cudaStreamSynchronize(g->stream)!=cudaSuccess) {
    ng_cuda_destroy(g);return nullptr;
  }
  return g;
}
extern "C" int ng_cuda_render(NGCuda *g,const void *packet,size_t bytes,
                              uint32_t frames,double *stereo) {
  if(!g||!packet||!stereo||!frames||frames>g->max_frames||bytes<192||bytes>g->packet_capacity)return 0;
  uint32_t h[16];std::memcpy(h,packet,sizeof(h));
  size_t offset=h[1]==2?h[10]:192;
  if(h[0]!=0x5047474eu||(h[1]!=1&&h[1]!=2)||h[2]!=frames||h[4]!=g->capacity||
     h[5]!=bytes||offset<64+4*(size_t)frames||offset%64||
     offset+(size_t)h[3]*64!=bytes||h[3]>(size_t)g->capacity*frames)return 0;
  /* Packet is owner-generated by ng_pack; overlapping segments are impossible.
   * Output remains private until BOTH kernels and complete readback succeed. */
  if(cudaSetDevice(g->device)!=cudaSuccess)return 0;
  bool ok=cudaMemsetAsync(g->partial,0,(size_t)frames*g->capacity*sizeof(float2),g->stream)==cudaSuccess;
  if(ok)ok=cudaMemcpyAsync(g->packet,packet,bytes,cudaMemcpyHostToDevice,g->stream)==cudaSuccess;
  if(ok&&h[3]) {
    grains<<<(h[3]+63)/64,64,0,g->stream>>>(g->resources,g->packet,g->partial);
    ok=cudaGetLastError()==cudaSuccess;
  }
  if(ok) {
    reduce<<<frames,64,0,g->stream>>>(g->packet,g->partial,g->output);
    ok=cudaGetLastError()==cudaSuccess;
  }
  if(ok)ok=cudaMemcpyAsync(g->host,g->output,frames*sizeof(float2),cudaMemcpyDeviceToHost,g->stream)==cudaSuccess;
  /* Drain even after a failed launch/copy: the caller may reuse its packet
   * immediately after fallback. Never leave a successful earlier upload live. */
  cudaError_t completion=cudaStreamSynchronize(g->stream);
  if(!ok||completion!=cudaSuccess)return 0;
  for(unsigned f=0;f<frames;++f)
    if(!std::isfinite(g->host[f].x)||!std::isfinite(g->host[f].y))return 0;
  for(unsigned f=0;f<frames;++f){stereo[2*f]=g->host[f].x;stereo[2*f+1]=g->host[f].y;}
  return 1;
}
