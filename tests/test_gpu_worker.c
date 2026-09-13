#define _POSIX_C_SOURCE 200809L
#include "naviergrain_gpu_worker.h"
#include "naviergrain_cuda.h"
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"GPU worker %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static pthread_t host;
static atomic_uint calls, hold, entered, wrong_thread;
static unsigned fail_call;
static void pause_short(void){const struct timespec t={0,250000};nanosleep(&t,NULL);}
int __real_ng_cuda_render(NGCuda *,const void *,size_t,uint32_t,double *);
int __wrap_ng_cuda_render(NGCuda *g,const void *p,size_t bytes,uint32_t n,double *out) {
  if(pthread_equal(host,pthread_self()))atomic_store(&wrong_thread,1);
  unsigned call=atomic_fetch_add(&calls,1)+1;
  atomic_store(&entered,1);
  while(atomic_load(&hold))pause_short();
  if(call==fail_call){for(unsigned i=0;i<2*n;++i)out[i]=NAN;return 0;}
  return __real_ng_cuda_render(g,p,bytes,n,out);
}
static void controls(unsigned batch,double *c) {
  double config[NG_CONFIG_COUNT];ng_defaults(config,c);
  c[NG_CONTROL_GAIN]=.15;c[NG_CONTROL_GRAIN_RATE]=1000;c[NG_CONTROL_GRAIN_MS]=80;
  c[NG_CONTROL_PITCH_RATIO]=batch<12?.5:2.25;
  c[NG_CONTROL_RESET]=batch==16;c[NG_CONTROL_FREEZE]=batch>=23&&batch<26;
}
static void run(int device,unsigned fault,int disable) {
  atomic_store(&calls,0);atomic_store(&entered,0);atomic_store(&hold,device==0);
  fail_call=fault;
  double values[NG_CONFIG_COUNT],c[NG_CONTROL_COUNT],source[997];ng_defaults(values,c);
  values[NG_CONFIG_MAX_GRAINS]=128;values[NG_CONFIG_EMITTER_COUNT]=32;
  values[NG_CONFIG_GRID_SIZE]=16;
  NGConfig config;CHECK(!ng_config_parse(&config,values,NG_CONFIG_COUNT));
  for(unsigned i=0;i<997;++i)source[i]=.2*sin(6.283185307179586*i/37);
  NGGPUWorker *w=ng_gpu_worker_create(&config,source,997,36000,48000,512,device);CHECK(w);
  size_t bytes=ng_memory_size(&config,997,36000,48000);
  NGEngine *ref=ng_init(malloc(bytes),bytes,&config,997,36000,48000);CHECK(ref);
  memcpy(ng_source(ref),source,sizeof(source));memset(source,0,sizeof(source));
  double out[1024],expected[1024];NGGPUResult result;
  memset(out,0x5a,sizeof(out));memset(&result,0x5a,sizeof(result));
  CHECK(ng_gpu_worker_read(w,out,1024,&result)==0);
  CHECK(out[0]!=0&&result.frames==0x5a5a5a5a);
  CHECK(ng_gpu_worker_submit(w,c,0,1)==-1);
  CHECK(ng_gpu_worker_submit(w,c,513,1)==-1);
  for(unsigned b=0;b<4;++b){controls(b,c);CHECK(ng_gpu_worker_submit(w,c,512,1)==1);}
  c[NG_CONTROL_RESET]=1;
  CHECK(ng_gpu_worker_submit(w,c,512,0)==0); /* rejected reset/downgrade */
  if(device==0) {
    for(unsigned i=0;i<20000&&!atomic_load(&entered);++i)pause_short();
    CHECK(atomic_load(&entered));
    /* Device thread is deliberately blocked. Both host operations return. */
    CHECK(ng_gpu_worker_read(w,out,1024,&result)==0);
    CHECK(ng_gpu_worker_submit(w,c,512,0)==0);
    atomic_store(&hold,0);
  }
  unsigned submitted=4;uint64_t frame=0;double error=0,peak=0;
  for(unsigned b=0;b<40;++b) {
    int read=0;
    for(unsigned i=0;i<20000;++i) {
      read=ng_gpu_worker_read(w,out,1,&result);
      if(read)break;
      pause_short();
    }
    CHECK(read==-1); /* insufficient output must not consume */
    CHECK(ng_gpu_worker_read(w,out,1024,&result)==1);
    unsigned n=b==39?7:512;
    CHECK(result.ok&&result.sequence==b&&result.first_frame==frame&&result.frames==n);
    frame+=n;controls(b,c);ng_controls(ref,c);
    for(unsigned f=0;f<n;++f)ng_sample(ref,&expected[2*f],&expected[2*f+1]);
    for(unsigned i=0;i<2*n;++i) {
      CHECK(isfinite(out[i]));error=fmax(error,fabs(out[i]-expected[i]));
      peak=fmax(peak,fabs(out[i]));CHECK(fabs(out[i]-expected[i])<1e-5);
    }
    int active=device==0&&(!fault||b+1<fault)&&(!disable||b<20);
    CHECK(result.gpu_active==active);
    if(!active)CHECK(!memcmp(out,expected,2*n*sizeof(double)));
    double stats[NG_STAT_COUNT];ng_stats(ref,stats);
    for(unsigned i=0;i<NG_STAT_COUNT;++i) {
      if(i==NG_STAT_PRE_LIMITER_PEAK)CHECK(fabs(stats[i]-result.stats[i])<1e-5);
      else CHECK(stats[i]==result.stats[i]);
    }
    if(submitted<40) {
      controls(submitted,c);n=submitted==39?7:512;
      CHECK(ng_gpu_worker_submit(w,c,n,!(disable&&submitted==20))==1);
      ++submitted;
    }
  }
  CHECK(peak>0.001&&!atomic_load(&wrong_thread));
  CHECK(ng_gpu_worker_read(w,out,1024,&result)==0);
  /* Teardown with accepted unconsumed work, joined outside callback. */
  CHECK(ng_gpu_worker_submit(w,c,512,1)==1);
  ng_gpu_worker_destroy(w);free(ref);
  printf("worker device=%d fault=%u disable=%d: FIFO/reset/freeze/partial tail, exact fallback, source ownership; max error %.9g\n",device,fault,disable,error);
}
static void plugin(const char *path) {
  void *module=dlopen(path,RTLD_NOW|RTLD_LOCAL);CHECK(module);
  const NGNativeGPUWorkerAPI *(*entry)(void);
  *(void **)(&entry)=dlsym(module,"naviergrain_native_gpu_api");CHECK(entry);
  const NGNativeGPUWorkerAPI *api=entry();CHECK(api&&api->version==1);
  double values[NG_CONFIG_COUNT],c[NG_CONTROL_COUNT],source[997];ng_defaults(values,c);
  values[NG_CONFIG_GRID_SIZE]=16;values[NG_CONFIG_MAX_GRAINS]=128;
  NGConfig config;CHECK(!ng_config_parse(&config,values,NG_CONFIG_COUNT));
  for(unsigned i=0;i<997;++i)source[i]=.2*sin(6.283185307179586*i/37);
  NGGPUWorker *w=api->create(&config,source,997,48000,48000,512,0);CHECK(w);
  size_t bytes=ng_memory_size(&config,997,48000,48000);
  NGEngine *ref=ng_init(malloc(bytes),bytes,&config,997,48000,48000);CHECK(ref);
  memcpy(ng_source(ref),source,sizeof(source));
  double peak=0;
  for(unsigned b=0;b<12;++b) {
    controls(b,c);CHECK(api->submit(w,c,512,b!=6)==1);
    double out[1024],expected[1024];NGGPUResult result;int ready=0;
    for(unsigned i=0;i<20000&&!ready;++i){ready=api->read(w,out,1024,&result);if(!ready)pause_short();}
    CHECK(ready==1&&result.ok&&result.gpu_active==(b<6));
    CHECK(result.sequence==b&&result.first_frame==512u*b);
    ng_controls(ref,c);
    for(unsigned f=0;f<512;++f)ng_sample(ref,&expected[2*f],&expected[2*f+1]);
    for(unsigned i=0;i<1024;++i){CHECK(fabs(out[i]-expected[i])<1e-5);peak=fmax(peak,fabs(out[i]));}
    if(b>=6)CHECK(!memcmp(out,expected,sizeof(out)));
  }
  CHECK(peak>.001);api->destroy(w);free(ref);CHECK(!dlclose(module));
  puts("Csound plugin export: prepared native worker, GPU audio and permanent exact CPU downgrade passed");
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  host=pthread_self();
  run(-1,0,0);run(0,8,0);run(0,0,1);
  plugin(argv[1]);
  puts("Native worker: bounded nonblocking host handoff and GPU-to-CPU recovery passed");
}
