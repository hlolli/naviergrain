#define _POSIX_C_SOURCE 200809L
#include <csound.h>
#include "fluidgrain_buffered.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"buffered opcode %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void settle(void){const struct timespec t={0,30000000};nanosleep(&t,NULL);}
static double ch(CSOUND *h,const char *name) {
  int32_t error;double v=csoundGetControlChannel(h,name,&error);CHECK(!error&&isfinite(v));return v;
}
static void setup_profile(CSOUND *h,const char *module,unsigned block,double rate,double duration) {
  char option[4096],csd[8192];
  snprintf(option,sizeof(option),"--opcode-lib=%s",module);CHECK(!csoundSetOption(h,option));
  snprintf(csd,sizeof(csd),
    "<CsoundSynthesizer>\n<CsOptions>\n-n -d -m0 --sample-accurate\n</CsOptions>\n<CsInstruments>\n"
    "sr=48000\nksmps=%u\nnchnls=2\n0dbfs=1\n#include \"include/fluidgrain.inc\"\n"
    "instr 1\nkControl[] fillarray $FG_CONTROL_DEFAULTS\n"
    "kControl[$FG_CONTROL_GAIN] init .15\nkControl[$FG_CONTROL_GRAIN_RATE] init %.17g\n"
    "kControl[$FG_CONTROL_GRAIN_MS] init %.17g\n"
    "kControl[$FG_CONTROL_RESET] chnget \"reset\"\nkControl[$FG_CONTROL_FREEZE] chnget \"freeze\"\n"
    "kEnable chnget \"enable\"\nkMode chnget \"mode\"\n"
    "aL,aR,kStats[],kGPU,kUnder,kQueued,kPlayed naviergrain_buffered 1,kControl,kEnable,kMode\n"
    "chnset kStats[$FG_STAT_LIVE_GRAINS],\"voices\"\n"
    "chnset kStats[$FG_STAT_VOICE_DROPS],\"drops\"\n"
    "chnset kStats[$FG_STAT_NUMERIC_INTERVENTIONS],\"interventions\"\n"
    "chnset kGPU,\"gpu\"\nchnset kUnder,\"under\"\nchnset kQueued,\"queued\"\nchnset kPlayed,\"played\"\n"
    "outs aL,aR\nendin\n</CsInstruments>\n<CsScore>\ni 1 .0001458333333333 120\ne\n</CsScore>\n</CsoundSynthesizer>\n",block,rate,duration);
  CHECK(!csoundCompileCSD(h,csd,1,0));
}
static void setup(CSOUND *h,const char *module,unsigned block) {
  setup_profile(h,module,block,600,40);
}
static void run(const char *module,const FGNativeBufferedAPI *api,int device,unsigned block) {
  double values[FG_CONFIG_COUNT],controls[FG_CONTROL_COUNT],source[997];fg_defaults(values,controls);
  values[FG_CONFIG_GRID_SIZE]=16;values[FG_CONFIG_MAX_GRAINS]=128;
  controls[FG_CONTROL_GAIN]=.15;controls[FG_CONTROL_GRAIN_RATE]=600;controls[FG_CONTROL_GRAIN_MS]=40;
  FGConfig config;CHECK(!fg_config_parse(&config,values,FG_CONFIG_COUNT));
  for(unsigned i=0;i<997;++i)source[i]=.2*sin(6.283185307179586*i/37);
  FGBuffered *b=api->create(&config,source,997,36000,48000,512,device);CHECK(b);
  FGBuffered *other=api->create(&config,source,997,36000,48000,512,-1);CHECK(other);
  size_t bytes=fg_memory_size(&config,997,36000,48000);
  FGEngine *ref=fg_init(malloc(bytes),bytes,&config,997,36000,48000);CHECK(ref);
  memcpy(fg_source(ref),source,sizeof(source));memset(source,0,sizeof(source));
  CSOUND *h=csoundCreate(NULL,NULL),*second=csoundCreate(NULL,NULL);CHECK(h&&second);
  setup(h,module,block);setup(second,module,block);
  CHECK(!api->register_buffer(h,0,b));CHECK(api->register_buffer(h,1,b));
  CHECK(!api->register_buffer(h,1,other));CHECK(!api->register_buffer(second,1,b));
  CHECK(!api->destroy(b));CHECK(api->register_buffer(second,1,other));
  CHECK(api->unregister_buffer(second,other));CHECK(api->destroy(other));
  csoundDestroy(second);
  CHECK(!csoundStart(h));csoundSetControlChannel(h,"enable",1);
  /* Preparation block: no submitted grains, even with score sample offset. */
  CHECK(!csoundPerformKsmps(h));CHECK(ch(h,"played")==0&&ch(h,"queued")==0);
  for(unsigned i=0;i<2*block;++i)CHECK(csoundGetSpout(h)[i]==0);
  CHECK(!api->unregister_buffer(h,b));CHECK(!api->destroy(b));
  double peak=0,error=0;unsigned total=0;
  for(unsigned phase=0;phase<3;++phase) {
    /* Entire four-batch submission is deterministic. Drain before the next
     * control event, so expected score/control latency is explicit. */
    csoundSetControlChannel(h,"mode",1);
    csoundSetControlChannel(h,"reset",phase==1);
    csoundSetControlChannel(h,"freeze",phase==2);
    if(phase==1)csoundSetControlChannel(h,"enable",0);
    CHECK(!csoundPerformKsmps(h));CHECK(ch(h,"queued")==2048&&ch(h,"played")==total);
    csoundSetControlChannel(h,"mode",0);
    for(unsigned i=0;i<3;++i) {
      settle();CHECK(!csoundPerformKsmps(h));
      CHECK(ch(h,"played")==total&&ch(h,"queued")==2048);
      for(unsigned f=0;f<2*block;++f)CHECK(csoundGetSpout(h)[f]==0);
    }
    CHECK(ch(h,"gpu")==(FG_TEST_DEVICE_GPU&&(device==0)&&phase==0));
    double expected[4096];
    for(unsigned batch=0;batch<4;++batch) {
      controls[FG_CONTROL_RESET]=phase==1&&batch==0;
      controls[FG_CONTROL_FREEZE]=phase==2;
      fg_controls(ref,controls);
      for(unsigned f=0;f<512;++f)fg_sample(ref,&expected[2*(batch*512+f)],&expected[2*(batch*512+f)+1]);
    }
    csoundSetControlChannel(h,"mode",2);
    unsigned consumed=0;
    for(unsigned calls=0;consumed<2048;++calls) {
      CHECK(calls<1000);CHECK(!csoundPerformKsmps(h));
      unsigned next=(unsigned)ch(h,"played")-total;CHECK(next>=consumed&&next-consumed<=block);
      const MYFLT *out=csoundGetSpout(h);
      for(unsigned f=0;f<next-consumed;++f)for(unsigned channel=0;channel<2;++channel) {
        double wanted=expected[2*(consumed+f)+channel];
        /* Restart after starvation inserts a fade in, never drops samples. */
        if(block==64&&phase&&consumed+f<32)wanted*=(double)(consumed+f+1)/32;
        double v=out[2*f+channel];CHECK(isfinite(v));
        peak=fmax(peak,fabs(v));error=fmax(error,fabs(v-wanted));CHECK(fabs(v-wanted)<1e-5);
        if((device<0||!FG_TEST_DEVICE_GPU)&&(!phase||block!=64||consumed+f>=32))CHECK(v==wanted);
      }
      for(unsigned f=next-consumed;f<block;++f)CHECK(out[2*f]==0&&out[2*f+1]==0);
      consumed=next;
    }
    total+=2048;CHECK(ch(h,"queued")==0);
    /* For non-dividing blocks, drain has already observed the empty tail.
     * The exact-block case exercises underrun and retained-frame fade recovery. */
    CHECK(ch(h,"under")==((block==64)?phase:0));
  }
  CHECK(peak>.001);CHECK(!api->unregister_buffer(h,b));
  /* Pending work at teardown: opcode/reset only detach; host joins afterwards. */
  csoundSetControlChannel(h,"mode",1);CHECK(!csoundPerformKsmps(h));
  csoundDestroy(h);CHECK(api->destroy(b));free(ref);
  printf("buffered Csound device=%d block=%u frames=%u max_error=%.9g: pause/drain/reset/freeze/downgrade/FIFO and host teardown passed\n",device,block,total,error);
}
static int rejected;
static void message(CSOUND *h,int32_t attributes,const char *text) {
  (void)h;(void)attributes;
  if(strstr(text,"needs fresh host-prepared binding"))rejected=1;
}
static void invalid_bindings(const char *module,const FGNativeBufferedAPI *api) {
  double values[FG_CONFIG_COUNT],controls[FG_CONTROL_COUNT],source[8]={.1};
  fg_defaults(values,controls);values[FG_CONFIG_GRID_SIZE]=16;
  values[FG_CONFIG_MAX_GRAINS]=32;
  FGConfig config;CHECK(!fg_config_parse(&config,values,FG_CONFIG_COUNT));
  for(unsigned test=0;test<3;++test) {
    CSOUND *h=csoundCreate(NULL,NULL);CHECK(h);setup(h,module,64);
    FGBuffered *b=NULL;
    if(test) {
      b=api->create(&config,source,8,48000,test==1?44100:48000,test==2?32:512,-1);
      CHECK(b&&api->register_buffer(h,1,b));
    }
    rejected=0;csoundSetMessageStringCallback(h,message);
    CHECK(!csoundStart(h));(void)csoundPerformKsmps(h);CHECK(rejected);
    csoundDestroy(h);CHECK(api->destroy(b));
  }
  puts("buffered Csound: missing binding, sample-rate mismatch, undersized batch rejected");
}
/* Opt-in paced host measurement, deliberately not a default CTest. No audio
 * device is opened and capacity misses are reported, not silently retried. */
