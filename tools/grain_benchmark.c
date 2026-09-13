/* Test-only fixed-grain hot-path comparison; production reader, no solver.
 * Build: cc -std=c11 -O3 -DNDEBUG -Isrc tools/grain_benchmark.c
 *        -lm -o build/grain_benchmark
 * Run from a fresh output directory. Files use the local little-endian host. */
#define _POSIX_C_SOURCE 200809L
#include "../src/naviergrain_resampler.c" /* reuse the canonical span kernels */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SOURCE 1021u
#define VOICES 4096u
#define WINDOW 8193u
#define PI 3.14159265358979323846
static double source[SOURCE], window[WINDOW];
static float voices[VOICES][8];
static double now(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void require(int ok) { if (!ok) { perror("grain benchmark"); exit(1); } }
static FILE *create(const char *name) { FILE *f=fopen(name,"wx"); require(f!=NULL); return f; }
static void binary(const char *name, const void *data, size_t size) {
  FILE *f=create(name); require(fwrite(data,1,size,f)==size); require(fclose(f)==0);
}
/* Identical prepared band choices to WGSL, reusing the production convolution.
 * This additional baseline removes per-sample logarithm and band-selection cost.
 * Input pitches/positions are finite positive dyadic fixtures by construction. */
static double prepared_read(const NGResampler *reader, const float *v, double position) {
  double integer=floor(position), frac=(position-integer)*NG_SINC_PHASES;
  unsigned phase=(unsigned)fmin(NG_SINC_PHASES-1, floor(frac));
  unsigned band=(unsigned)v[2];double blend=v[3];
  double result=convolve(reader,band,source,SOURCE,integer,phase,frac-phase,1);
  if(blend>0 && band+1<NG_SINC_BANDS)
    result+=blend*(convolve(reader,band+1,source,SOURCE,integer,phase,frac-phase,1)-result);
  return result;
}
static void render(const NGResampler *reader, unsigned count, unsigned frames, double *out, int prepared) {
  for (unsigned frame=0;frame<frames;++frame) {
    double l=0,r=0;
    for (unsigned i=0;i<count;++i) {
      const float *v=voices[i];
      double env=window[(unsigned)v[6]+frame];
      double position=v[0]+(double)frame*v[1];
      double sample=(prepared ? prepared_read(reader,v,position) :
        ng_read_bandlimited(reader,source,SOURCE,position,v[1],1,96))*env;
      l+=sample*v[4]; r+=sample*v[5];
    }
    /* Fixed gain only: isolate convolution/window/pan/mix from overlap smoothing. */
    out[2*frame]=l/sqrt(count);out[2*frame+1]=r/sqrt(count);
  }
}
int main(void) {
  const uint32_t endian=1; require(*(const unsigned char *)&endian==1);
  NGResampler reader; size_t coefficients=ng_resampler_doubles(4);
  double *storage=calloc(coefficients,sizeof(double));require(storage!=NULL);
  ng_resampler_init(&reader,storage,4);
  for(unsigned i=0;i<SOURCE;++i)
    source[i]=(float)(.4*sin(2*PI*17*i/SOURCE)+.2*sin(2*PI*197*i/SOURCE)+.1*cos(2*PI*401*i/SOURCE));
  for(unsigned i=0;i<WINDOW;++i) window[i]=(float)(.5-.5*cos(2*PI*i/(WINDOW-1)));
  for(unsigned i=0;i<VOICES;++i) {
    float *v=voices[i];v[0]=(float)((i*73u)%SOURCE)+ (i%128u)/128.0f;
    v[1]=(16u+(i*37u)%241u)/64.0f; /* exact dyadic pitches from .25 to 4 */
    double location=log(fmax(1,v[1]))*32/reader.log_range;
    v[2]=(float)floor(location);v[3]=(float)(location-floor(location));
    v[4]=(float)cos(.5*PI*(i%127u)/126);v[5]=(float)sin(.5*PI*(i%127u)/126);
    v[6]=(float)((i*41u)%4096u);
  }
  /* One resident input buffer: band metadata, source, Hann, coefficients. */
  size_t size=66+SOURCE+WINDOW+coefficients;
  float *data=calloc(size,sizeof(float));require(data!=NULL);
  for(unsigned b=0;b<33;++b) {
    data[2*b]=(float)reader.radius[b];
    data[2*b+1]=(float)(66+SOURCE+WINDOW+(reader.coefficients[b]-storage));
  }
  for(unsigned i=0;i<SOURCE;++i)data[66+i]=(float)source[i];
  for(unsigned i=0;i<WINDOW;++i)data[66+SOURCE+i]=(float)window[i];
  for(size_t i=0;i<coefficients;++i)data[66+SOURCE+WINDOW+i]=(float)storage[i];
  binary("input.f32",data,size*sizeof(float));binary("voices.f32",voices,sizeof(voices));
  FILE *report=create("cpu.json");
  fprintf(report,"{\"sourceLength\":%u,\"windowLength\":%u,\"compiler\":\"%s\",\"rows\":[",SOURCE,WINDOW,__VERSION__);
  const unsigned counts[]={32,256,1024,4096},blocks[]={64,512,2048};
  double out[4096];int first=1;
  for(unsigned c=0;c<4;++c)for(unsigned b=0;b<3;++b){
    unsigned n=counts[c],frames=blocks[b];double times[7],cached[7];
    render(&reader,n,frames,out,0); /* warm source/filter cache */
    for(unsigned k=0;k<7;++k){double start=now();render(&reader,n,frames,out,0);times[k]=now()-start;}
    char filename[80];snprintf(filename,sizeof(filename),"reference-%u-%u.f64",n,frames);
    binary(filename,out,2*frames*sizeof(double));
    fprintf(report,"%s{\"voices\":%u,\"frames\":%u,\"ms\":[",first?"":",",n,frames);first=0;
    for(unsigned k=0;k<7;++k)fprintf(report,"%s%.9f",k?",":"",times[k]);
    double prepared[4096];render(&reader,n,frames,prepared,1);
    double error=0;
    for(unsigned i=0;i<2*frames;++i)error=fmax(error,fabs(out[i]-prepared[i]));
    require(error<1e-6);
    for(unsigned k=0;k<7;++k){double start=now();render(&reader,n,frames,prepared,1);cached[k]=now()-start;}
    snprintf(filename,sizeof(filename),"prepared-%u-%u.f64",n,frames);
    binary(filename,prepared,2*frames*sizeof(double));
    fprintf(report,"],\"preparedMs\":[");
    for(unsigned k=0;k<7;++k)fprintf(report,"%s%.9f",k?",":"",cached[k]);
    fprintf(report,"],\"preparedMaxError\":%.12g}",error);fflush(report);
    fprintf(stderr,"CPU %u grains x %u samples complete\n",n,frames);
  }
  fprintf(report,"]}\n");require(fclose(report)==0);free(data);free(storage);return 0;
}
