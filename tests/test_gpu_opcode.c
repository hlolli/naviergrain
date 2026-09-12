#include <csound.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"GPU opcode %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static atomic_int offline_rejected;
static void live_message(CSOUND *h,int32_t attributes,const char *text) {
  (void)h;(void)attributes;
  if(strstr(text,"synchronous offline renderer"))atomic_store(&offline_rejected,1);
}
static CSOUND *prepare(const char *module,int gpu,int device,unsigned frames) {
  CSOUND *h=csoundCreate(NULL,NULL);CHECK(h);
  char option[4096];snprintf(option,sizeof(option),"--opcode-lib=%s",module);
  CHECK(!csoundSetOption(h,option));
  if(gpu==2) {
    csoundSetMessageStringCallback(h,live_message);
    CHECK(!csoundSetOption(h,"--realtime"));
  }
  char call[512],csd[8192];
  if(gpu)snprintf(call,sizeof(call),
    "aL,aR,kStats[],kGPU fluidgrain_gpu giSource,sr*.75,iConfig,kControl,%d,kEnable\nchnset kGPU,\"gpu\"",device);
  else snprintf(call,sizeof(call),"aL,aR,kStats[] fluidgrain giSource,sr*.75,iConfig,kControl");
  snprintf(csd,sizeof(csd),
    "<CsoundSynthesizer>\n<CsOptions>\n-n -d -m0 --sample-accurate\n</CsOptions>\n<CsInstruments>\n"
    "sr=48000\nksmps=%u\nnchnls=2\n0dbfs=1\n#include \"include/fluidgrain.inc\"\n"
    "giSource ftgen 1,0,-997,10,1,.2,.1\ninstr 1\n"
    "iConfig[] fillarray $FG_CONFIG_DEFAULTS\niConfig[$FG_CONFIG_GRID_SIZE]=16\n"
    "iConfig[$FG_CONFIG_MAX_GRAINS]=128\niConfig[$FG_CONFIG_SOURCE_LOOP]=1\n"
    "kControl[] fillarray $FG_CONTROL_DEFAULTS\nkControl[$FG_CONTROL_GRAIN_RATE] init 1200\n"
    "kControl[$FG_CONTROL_GAIN] init .1\nkControl[$FG_CONTROL_RESET] chnget \"reset\"\n"
    "kEnable chnget \"enable\"\n%s\nouts aL,aR\nendin\n</CsInstruments>\n"
    "<CsScore>\ni 1 0.000145833333333 0.31\ne\n</CsScore>\n</CsoundSynthesizer>\n",frames,call);
  if(gpu==2) {
    /* Global i-time runs synchronously, unlike queued realtime score notes.
     * No device is opened: this tests the explicit --realtime guard. */
    snprintf(csd,sizeof(csd),
      "<CsoundSynthesizer>\n<CsOptions>\n-n -d -m0\n</CsOptions>\n<CsInstruments>\n"
      "sr=48000\nksmps=32\nnchnls=2\n0dbfs=1\n#include \"include/fluidgrain.inc\"\n"
      "giSource ftgen 1,0,-997,10,1\niConfig[] fillarray $FG_CONFIG_DEFAULTS\n"
      "kControl[] fillarray $FG_CONTROL_DEFAULTS\nkEnable init 1\n"
      "aL,aR,kStats[],kGPU fluidgrain_gpu giSource,sr,iConfig,kControl,0,kEnable\n"
      "</CsInstruments>\n<CsScore>\nf 0 .01\ne\n</CsScore>\n</CsoundSynthesizer>\n");
    int result=csoundCompileCSD(h,csd,1,0);
    if(!result)result=csoundStart(h);
    CHECK(result!=0&&atomic_load(&offline_rejected));
    return h;
  }
  CHECK(!csoundCompileCSD(h,csd,1,0));CHECK(!csoundStart(h));
  csoundSetControlChannel(h,"enable",1);return h;
}
static void run(const char *module,int device,unsigned frames) {
  CSOUND *ref=prepare(module,0,device,frames),*gpu=prepare(module,1,device,frames);
  unsigned blocks=0;double peak=0,error=0;
  for(;;++blocks) {
    csoundSetControlChannel(ref,"reset",blocks==10);
    csoundSetControlChannel(gpu,"reset",blocks==10);
    /* Downgrade on command, then ensure re-enabling does not silently retry. */
    csoundSetControlChannel(gpu,"enable",blocks==20?0:1);
    int a=csoundPerformKsmps(ref),b=csoundPerformKsmps(gpu);CHECK(a==b);
    if(a)break;
    int32_t channel_error=0;
    double active=csoundGetControlChannel(gpu,"gpu",&channel_error);CHECK(!channel_error);
    CHECK(active==(device==0&&blocks<20));
    const MYFLT *x=csoundGetSpout(ref),*y=csoundGetSpout(gpu);
    for(unsigned i=0;i<2*frames;++i) {
      CHECK(isfinite(y[i]));peak=fmax(peak,fabs(x[i]));error=fmax(error,fabs(x[i]-y[i]));
      CHECK(fabs(x[i]-y[i])<1e-5);
    }
    if(device<0||blocks>=20)CHECK(!memcmp(x,y,2*frames*sizeof(MYFLT)));
    if(blocks==0)for(unsigned i=0;i<14;++i)CHECK(x[i]==0&&y[i]==0);
    CHECK(blocks<1000);
  }
  CHECK(peak>.001);
  printf("Csound CUDA device=%d block=%u calls=%u error=%.9g\n",device,frames,blocks,error);
  csoundDestroy(gpu);csoundDestroy(ref);
}
int main(int argc,char **argv) {
  CHECK(argc==2);csoundInitialize(CSOUNDINIT_NO_SIGNAL_HANDLER|CSOUNDINIT_NO_ATEXIT);
  run(argv[1],0,512);run(argv[1],-1,37);
  CSOUND *live=prepare(argv[1],2,0,32);
  CHECK(atomic_load(&offline_rejected));
  csoundDestroy(live);
  puts("native CUDA opcode: actual Csound render, independent instances, downgrade, exact fallback, score offsets/tails and teardown passed");
  return 0;
}
