#include "fluidgrain_native_gpu.h"
#include "fluidgrain_device_gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"native GPU %d: %s\n",__LINE__,#x);exit(1);}}while(0)
/* Test-build fault injection only. Emulate failed/partial readback without
 * resetting a real device shared with other applications or plugin instances. */
static int reject_next;
int fg_device_gpu_render(FGDeviceGPU *,const void *,size_t,uint32_t,double *);
int fg_test_device_render(FGDeviceGPU *g,const void *p,size_t bytes,uint32_t n,double *out) {
  if(reject_next){reject_next=0;for(unsigned i=0;i<2*n;++i)out[i]=NAN;return 0;}
  return fg_device_gpu_render(g,p,bytes,n,out);
}
static FGEngine *engine(const FGConfig *config,double ratio) {
  size_t bytes=fg_memory_size(config,997,48000*ratio,48000);
  FGEngine *e=fg_init(malloc(bytes),bytes,config,997,48000*ratio,48000);CHECK(e);
  for(unsigned i=0;i<997;++i)fg_source(e)[i]=.2*sin(6.283185307179586*i/37);
  return e;
}
static void run(unsigned frames,int device,int loop,int inject,int carrier) {
  double values[FG_CONFIG_COUNT],c[FG_CONTROL_COUNT];fg_defaults(values,c);
  values[FG_CONFIG_MAX_GRAINS]=128;values[FG_CONFIG_EMITTER_COUNT]=32;
  values[FG_CONFIG_GRID_SIZE]=16;values[FG_CONFIG_SOURCE_LOOP]=loop;
  FGConfig config;CHECK(!fg_config_parse(&config,values,FG_CONFIG_COUNT));
  FGEngine *ref=engine(&config,.75),*actual=engine(&config,.75);
  if(carrier){CHECK(fg_source_cosine(ref));CHECK(fg_source_cosine(actual));}
  if(carrier==2){CHECK(fg_sound_space(ref,55,12000));CHECK(fg_sound_space(actual,55,12000));}
  FGNativeGPU *g=fg_native_gpu_create(actual,&config,frames,device);CHECK(g);
  CHECK(fg_native_gpu_active(g)==(device==0));
  double error=0,peak=0;
  for(unsigned b=0;b<48;++b) {
    unsigned n=b==47?7:frames;
    c[FG_CONTROL_GRAIN_RATE]=1200;c[FG_CONTROL_GRAIN_MS]=80;c[FG_CONTROL_GAIN]=.15;
    c[FG_CONTROL_PITCH_RATIO]=b<9?.5:2.25;
    c[FG_CONTROL_FREEZE]=b>=12&&b<15;c[FG_CONTROL_RESET]=b==18;
    fg_controls(ref,c);fg_controls(actual,c);
    double a[1024],out[1024];
    for(unsigned f=0;f<n;++f)fg_sample(ref,&a[2*f],&a[2*f+1]);
    if(inject&&b==27)reject_next=1;
    CHECK(!fg_native_gpu_render(g,0,out));
    CHECK(fg_native_gpu_render(g,n,out));
    CHECK(fg_native_gpu_active(g)==(device==0&&(!inject||b<27)));
    for(unsigned i=0;i<2*n;++i) {
      CHECK(isfinite(out[i]));error=fmax(error,fabs(out[i]-a[i]));peak=fmax(peak,fabs(a[i]));
      CHECK(fabs(out[i]-a[i])<1e-5);
    }
    if(!fg_native_gpu_active(g))CHECK(!memcmp(out,a,2*n*sizeof(double)));
    FGCounters ca=fg_counters(ref),cb=fg_counters(actual);
    CHECK(ca.births==cb.births&&ca.voice_drops==cb.voice_drops&&
          ca.epoch==cb.epoch&&ca.numeric_interventions==cb.numeric_interventions);
  }
  CHECK(peak>0.0001);
  printf("native Metal frames=%u device=%d loop=%d fault=%d error=%.9g peak=%.6g\n",
    frames,device,loop,inject,error,peak);
  fg_native_gpu_destroy(g);free(actual);free(ref);
}
int main(void) {
  run(512,0,1,1,0);run(32,0,0,0,0);
  run(512,0,1,1,1);
  run(512,0,1,1,2);
  run(512,-1,1,0,0);run(512,2147483647,1,0,0);
  puts("native Metal: GPU audio, partial tail, reset/freeze, injected readback failure, exact CPU recovery and absent-device fallback passed");
  return 0;
}
