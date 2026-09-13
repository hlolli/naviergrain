#include "naviergrain_pack.h"
#include <math.h>
#include <string.h>
#include <limits.h>
static void pack_u32(unsigned char *p, uint32_t x) {
  for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(x>>(8*i));
}
static uint32_t unpack_u32(const unsigned char *p) {
  uint32_t x=0; for(unsigned i=0;i<4;++i) x|=(uint32_t)p[i]<<(8*i); return x;
}
static void pack_f32(unsigned char *p, float x) {
  uint32_t bits; memcpy(&bits,&x,4); pack_u32(p,bits);
}
static float unpack_f32(const unsigned char *p) {
  uint32_t bits=unpack_u32(p); float x; memcpy(&x,&bits,4); return x;
}
size_t ng_pack_resources_size(const NGGrainResources *r) {
  if(!r || !r->reader || !r->source || !r->source_length || r->source_length>INT32_MAX) return 0;
  size_t words=82+r->source_length;
  for(unsigned b=0;b<NG_SINC_BANDS;++b) words+=2*r->reader->radius[b]*(NG_SINC_PHASES+1u);
  return words<=UINT32_MAX/4 ? words*4 : 0;
}
int ng_pack_resources(const NGGrainResources *r,void *memory,size_t bytes) {
  size_t size=ng_pack_resources_size(r);
  if(!size || !memory || bytes<size) return 0;
  unsigned char *p=memory;memset(p,0,82*4);
  pack_u32(p,0x5247474eu);pack_u32(p+4,r->reader->cosine?2:1);pack_u32(p+8,(uint32_t)r->source_length);
  pack_u32(p+12,(uint32_t)r->loop);pack_f32(p+16,(float)r->edge_samples);
  pack_f32(p+20,(float)r->reader->log_range);
  pack_u32(p+24,r->reader->cosine?1:0);
  size_t word=82;
  for(size_t i=0;i<r->source_length;++i) {
    float x=(float)r->source[i];if(!isfinite(x))return 0;
    pack_f32(p+4*word++,x);
  }
  for(unsigned b=0;b<NG_SINC_BANDS;++b) {
    pack_u32(p+64+8*b,r->reader->radius[b]);pack_u32(p+68+8*b,(uint32_t)word);
    unsigned n=2*r->reader->radius[b]*(NG_SINC_PHASES+1u);
    for(unsigned i=0;i<n;++i)pack_f32(p+4*word++,(float)r->reader->coefficients[b][i]);
  }
  return 1;
}
static size_t pack_header(uint32_t frames) {
  return frames<=32 ? NG_PACK_HEADER : (64u+4u*frames+63u)&~(size_t)63u;
}
size_t ng_pack_size_frames(uint32_t capacity, uint32_t frames) {
  return capacity && capacity<=4096 && frames && frames<=NG_GRAIN_PLAN_MAX_FRAMES
    ? pack_header(frames)+(size_t)capacity*frames*NG_PACK_SEGMENT : 0;
}
size_t ng_pack_scratch_size_frames(uint32_t capacity, uint32_t frames) {
  return capacity && capacity<=4096 && frames && frames<=NG_GRAIN_PLAN_MAX_FRAMES
    ? (size_t)capacity*frames*sizeof(NGPlannedGrain *) : 0;
}
size_t ng_pack_size(uint32_t capacity) { return ng_pack_size_frames(capacity,32); }
size_t ng_pack_scratch_size(uint32_t capacity) { return ng_pack_scratch_size_frames(capacity,32); }
static double pack_value(const NGPlannedGrain *g, unsigned column) {
  switch(column) {case 0:return g->phase;case 1:return g->increment;
    case 2:return g->pan;default:return g->envelope;}
}
static float pack_eval(const float *p,float t) { return p[0]+t*(p[1]+t*p[2]); }
/* Fit endpoints and a middle sample, then check EVERY sample after f32
 * quantization. No assumption about the controls or particle trajectories. */
