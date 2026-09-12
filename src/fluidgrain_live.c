#include "fluidgrain_live.h"
#include "fluidgrain_core.h"
#ifdef FG_LIVE_NATIVE
#include "fluidgrain_native_gpu.h"
#endif
#include <stdlib.h>
#include <math.h>

struct FGLive {
  FGEngine *engine;
#ifdef FG_LIVE_NATIVE
  FGNativeGPU *renderer;
#endif
  double sample_rate;
  unsigned history_cursor;
  double history[2048], fft_re[2048], fft_im[2048], spectrum[FG_LIVE_SPECTRUM_BINS];
  double particles[FG_LIVE_PARTICLE_SIZE];
  double controls[FG_CONTROL_COUNT], audio[1024], view[FG_VIEW_SIZE], stats[FG_STAT_COUNT];
};
void fg_live_defaults(double out[24]) {
  double config[FG_CONFIG_COUNT];fg_defaults(config,out);
  out[FG_CONTROL_GAIN]=.08;out[FG_CONTROL_GRAIN_RATE]=4000;
  out[FG_LIVE_LOW_HZ]=FG_LIVE_SPATIAL_FLOOR_HZ;out[FG_LIVE_HIGH_HZ]=FG_LIVE_SPATIAL_CEILING_HZ;
  out[FG_CONTROL_SCHEDULER]=1;out[FG_CONTROL_STEREO_WIDTH]=.85;
}
int fg_live_control_valid(unsigned index,double value) {
  if(index>=FG_CONTROL_COUNT||!isfinite(value))return 0;
  if(index==FG_LIVE_LOW_HZ)return value==FG_LIVE_SPATIAL_FLOOR_HZ;
  if(index==FG_LIVE_HIGH_HZ)return value==FG_LIVE_SPATIAL_CEILING_HZ;
  if(index==FG_CONTROL_SCHEDULER)return value==1;
  if(index==FG_CONTROL_STEREO_WIDTH)return value==.85;
  const FGParameter *p=&fg_control_parameters[index];
  return value>=p->minimum&&value<=p->maximum&&(!p->discrete||value==floor(value));
}
void fg_live_destroy(FGLive *l) {
  if(!l)return;
#ifdef FG_LIVE_NATIVE
  fg_native_gpu_destroy(l->renderer);
#endif
  free(l->engine);free(l);
}
FGLive *fg_live_create(double sample_rate,int device) {
  if(!isfinite(sample_rate)||sample_rate<8000||sample_rate>192000)return NULL;
  FGLive *l=calloc(1,sizeof(*l));if(!l)return NULL;
  l->sample_rate=sample_rate;
  double values[FG_CONFIG_COUNT];fg_defaults(values,l->controls);
  values[FG_CONFIG_SOURCE_LOOP]=1;
  values[FG_CONFIG_MAX_GRAINS]=FG_LIVE_PARTICLE_CAPACITY;
  /* Bound live work; the offline engine retains its larger voice capacity. */
  values[FG_CONFIG_EMITTER_COUNT]=512;
  fg_live_defaults(l->controls);
  FGConfig config;
  if(fg_config_parse(&config,values,FG_CONFIG_COUNT)){free(l);return NULL;}
  size_t bytes=fg_memory_size(&config,240,52800,sample_rate);
  void *memory=malloc(bytes);
  l->engine=fg_init(memory,bytes,&config,240,52800,sample_rate);
  if(!l->engine){free(memory);free(l);return NULL;}
  /* The unit cycle is read analytically. Sound-space frequency sets its phase
   * increment directly; the source's nominal 220 Hz rate is not a pitch base. */
  if(!fg_source_cosine(l->engine)||!fg_sound_space(l->engine,FG_LIVE_SPATIAL_FLOOR_HZ,FG_LIVE_SPATIAL_CEILING_HZ)){fg_live_destroy(l);return NULL;}
#ifdef FG_LIVE_NATIVE
  l->renderer=fg_native_gpu_create(l->engine,&config,512,device);
  if(!l->renderer){fg_live_destroy(l);return NULL;}
#else
  (void)device;
#endif
  return l;
}
int fg_live_control(FGLive *l,unsigned index,double value) {
  if(!l||!fg_live_control_valid(index,value))return 0;
  l->controls[index]=value;return 1;
}
int fg_live_render(FGLive *l,unsigned frames) {
  if(!l||!frames||frames>512)return 0;
  double core_controls[FG_CONTROL_COUNT];
  for(unsigned i=0;i<FG_CONTROL_COUNT;++i)core_controls[i]=l->controls[i];
  core_controls[FG_CONTROL_PITCH_RATIO]=1;core_controls[FG_CONTROL_PITCH_DEPTH]=0;
  fg_controls(l->engine,core_controls);
#ifdef FG_LIVE_NATIVE
  if(!fg_native_gpu_render(l->renderer,frames,l->audio))return 0;
#else
  for(unsigned f=0;f<frames;++f)fg_sample(l->engine,&l->audio[2*f],&l->audio[2*f+1]);
#endif
  for(unsigned f=0;f<frames;++f){l->history[l->history_cursor]=.5*(fmax(-1,fmin(1,l->audio[2*f]))+fmax(-1,fmin(1,l->audio[2*f+1])));l->history_cursor=(l->history_cursor+1)%2048;}
  l->controls[FG_CONTROL_RESET]=0;
  fg_stats(l->engine,l->stats);
  return 1;
}
const double *fg_live_audio(FGLive *l){return l?l->audio:NULL;}
const double *fg_live_view(FGLive *l){if(!l||!fg_view(l->engine,l->view,FG_VIEW_SIZE))return NULL;return l->view;}
const double *fg_live_stats(FGLive *l){return l?l->stats:NULL;}
int fg_live_gpu(FGLive *l){
#ifdef FG_LIVE_NATIVE
  return l&&fg_native_gpu_active(l->renderer);
#else
  (void)l;return 0;
#endif
}
void fg_live_cpu(FGLive *l){
#ifdef FG_LIVE_NATIVE
  if(l)fg_native_gpu_disable(l->renderer);
#else
  (void)l;
#endif
}