static uint64_t now_ns(void) {
  struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
  return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static void until_ns(uint64_t deadline) {
#ifdef __APPLE__
  /* macOS has no clock_nanosleep. Recheck the same monotonic deadline after
   * each relative sleep, including interruptions, so pacing does not drift. */
  for(;;) {
    uint64_t now=now_ns();
    if(now>=deadline)return;
    uint64_t remaining=deadline-now;
    struct timespec t={(time_t)(remaining/1000000000u),(long)(remaining%1000000000u)};
    CHECK(!nanosleep(&t,NULL)||errno==EINTR);
  }
#else
  struct timespec t={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)};
  int error;do{error=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL);}while(error==EINTR);
  CHECK(!error);
#endif
}
static int compare_time(const void *a,const void *b) {
  uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
}
typedef struct {
  uint64_t *times,sum,max_late;
  unsigned calls,over_budget,late_calls,gpu_calls;
  double peak,voices;
} CapacityPhase;
static void capacity(const char *module,const FGNativeBufferedAPI *api,
                     const char *name,double rate,double duration,unsigned seconds) {
  const unsigned block=64,half_calls=seconds*48000u/block/2;
  double values[FG_CONFIG_COUNT],controls[FG_CONTROL_COUNT],source[4096];
  fg_defaults(values,controls);values[FG_CONFIG_MAX_GRAINS]=1024;
  FGConfig config;CHECK(!fg_config_parse(&config,values,FG_CONFIG_COUNT));
  for(unsigned i=0;i<4096;++i)source[i]=.2*sin(6.283185307179586*i/37);
  uint64_t prepare=now_ns();
  FGBuffered *b=api->create(&config,source,4096,48000,48000,512,0);CHECK(b);
  CSOUND *h=csoundCreate(NULL,NULL);CHECK(h);setup_profile(h,module,block,rate,duration);
  CHECK(api->register_buffer(h,1,b));CHECK(!csoundStart(h));
  csoundSetControlChannel(h,"enable",1);CHECK(!csoundPerformKsmps(h));
  CHECK(ch(h,"played")==0&&ch(h,"queued")==0);
  prepare=now_ns()-prepare;
  CapacityPhase phases[2]={0};
  for(unsigned i=0;i<2;++i){phases[i].times=calloc(half_calls,sizeof(uint64_t));CHECK(phases[i].times);}
  csoundSetControlChannel(h,"mode",1);
  uint64_t origin=now_ns(),first_audio=0;double previous_played=0,initial_played[2]={0};
  double initial_under[2]={0};
  for(unsigned call=0;call<2*half_calls;++call) {
    unsigned phase=call/half_calls;
    if(call==half_calls) {
      csoundSetControlChannel(h,"enable",0);
      initial_played[1]=ch(h,"played");initial_under[1]=ch(h,"under");
    }
    uint64_t deadline=origin+(uint64_t)call*block*1000000000u/48000;
    uint64_t next=origin+(uint64_t)(call+1)*block*1000000000u/48000;
    until_ns(deadline);
    uint64_t begin=now_ns();CHECK(!csoundPerformKsmps(h));uint64_t end=now_ns();
    CapacityPhase *p=&phases[phase];uint64_t cost=end-begin;
    p->times[p->calls++]=cost;p->sum+=cost;
    if(cost>next-deadline)++p->over_budget;
    uint64_t late=begin>deadline?begin-deadline:0;
    if(late>next-deadline)++p->late_calls;
    if(late>p->max_late)p->max_late=late;
    double played=ch(h,"played"),queued=ch(h,"queued");
    CHECK(played>=previous_played&&played-previous_played<=block&&queued>=0&&queued<=2048);
    if(!first_audio&&played>0)first_audio=end-origin;
    previous_played=played;p->voices=fmax(p->voices,ch(h,"voices"));
    p->gpu_calls+=ch(h,"gpu")==1;
    const MYFLT *audio=csoundGetSpout(h);
    for(unsigned i=0;i<2*block;++i){CHECK(isfinite(audio[i]));p->peak=fmax(p->peak,fabs(audio[i]));}
    if((call+1)%3750==0)fprintf(stderr,"capacity %s: %u/%u seconds, voices %.0f, underruns %.0f\n",
      name,(call+1)/750,seconds,p->voices,ch(h,"under"));
  }
  double final_played=ch(h,"played"),final_under=ch(h,"under");
  double submitted=final_played+ch(h,"queued");
  double drops=ch(h,"drops"),interventions=ch(h,"interventions");
  CHECK(first_audio&&phases[0].gpu_calls>0&&ch(h,"gpu")==0);
  csoundSetControlChannel(h,"mode",2);
  uint64_t drain_start=now_ns();
  for(unsigned call=0;ch(h,"queued")>0;++call) {
    CHECK(now_ns()-drain_start<5000000000ull);
    until_ns(drain_start+(uint64_t)call*block*1000000000u/48000);
    CHECK(!csoundPerformKsmps(h));
    const MYFLT *audio=csoundGetSpout(h);
    for(unsigned i=0;i<2*block;++i)CHECK(isfinite(audio[i]));
  }
  CHECK(ch(h,"played")==submitted);
  for(unsigned i=0;i<2;++i) {
    CapacityPhase *p=&phases[i];qsort(p->times,p->calls,sizeof(uint64_t),compare_time);
    double played=(i?final_played:initial_played[1])-initial_played[i];
    double under=(i?final_under:initial_under[1])-initial_under[i];
    printf("{\"profile\":\"%s\",\"phase\":\"%s\",\"seconds\":%u,"
      "\"calls\":%u,\"gpu_calls\":%u,\"callback_mean_ms\":%.6f,\"callback_p99_ms\":%.6f,"
      "\"callback_max_ms\":%.6f,\"callback_over_budget\":%u,\"late_host_calls\":%u,"
      "\"host_max_late_ms\":%.6f,\"played_frames\":%.0f,\"underruns\":%.0f,"
      "\"max_voices\":%.0f,\"peak\":%.9g,\"prepare_ms\":%.3f,\"first_audio_ms\":%.3f,"
      "\"voice_drops_total\":%.0f,\"interventions_total\":%.0f,\"drained_frames\":%.0f}\n",
      name,i?"cpu_downgrade":"gpu",seconds/2,p->calls,p->gpu_calls,
      (double)p->sum/p->calls/1e6,p->times[(p->calls-1)*99/100]/1e6,
      p->times[p->calls-1]/1e6,p->over_budget,p->late_calls,p->max_late/1e6,
      played,under,p->voices,p->peak,prepare/1e6,first_audio/1e6,drops,interventions,submitted);
    free(p->times);
  }
  fflush(stdout);csoundDestroy(h);CHECK(api->destroy(b));
}
int main(int argc,char **argv) {
  CHECK(argc==2||(argc==3&&!strcmp(argv[2],"--capacity")));csoundInitialize(CSOUNDINIT_NO_SIGNAL_HANDLER|CSOUNDINIT_NO_ATEXIT);
  void *module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);CHECK(module);
  const FGNativeBufferedAPI *(*entry)(void);void *symbol=dlsym(module,"fluidgrain_native_buffered_api");
  CHECK(symbol);memcpy(&entry,&symbol,sizeof(entry));const FGNativeBufferedAPI *api=entry();CHECK(api&&api->version==1);
  if(argc==3) {
    CHECK(FG_TEST_DEVICE_GPU);
    capacity(argv[1],api,"low",100,40,20);
    capacity(argv[1],api,"normal",600,80,20);
    capacity(argv[1],api,"dense",2000,200,20);
    CHECK(!dlclose(module));return 0;
  }
  invalid_bindings(argv[1],api);
  run(argv[1],api,-1,37);run(argv[1],api,0,64);
  CHECK(!dlclose(module));return 0;
}