static int pack_fit(const NGPlannedGrain *const *g, unsigned n, const float *times, float *coeff, int32_t *base) {
  double integer=floor(g[0]->phase);
  if (!isfinite(integer) || integer<INT32_MIN || integer>INT32_MAX) return 0;
  *base=(int32_t)integer;
  for(unsigned c=0;c<4;++c) {
    double a=pack_value(g[0],c)-(c==0?integer:0), b=0,d=0;
    if(n>1) {
      double end=pack_value(g[n-1],c)-(c==0?integer:0)-a;
      b=end;
      if(n>2) {
        unsigned middle=(n-1)/2; double t=(double)middle/(n-1);
        double m=pack_value(g[middle],c)-(c==0?integer:0)-a;
        d=(m-t*end)/(t*t-t); b=end-d;
      }
    }
    coeff[3*c]=(float)a;coeff[3*c+1]=(float)b;coeff[3*c+2]=(float)d;
    for(unsigned f=0;f<n;++f) {
      double value=pack_eval(coeff+3*c,times[f])+(c==0?integer:0);
      double exact=pack_value(g[f],c);
      double limit=c==0?2e-6:c==1?2e-7*fmax(1,exact):2e-7;
      if(!isfinite(value)||fabs(value-exact)>limit) return 0;
      if(c==0 && (value<INT32_MIN+1024.0 || value>INT32_MAX-1024.0)) return 0;
      if(c==1 && value<=0) return 0;
      if(c>=2 && (value<0||value>1)) return 0;
    }
  }
  return 1;
}
size_t ng_pack(const NGGrainPlan *plan,void *packet,size_t bytes,void *scratch,size_t scratch_bytes) {
  NGGrainPlanView v;
  if(!ng_grain_plan_view(plan,&v)||!packet||!scratch||bytes<ng_pack_size_frames(v.capacity,v.frames)||
     scratch_bytes<ng_pack_scratch_size_frames(v.capacity,v.frames)) return 0;
  const NGPlannedGrain **lookup=scratch;
  memset(lookup,0,ng_pack_scratch_size_frames(v.capacity,v.frames));
  size_t header=pack_header(v.frames);
  unsigned char *out=packet;memset(out,0,header);
  for(unsigned f=0;f<v.frames;++f) {
    if(!isfinite((float)v.frame[f].gain))return 0;
    pack_f32(out+64+4*f,(float)v.frame[f].gain);
    for(unsigned i=0;i<v.frame[f].count;++i) {
      const NGPlannedGrain *g=&v.grain[v.frame[f].offset+i];
      if(g->slot>=v.capacity) return 0;
      lookup[g->slot*v.frames+f]=g;
    }
  }
  /* Each candidate length uses the same normalized f32 sample times for all
   * columns and segments. Cache the actual divisions (not reciprocal
   * multiplication) once per length to preserve the wire bytes exactly. */
  float times[33][32];
  uint64_t initialized_times=0;
  uint32_t segments=0;
  for(unsigned slot=0;slot<v.capacity;++slot) {
    const NGPlannedGrain **row=lookup+slot*v.frames;
    for(unsigned first=0;first<v.frames;) {
      if(!row[first]){++first;continue;}
      unsigned count=1;
      while(count<32 && first+count<v.frames && row[first+count] &&
            row[first+count]->id==row[first]->id &&
            v.frame[first+count].epoch==v.frame[first].epoch &&
            row[first+count]->phase>=row[first+count-1]->phase) ++count;
      float coeff[12];int32_t base;
      for (;;) {
        uint64_t bit=UINT64_C(1)<<count;
        if (!(initialized_times&bit)) {
          for (unsigned f=0;f<count;++f)
            times[count][f]=count>1?(float)f/(float)(count-1):0;
          initialized_times|=bit;
        }
        if (pack_fit(row+first,count,times[count],coeff,&base)) break;
        if(count==1) return 0;
        count=(count+1)/2;
      }
      unsigned char *segment=out+header+(size_t)segments*NG_PACK_SEGMENT;
      pack_u32(segment,slot);pack_u32(segment+4,first);pack_u32(segment+8,count);
      pack_u32(segment+12,(uint32_t)base);
      for(unsigned i=0;i<12;++i)pack_f32(segment+16+4*i,coeff[i]);
      ++segments;first+=count;
    }
  }
  size_t used=header+(size_t)segments*NG_PACK_SEGMENT;
  pack_u32(out,NG_PACK_MAGIC);pack_u32(out+4,v.frames<=32?1:2);
  if(v.frames>32)pack_u32(out+40,(uint32_t)header);
  pack_u32(out+8,v.frames);
  pack_u32(out+12,segments);pack_u32(out+16,v.capacity);pack_u32(out+20,(uint32_t)used);
  uint32_t peak=0;for(unsigned f=0;f<v.frames;++f)if(v.frame[f].count>peak)peak=v.frame[f].count;
  pack_u32(out+32,(uint32_t)v.grains);pack_u32(out+36,peak);
  pack_u32(out+24,(uint32_t)v.start_frame);pack_u32(out+28,(uint32_t)(v.start_frame>>32));
  return used;
}
int ng_pack_render(const NGGrainPlan *plan,const void *packet,size_t bytes,double *output,size_t samples) {
  NGGrainPlanView v;NGGrainResources r;
  if(!ng_grain_plan_view(plan,&v)||!ng_grain_plan_resources(plan,&r)||!packet||!output||
     bytes<NG_PACK_HEADER||samples<v.frames*2u) return 0;
  const unsigned char *p=packet;
  uint32_t segments=unpack_u32(p+12);
  size_t header=pack_header(v.frames);
  if(unpack_u32(p)!=NG_PACK_MAGIC||unpack_u32(p+4)!=(v.frames<=32?1u:2u)||
     (v.frames>32 && unpack_u32(p+40)!=header)||unpack_u32(p+8)!=v.frames||
     unpack_u32(p+16)!=v.capacity||segments>v.capacity*v.frames||
     bytes!=header+(size_t)segments*NG_PACK_SEGMENT||unpack_u32(p+20)!=bytes||
     unpack_u32(p+24)!=(uint32_t)v.start_frame||unpack_u32(p+28)!=(uint32_t)(v.start_frame>>32))return 0;
  /* Validate all descriptors before touching output. */
  for(unsigned j=0;j<segments;++j) {
    const unsigned char *s=p+header+j*NG_PACK_SEGMENT;
    unsigned first=unpack_u32(s+4),n=unpack_u32(s+8);
    if(unpack_u32(s)>=v.capacity||first>=v.frames||!n||n>v.frames-first)return 0;
    for(unsigned k=0;k<12;++k)if(!isfinite(unpack_f32(s+16+4*k)))return 0;
  }
  memset(output,0,v.frames*2u*sizeof(double));
  for(unsigned j=0;j<segments;++j) {
    const unsigned char *s=p+header+j*NG_PACK_SEGMENT;
    unsigned first=unpack_u32(s+4),n=unpack_u32(s+8);int32_t base;
    uint32_t bits=unpack_u32(s+12);memcpy(&base,&bits,4);
    float coeff[12];for(unsigned k=0;k<12;++k)coeff[k]=unpack_f32(s+16+4*k);
    for(unsigned i=0;i<n;++i) {
      float t=n>1?(float)i/(float)(n-1):0;
      double phase=(double)base+pack_eval(coeff,t),inc=pack_eval(coeff+3,t);
      double pan=pack_eval(coeff+6,t),env=pack_eval(coeff+9,t);
      double value=ng_read_bandlimited(r.reader,r.source,r.source_length,phase,inc,r.loop,r.edge_samples)*env;
      output[2*(first+i)]+=value*cos(1.5707963267948966*pan);
      output[2*(first+i)+1]+=value*sin(1.5707963267948966*pan);
    }
  }
  for(unsigned f=0;f<v.frames;++f){double gain=unpack_f32(p+64+4*f);output[2*f]*=gain;output[2*f+1]*=gain;}
  return 1;
}