const double *fg_live_particles(FGLive *l) {
  if(!l)return NULL;
  const double *view=fg_live_view(l);double *p=l->particles;
  p[0]=2;p[1]=FG_LIVE_PARTICLE_STRIDE;p[2]=0;p[3]=FG_LIVE_PARTICLE_CAPACITY;
  p[4]=l->sample_rate;p[5]=view[6];p[6]=view[2];p[7]=view[3];
  fg_sound_bounds(l->engine,p+8);
  for(unsigned slot=0;slot<FG_LIVE_PARTICLE_CAPACITY;++slot) {
    FGGrainState g;if(!fg_grain_state(l->engine,slot,&g))continue;
    unsigned k=FG_LIVE_PARTICLE_HEADER+(unsigned)p[2]*FG_LIVE_PARTICLE_STRIDE;
    p[k]=(double)(uint32_t)g.id;p[k+1]=(double)(uint32_t)(g.id>>32);
    p[k+2]=g.x;p[k+3]=g.y;p[k+4]=g.path_x;p[k+5]=g.path_y;
    p[k+6]=g.velocity_x;p[k+7]=g.velocity_y;p[k+8]=g.speed;p[k+9]=g.frequency;
    p[k+10]=(double)g.age/g.length;p[k+11]=g.length*1000.0/l->sample_rate;
    p[k+12]=g.space_x;p[k+13]=g.space_y;p[k+14]=g.space_z;p[k+15]=g.pan;++p[2];
  }
  return p;
}
const double *fg_live_spectrum(FGLive *l) {
  if(!l)return NULL;
  const unsigned n=2048;
  for(unsigned i=0;i<n;++i){l->fft_re[i]=l->history[(l->history_cursor+i)%n]*(.5-.5*cos(6.283185307179586*i/n));l->fft_im[i]=0;}
  for(unsigned i=1,j=0;i<n;++i){unsigned bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;
    if(i<j){double t=l->fft_re[i];l->fft_re[i]=l->fft_re[j];l->fft_re[j]=t;}}
  for(unsigned width=2;width<=n;width*=2){
    double angle=-6.283185307179586/width,wr=cos(angle),wi=sin(angle);
    for(unsigned base=0;base<n;base+=width){double ar=1,ai=0;
      for(unsigned j=0;j<width/2;++j){unsigned a=base+j,b=a+width/2;
        double tr=ar*l->fft_re[b]-ai*l->fft_im[b],ti=ar*l->fft_im[b]+ai*l->fft_re[b];
        l->fft_re[b]=l->fft_re[a]-tr;l->fft_im[b]=l->fft_im[a]-ti;
        l->fft_re[a]+=tr;l->fft_im[a]+=ti;
        double next=ar*wr-ai*wi;ai=ar*wi+ai*wr;ar=next;
      }
    }
  }
  double maximum=fmin(12000,.45*l->sample_rate),range=maximum/55;
  for(unsigned band=0;band<FG_LIVE_SPECTRUM_BINS;++band){
    unsigned first=(unsigned)fmax(1,round(55*pow(range,(double)band/FG_LIVE_SPECTRUM_BINS)*n/l->sample_rate));
    unsigned end=(unsigned)fmin(n/2,round(55*pow(range,(double)(band+1)/FG_LIVE_SPECTRUM_BINS)*n/l->sample_rate));
    if(end<first)end=first;double peak=0;
    for(unsigned i=first;i<=end;++i)peak=fmax(peak,hypot(l->fft_re[i],l->fft_im[i])*4/n);
    l->spectrum[band]=fmax(-100,fmin(0,20*log10(fmax(1e-10,peak))));
  }
  return l->spectrum;
}
