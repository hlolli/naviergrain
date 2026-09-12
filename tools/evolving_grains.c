/* Test-only evolving trajectory planner/reference. No production DSP changes.
 * The 32-sample descriptors preserve CPU phase precision across long renders;
 * birth/death and target changes are independent of GPU delivery batch size. */
#define main fixed_grain_benchmark_main
#include "grain_benchmark.c"
#undef main
#include <string.h>
#define SPAN 32u
#define TOTAL 8192u
#define STRIDE 12u
#define MAX_LIVE 1024u
static const double smooth10 = 0.002081164700700744;
static const double smooth50 = 0.000416579873166164;
typedef struct {
  double phase, pitch, pan;
  unsigned age, length, birth, generation, loop;
} Voice;
static void birth(Voice *v, unsigned i) {
  v->phase=fmod(i*73.1234567+v->generation*197.7654321,SOURCE);
  v->pitch=-1.9+3.8*((i*19u+v->generation*7u)%127u)/126;
  v->pan=.02+.96*((i*31u)%127u)/126;
  v->age=0;v->length=703u+(i*43u+v->generation*97u)%1024u;
  v->loop=i%8u!=0;
}
int main(void) {
  const uint32_t endian=1;require(*(const unsigned char *)&endian==1);
  FGResampler reader;size_t coefficients=fg_resampler_doubles(4);
  double *storage=calloc(coefficients,sizeof(double));require(storage!=NULL);
  fg_resampler_init(&reader,storage,4);
  for(unsigned i=0;i<SOURCE;i++)source[i]=(float)(.4*sin(2*PI*17*i/SOURCE)+.2*sin(2*PI*197*i/SOURCE)+.1*cos(2*PI*401*i/SOURCE));
  size_t size=66+SOURCE+WINDOW+coefficients;
  float *data=calloc(size,sizeof(float));require(data!=NULL);
  for(unsigned b=0;b<33;b++) {data[2*b]=reader.radius[b];data[2*b+1]=(float)(66+SOURCE+WINDOW+(reader.coefficients[b]-storage));}
  for(unsigned i=0;i<SOURCE;i++)data[66+i]=(float)source[i];
  for(size_t i=0;i<coefficients;i++)data[66+SOURCE+WINDOW+i]=(float)storage[i];
  binary("input.f32",data,size*sizeof(float));
  FILE *report=create("evolving.json");
  fprintf(report,"{\"total\":%u,\"span\":%u,\"stride\":%u,\"sourceLength\":%u,\"smooth10\":%.17g,\"smooth50\":%.17g,\"compiler\":\"%s\",\"rows\":[",TOTAL,SPAN,STRIDE,SOURCE,smooth10,smooth50,__VERSION__);
  unsigned counts[]={32,256,1024};
  for(unsigned c=0;c<3;c++) {
    unsigned count=counts[c];size_t entries=(TOTAL/SPAN)*count*STRIDE;
    float *plan=calloc(entries,sizeof(float));require(plan!=NULL);
    double out[TOTAL*2]={0},continuous[TOTAL*2]={0},powers[TOTAL]={0};
    double segmentMs[TOTAL/SPAN],planMs=0,renderOverlap=0;
    Voice state[MAX_LIVE]={0};unsigned births=0,deaths=0,peak=0,live[TOTAL]={0};
    for(unsigned i=0;i<count;i++) {state[i].birth=(i*13u)%193u;birth(&state[i],i);}
    for(unsigned start=0;start<TOTAL;start+=SPAN) {
      double begin=now();
      for(unsigned i=0;i<count;i++) {
        Voice *v=&state[i];float *p=plan+((start/SPAN)*count+i)*STRIDE;
        unsigned first=start>v->birth?start:v->birth;
        if(first>=start+SPAN) continue;
        if(v->age==0)births++;
        unsigned n=SPAN-(first-start);if(n>v->length-v->age)n=v->length-v->age;
        double tp=-1.95+3.9*(.5+.5*sin(.021*(start/SPAN)+i*.071));
        double pan=.5+.48*sin(.037*(start/SPAN)+i*.11);
        p[0]=(float)floor(v->phase);p[1]=(float)(v->phase-floor(v->phase));
        p[2]=(float)v->pitch;p[3]=(float)v->pan;p[4]=(float)tp;p[5]=(float)pan;
        p[6]=(float)v->age;p[7]=(float)v->length;p[8]=(float)(first-start);p[9]=(float)n;
        p[10]=(float)v->loop;p[11]=(float)(v->generation*count+i+1);
        /* Targets use the wire precision on BOTH sides; accumulated state and
         * production reference remain double. Planning excludes convolution. */
        for(unsigned j=0;j<n;j++) {
          v->pitch+=smooth10*((double)p[4]-v->pitch);
          v->pan+=smooth10*((double)p[5]-v->pan);
          double env=(v->age==0||v->age==v->length-1)?0:.5-.5*cos(2*PI*v->age/(v->length-1));
          unsigned frame=first+j;live[frame]++;powers[frame]+=env*env;
          double inc=exp2(v->pitch);
          double sample=fg_read_bandlimited(&reader,source,SOURCE,v->phase,inc,(int)v->loop,96)*env;
          continuous[2*frame]+=sample*cos(.5*PI*v->pan);continuous[2*frame+1]+=sample*sin(.5*PI*v->pan);
          v->phase+=inc;if(v->loop)v->phase=fmod(v->phase,SOURCE);
          v->age++;
        }
        if(v->age==v->length){deaths++;v->generation++;v->birth=first+n+33+(i%29);birth(v,i);}
      }
      planMs+=now()-begin;
      /* Independently reconstruct each descriptor using the public C reader.
       * This is the exact CPU fallback contract; GPU cannot alter descriptors. */
      begin=now();double renderPower[SPAN]={0};
      for(unsigned i=0;i<count;i++) {
        const float *p=plan+((start/SPAN)*count+i)*STRIDE;
        double phase=(double)p[0]+p[1],pitch=p[2],pan=p[3];
        for(unsigned j=0;j<(unsigned)p[9];j++) {
          unsigned frame=start+(unsigned)p[8]+j,age=(unsigned)p[6]+j,len=(unsigned)p[7];
          pitch+=smooth10*(p[4]-pitch);pan+=smooth10*(p[5]-pan);
          double env=(age==0||age==len-1)?0:.5-.5*cos(2*PI*age/(len-1));
          double inc=exp2(pitch),sample=fg_read_bandlimited(&reader,source,SOURCE,phase,inc,(int)p[10],96)*env;
          out[2*frame]+=sample*cos(.5*PI*pan);out[2*frame+1]+=sample*sin(.5*PI*pan);
          renderPower[frame-start]+=env*env;
          phase+=inc;if(p[10])phase=fmod(phase,SOURCE);
        }
      }
      for(unsigned j=0;j<SPAN;j++) {
        renderOverlap+=smooth50*(renderPower[j]-renderOverlap);
        double gain=.15/sqrt(fmax(1,renderOverlap));
        out[2*(start+j)]*=gain;out[2*(start+j)+1]*=gain;
      }
      segmentMs[start/SPAN]=now()-begin;
    }
    double overlap=0;
    for(unsigned frame=0;frame<TOTAL;frame++) {
      overlap+=smooth50*(powers[frame]-overlap);
      double gain=.15/sqrt(fmax(1,overlap));
      continuous[frame*2]*=gain;continuous[frame*2+1]*=gain;
      if(live[frame]>peak)peak=live[frame];
    }
    require(fabs(overlap-renderOverlap)<1e-10);
    double checkpointError=0;
    for(unsigned k=0;k<TOTAL*2;k++)checkpointError=fmax(checkpointError,fabs(out[k]-continuous[k]));
    require(checkpointError<1e-5);
    char name[96];snprintf(name,sizeof(name),"plan-%u.f32",count);binary(name,plan,entries*sizeof(float));
    snprintf(name,sizeof(name),"reference-%u.f64",count);binary(name,out,sizeof(out));
    snprintf(name,sizeof(name),"continuous-%u.f64",count);binary(name,continuous,sizeof(continuous));
    fprintf(report,"%s{\"voices\":%u,\"births\":%u,\"deaths\":%u,\"peakLive\":%u,\"planningAndValidationMs\":%.9f,\"checkpointMaxError\":%.17g,\"finalOverlap\":%.17g,\"segmentRenderMs\":[",c?",":"",count,births,deaths,peak,planMs,checkpointError,overlap);
    for(unsigned k=0;k<TOTAL/SPAN;k++)fprintf(report,"%s%.9f",k?",":"",segmentMs[k]);
    fprintf(report,"]}");fflush(report);free(plan);
    fprintf(stderr,"Evolving %u voices: %u births, %u deaths complete\n",count,births,deaths);
  }
  fprintf(report,"]}\n");require(fclose(report)==0);free(storage);free(data);return 0;
}
