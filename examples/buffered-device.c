#define _POSIX_C_SOURCE 200809L
#include <csound.h>
#include <csound_rtaudio.h>
#include "fluidgrain_buffered.h"
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#define DEFAULT_BACKEND "auhal"
#else
#define DEFAULT_BACKEND "alsa"
#endif

static volatile sig_atomic_t stopping;
static void stop_requested(int signal_number) { (void)signal_number; stopping=1; }
static double monotonic(void) {
  struct timespec t;
  if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;
  return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
static int option(CSOUND *h,const char *prefix,const char *value) {
  char text[4096];
  int n=snprintf(text,sizeof(text),"%s%s",prefix,value);
  return n<0||(size_t)n>=sizeof(text)||csoundSetOption(h,text)!=0;
}
static double channel(CSOUND *h,const char *name) {
  int32_t error=0;
  double value=csoundGetControlChannel(h,name,&error);
  return error?NAN:value;
}
static int list_devices(CSOUND *h,const char *requested) {
  int32_t count=csoundGetAudioDevList(h,NULL,1);
  if(count<=0){fprintf(stderr,"No output devices available for this backend.\n");return 1;}
  CS_AUDIODEVICE *devices=calloc((size_t)count,sizeof(*devices));
  if(!devices)return 1;
  int32_t found=csoundGetAudioDevList(h,devices,1);
  if(found<0||found>count){free(devices);return 1;}
  int matched=requested==NULL;
  for(int32_t i=0;i<found;++i) {
    if(requested) {
      if(!strcmp(requested,devices[i].device_id))matched=1;
      continue;
    }
    printf("%s\t%s",devices[i].device_id,devices[i].device_name);
    if(devices[i].max_nchnls>0)printf("\t%d channels",devices[i].max_nchnls);
    putchar('\n');
  }
  free(devices);return found&&matched?0:1;
}
static int compile(CSOUND *h) {
  double config[FG_CONFIG_COUNT],controls[FG_CONTROL_COUNT];
  fg_defaults(config,controls);
  char defaults[2048];size_t used=0;
  for(unsigned i=0;i<FG_CONTROL_COUNT;++i) {
    int n=snprintf(defaults+used,sizeof(defaults)-used,"%s%.17g",i?",":"",controls[i]);
    if(n<0||(size_t)n>=sizeof(defaults)-used)return 1;
    used+=(size_t)n;
  }
  char csd[8192];
  int n=snprintf(csd,sizeof(csd),
    "<CsoundSynthesizer>\n<CsInstruments>\n"
    "sr=48000\nksmps=64\nnchnls=2\n0dbfs=1\n"
    "instr 1\nkControl[] fillarray %s\n"
    "kControl[%u] init .04\nkControl[%u] init 600\nkControl[%u] init 80\n"
    "kEnable chnget \"enable\"\nkMode chnget \"mode\"\n"
    "aL,aR,kStats[],kGPU,kUnder,kQueued,kPlayed fluidgrain_buffered 1,kControl,kEnable,kMode\n"
    "chnset kGPU,\"gpu\"\nchnset kUnder,\"under\"\n"
    "chnset kQueued,\"queued\"\nchnset kPlayed,\"played\"\n"
    "chnset kStats[%u],\"voices\"\nchnset kStats[%u],\"drops\"\n"
    "outs aL,aR\nendin\n</CsInstruments>\n<CsScore>\ni 1 0 -1\nf 0 z\n"
    "</CsScore>\n</CsoundSynthesizer>\n",defaults,FG_CONTROL_GAIN,
    FG_CONTROL_GRAIN_RATE,FG_CONTROL_GRAIN_MS,FG_STAT_LIVE_GRAINS,FG_STAT_VOICE_DROPS);
  if(n<0||(size_t)n>=sizeof(csd))return 1;
  return csoundCompileCSD(h,csd,1,0)!=0;
}
static int seconds_value(const char *text,double *value) {
  char *end;errno=0;*value=strtod(text,&end);
  return errno||end==text||*end||!isfinite(*value)||*value<=0||*value>3600;
}
int main(int argc,char **argv) {
  const char *module=NULL,*backend=DEFAULT_BACKEND,*output="dac";
  double seconds=10,cpu_after=0;int device=0,listing=0;
  for(int i=1;i<argc;++i) {
    if(!strcmp(argv[i],"--cpu")){device=-1;continue;}
    if(!strcmp(argv[i],"--list-devices")){listing=1;continue;}
    if(!strcmp(argv[i],"--help"))goto usage;
    if(i+1>=argc)goto bad_usage;
    const char *key=argv[i],*value=argv[++i];
    if(!strcmp(key,"--module"))module=value;
    else if(!strcmp(key,"--backend"))backend=value;
    else if(!strcmp(key,"--output"))output=value;
    else if(!strcmp(key,"--seconds")){if(seconds_value(value,&seconds))goto bad_usage;}
    else if(!strcmp(key,"--cpu-after")){if(seconds_value(value,&cpu_after))goto bad_usage;}
    else goto bad_usage;
  }
  if(!listing&&!module)goto bad_usage;
  if(strncmp(output,"dac",3)){fprintf(stderr,"--output must select a Csound DAC device.\n");return 2;}
  if(csoundInitialize(CSOUNDINIT_NO_SIGNAL_HANDLER|CSOUNDINIT_NO_ATEXIT))return 1;
  int result=1;void *library=NULL;FGBuffered *buffer=NULL;
  const FGNativeBufferedAPI *api=NULL;
  CSOUND *h=csoundCreate(NULL,NULL);if(!h)return 1;
  if(option(h,"-+rtaudio=",backend))goto cleanup;
  csoundSetRTAudioModule(h,backend);
  if(listing){result=list_devices(h,NULL);goto cleanup;}
  /* AUHAL can silently use the default device for an invalid selection.
   * Require an ID from its device list before opening an explicit output. */
  if(!strcmp(backend,"auhal")&&strcmp(output,"dac")&&list_devices(h,output)) {
    fprintf(stderr,"Cannot open device %s: select an ID from --list-devices.\n",output);
    goto cleanup;
  }
  library=dlopen(module,RTLD_NOW|RTLD_LOCAL);
  if(!library){fprintf(stderr,"Cannot load plugin: %s\n",dlerror());goto cleanup;}
  const FGNativeBufferedAPI *(*entry)(void)=NULL;
  void *symbol=dlsym(library,"fluidgrain_native_buffered_api");
  if(!symbol)goto cleanup;
  memcpy(&entry,&symbol,sizeof(entry));api=entry();
  if(!api||api->version!=1)goto cleanup;
  if(option(h,"--opcode-lib=",module)||option(h,"-o",output)||
     csoundSetOption(h,"-d")||csoundSetOption(h,"-m0")||
     csoundSetOption(h,"-b256")||csoundSetOption(h,"-B1024")||compile(h))goto cleanup;
  double values[FG_CONFIG_COUNT],controls[FG_CONTROL_COUNT],source[4096];
  fg_defaults(values,controls);FGConfig config;
  if(fg_config_parse(&config,values,FG_CONFIG_COUNT))goto cleanup;
  for(unsigned i=0;i<4096;++i)source[i]=.2*sin(6.283185307179586*(double)i/37);
  buffer=api->create(&config,source,4096,48000,48000,512,device);
  if(!buffer||!api->register_buffer(h,1,buffer))goto cleanup;
  /* This host drives Csound synchronously. Start opens the device, then one
   * silent block initializes the opcode before the device-paced render loop.
   * GPU preparation already completed above. No --realtime async init. */
  if(csoundStart(h))goto cleanup;
  csoundSetControlChannel(h,"enable",device>=0);
  csoundSetControlChannel(h,"mode",0);
  if(csoundPerformKsmps(h)||channel(h,"queued")!=0||channel(h,"played")!=0)goto cleanup;
  struct sigaction action={0};action.sa_handler=stop_requested;sigemptyset(&action.sa_mask);
  if(sigaction(SIGINT,&action,NULL)||sigaction(SIGTERM,&action,NULL))goto cleanup;
  double start=monotonic(),drain_start=0,submitted=0,max_voices=0,peak=0;
  if(start<0)goto cleanup;
  int draining=0,downgraded=device<0,saw_gpu=0,saw_cpu=0;
  csoundSetControlChannel(h,"mode",1);
  fprintf(stderr,"Device ready; Ctrl-C drains queued audio.\n");
  for(;;) {
    double now=monotonic();if(now<0)goto cleanup;
    if(!draining&&(stopping||now-start>=seconds)) {
      submitted=channel(h,"played")+channel(h,"queued");
      if(!isfinite(submitted))goto cleanup;
      csoundSetControlChannel(h,"mode",2);draining=1;drain_start=now;
    }
    if(!downgraded&&cpu_after>0&&now-start>=cpu_after) {
      csoundSetControlChannel(h,"enable",0);downgraded=1;
    }
    /* The backend paces device writes. No host sleep or GPU call is used. */
    if(csoundPerformKsmps(h)){fprintf(stderr,"Csound device performance stopped unexpectedly.\n");goto cleanup;}
    double played=channel(h,"played"),queued=channel(h,"queued");
    if(!isfinite(played)||!isfinite(queued)||queued<0||queued>2048)goto cleanup;
    max_voices=fmax(max_voices,channel(h,"voices"));
    if(played>0){saw_gpu|=channel(h,"gpu")==1;saw_cpu|=channel(h,"gpu")==0;}
    const MYFLT *audio=csoundGetSpout(h);
    for(unsigned i=0;i<128;++i){if(!isfinite(audio[i]))goto cleanup;peak=fmax(peak,fabs(audio[i]));}
    if(draining&&queued==0) {
      if(played!=submitted){fprintf(stderr,"Drain frame accounting failed.\n");goto cleanup;}
      printf("device=%s backend=%s played=%.0f underruns=%.0f voices=%.0f drops=%.0f gpu=%d cpu=%d peak=%.9g elapsed=%.3f\n",
        output,backend,played,channel(h,"under"),max_voices,channel(h,"drops"),saw_gpu,saw_cpu,peak,monotonic()-start);
      result=played>0&&peak>0?0:1;break;
    }
    if(draining&&now-drain_start>5){fprintf(stderr,"Timed out draining worker audio.\n");goto cleanup;}
  }
cleanup:
  /* Stop/close device and deinitialize the opcode before joining the worker.
   * Csound's device close flushes its remaining output buffer. */
  csoundDestroy(h);
  if(buffer&&api&&!api->destroy(buffer)){fprintf(stderr,"Worker still bound after Csound teardown.\n");result=1;}
  if(library)dlclose(library);
  return result;
bad_usage:
  fprintf(stderr,"Invalid arguments; use --help.\n");return 2;
usage:
  puts("fluidgrain_device_host --module /path/to/plugin [--backend " DEFAULT_BACKEND "] [--output dac]\n"
       "  [--seconds 10] [--cpu | --cpu-after 5]\n"
       "fluidgrain_device_host [--backend " DEFAULT_BACKEND "] --list-devices\n"
       "Ctrl-C stops submission and drains queued audio. GPU rendering is optional; CPU recovers failures.");
  return 0;
}
