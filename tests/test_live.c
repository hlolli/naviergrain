#include "fluidgrain_live.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fluidgrain_core.h"
#include "fluidgrain_mapping.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"live %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int density(int device,int deadline) {
  FGLive *live=fg_live_create(48000,device);CHECK(live);
  if(device==0)CHECK(fg_live_gpu(live));
  CHECK(fg_live_control(live,FG_CONTROL_GRAIN_RATE,32000));
  CHECK(!fg_live_control(live,FG_CONTROL_GRAIN_RATE,32001));
  CHECK(fg_live_control(live,FG_CONTROL_MAPPING_MIX,0));
  double voices=0,peak=0;clock_t start=clock();struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);
  for(unsigned b=0;b<96;++b) {
    CHECK(fg_live_render(live,512));
    const double *a=fg_live_audio(live),*s=fg_live_stats(live);
    voices=fmax(voices,s[FG_STAT_LIVE_GRAINS]);
    CHECK(fg_live_particles(live)[2]==s[FG_STAT_LIVE_GRAINS]);
    CHECK(s[FG_STAT_LIVE_GRAINS]<=FG_LIVE_PARTICLE_CAPACITY);
    for(unsigned i=0;i<1024;++i){CHECK(isfinite(a[i]));peak=fmax(peak,fabs(a[i]));}
  }
  CHECK(voices==FG_LIVE_PARTICLE_CAPACITY);CHECK(peak>.001);
  CHECK(fg_live_stats(live)[FG_STAT_VOICE_DROPS]>0);
  clock_gettime(CLOCK_MONOTONIC,&end);double elapsed=end.tv_sec-begin.tv_sec+(end.tv_nsec-begin.tv_nsec)*1e-9;
  printf("32000 starts/s: %.0f peak voices, %.6f peak amplitude, GPU=%d, %.3f wall / %.3f CPU seconds for 1.024s audio\n",voices,peak,fg_live_gpu(live),elapsed,(double)(clock()-start)/CLOCKS_PER_SEC);
  if(deadline)CHECK(elapsed<1.024*.8);
  if(device==0)CHECK(fg_live_gpu(live));fg_live_destroy(live);return 0;
}
int main(int argc,char **argv) {
  if(argc>=2&&(!strcmp(argv[1],"--density")||!strcmp(argv[1],"--deadline")))return density(argc==3?atoi(argv[2]):-1,!strcmp(argv[1],"--deadline"));
  CHECK(!fg_live_create(NAN,-1));CHECK(!fg_live_create(0,-1));
  FGLive *live=fg_live_create(48000,-1);CHECK(live&&!fg_live_gpu(live));
  CHECK(fg_live_particles(live)[2]==0);
  for(unsigned i=0;i<FG_LIVE_SPECTRUM_BINS;++i)CHECK(fg_live_spectrum(live)[i]==-100);
  CHECK(fg_cosine_bend(.2,.1)<fg_cosine_bend(.8,.1));
  CHECK(fg_cosine_bend(.5,.1)<fg_cosine_bend(.5,2));
  CHECK(fg_cosine_duration(200,1,1,.1)>fg_cosine_duration(200,1,1,2));
  CHECK(!fg_live_control(live,0,NAN));CHECK(!fg_live_control(live,24,0));
  CHECK(fg_live_control(live,FG_CONTROL_SCHEDULER,1));CHECK(!fg_live_control(live,FG_CONTROL_SCHEDULER,0));
  CHECK(fg_live_control(live,FG_CONTROL_STEREO_WIDTH,.85));CHECK(!fg_live_control(live,FG_CONTROL_STEREO_WIDTH,0));
  CHECK(!fg_live_control(live,3,10));CHECK(!fg_live_control(live,21,.5));
  CHECK(!fg_live_render(live,0));CHECK(!fg_live_render(live,513));
  FILE *file=argc==2?fopen(argv[1],"wb"):NULL;if(argc==2)CHECK(file);
  double peak=0,lowest=12000,highest=0,last[FG_LIVE_PARTICLE_SIZE]={0};unsigned occupied[96]={0},moved=0;
  for(unsigned b=0;b<32;++b) {
    CHECK(fg_live_control(live,FG_LIVE_LOW_HZ,55));
    CHECK(fg_live_control(live,10,b<12?.7:-.7));
    CHECK(fg_live_control(live,21,b>=16&&b<20));
    if(b==24)CHECK(fg_live_control(live,22,1));
    CHECK(fg_live_render(live,512));const double *audio=fg_live_audio(live);
    const double *particles=fg_live_particles(live);
    CHECK(particles[2]==fg_live_stats(live)[FG_STAT_LIVE_GRAINS]);
    for(unsigned i=0;i<(unsigned)particles[2];++i){
      unsigned k=FG_LIVE_PARTICLE_HEADER+i*FG_LIVE_PARTICLE_STRIDE;
      CHECK(particles[k+8]>=0&&particles[k+9]>=55-1e-8&&particles[k+9]<=12000+1e-8);
      double height=particles[k+13]/3.6+.5;
      CHECK(height>=0&&height<=1);
      double expected=particles[8]*pow(particles[9]/particles[8],height);
      CHECK(fabs(particles[k+9]-expected)<1e-7);
      CHECK(fabs(particles[k+15]-(.5+.425*particles[k+12]))<1e-12);
      CHECK(particles[k+15]>=.075&&particles[k+15]<=.925);
      lowest=fmin(lowest,particles[k+9]);highest=fmax(highest,particles[k+9]);
      if(b>0&&b<8)for(unsigned j=0;j<(unsigned)last[2];++j){
        unsigned q=FG_LIVE_PARTICLE_HEADER+j*FG_LIVE_PARTICLE_STRIDE;
        if(particles[k]==last[q]&&particles[k+1]==last[q+1]){
          double dy=particles[k+13]-last[q+13],df=particles[k+9]-last[q+9];
          if(fabs(dy)>1e-8){CHECK(df*dy>0);++moved;}break;
        }
      }
      CHECK(particles[k+10]>=0&&particles[k+10]<1);
      CHECK(particles[k+11]>=5&&particles[k+11]<=500);
    }
    for(unsigned i=0;i<1024;++i){CHECK(isfinite(audio[i]));peak=fmax(peak,fabs(audio[i]));}
    if(file)CHECK(fwrite(audio,sizeof(double),1024,file)==1024);
    const double *spectrum=fg_live_spectrum(live);
    for(unsigned i=0;i<96;++i)if(spectrum[i]>-65)occupied[i]=1;
    memcpy(last,particles,(FG_LIVE_PARTICLE_HEADER+(unsigned)particles[2]*FG_LIVE_PARTICLE_STRIDE)*sizeof(double));
  }
  unsigned covered=0;for(unsigned i=0;i<96;++i)covered+=occupied[i];
  CHECK(lowest<100&&highest>8000);CHECK(covered>=80);CHECK(moved>20);
  printf("Sound space: %.1f..%.1f Hz, %u/96 FFT bands above -65 dBFS\n",lowest,highest,covered);
  const double *view=fg_live_view(live);CHECK(view&&view[0]==1&&view[1]==3344);
  if(file){
    CHECK(fwrite(view,sizeof(double),3344,file)==3344);
    const double *particles=fg_live_particles(live);unsigned n=FG_LIVE_PARTICLE_HEADER+(unsigned)particles[2]*FG_LIVE_PARTICLE_STRIDE;
    CHECK(fwrite(particles,sizeof(double),n,file)==n);
    CHECK(fwrite(fg_live_spectrum(live),sizeof(double),96,file)==96);
    CHECK(!fclose(file));
  }
  CHECK(peak>.001);fg_live_destroy(live);
  live=fg_live_create(48000,-1);CHECK(live);
  CHECK(!fg_live_control(live,FG_LIVE_LOW_HZ,1000));CHECK(!fg_live_control(live,FG_LIVE_HIGH_HZ,1200));
  FGLive *reference=fg_live_create(48000,-1);CHECK(reference);
  for(unsigned b=0;b<32;++b){
    CHECK(fg_live_render(live,512)&&fg_live_render(reference,512));
    CHECK(!memcmp(fg_live_audio(live),fg_live_audio(reference),1024*sizeof(double)));
  }
  fg_live_destroy(reference);
  fg_live_destroy(live);
  live=fg_live_create(8000,-1);CHECK(live&&fg_live_render(live,512));
  const double *limited=fg_live_particles(live);CHECK(limited[9]<=3600+1e-7);
  fg_live_destroy(live);puts("live engine: fixed spatial frequency law, rejected remapping, position/pan, Nyquist limit, reset/freeze passed");
  return 0;
}
