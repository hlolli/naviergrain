#include "naviergrain_buffered.h"
#include "naviergrain_gpu_worker.h"
PUBLIC const NGNativeGPUWorkerAPI *naviergrain_native_gpu_api(void) {
  static const NGNativeGPUWorkerAPI api={1,ng_gpu_worker_create,
    ng_gpu_worker_destroy,ng_gpu_worker_submit,ng_gpu_worker_read};
  return &api;
}

#include <stdlib.h>
#define NG_BUFFER_REGISTRY "::naviergrain.buffered.v1"
#define NG_BUFFER_FADE 32u
struct NGBuffered {
  NGGPUWorker *worker;
  void *host;
  double sample_rate;
  uint32_t frames;
  int claimed, used;
};
typedef struct { NGBuffered *entries[16]; } NGBufferRegistry;
static NGBuffered *buffer_create(const NGConfig *config,const double *source,
    size_t length,double source_sr,double engine_sr,uint32_t frames,int device) {
  NGBuffered *b=calloc(1,sizeof(*b));
  if(!b)return NULL;
  b->worker=ng_gpu_worker_create(config,source,length,source_sr,engine_sr,frames,device);
  if(!b->worker){free(b);return NULL;}
  b->frames=frames;b->sample_rate=engine_sr;return b;
}
static int buffer_destroy(NGBuffered *b) {
  if(!b)return 1;
  if(b->host||b->claimed)return 0;
  ng_gpu_worker_destroy(b->worker);free(b);return 1;
}
static int32_t buffer_registry_reset(CSOUND *csound,void *data) {
  (void)csound;
  NGBufferRegistry *r=data;
  for(unsigned i=0;i<16;++i)if(r->entries[i]) {
    r->entries[i]->host=NULL;r->entries[i]->claimed=0;r->entries[i]=NULL;
  }
  return OK;
}
static int buffer_register(void *host,unsigned key,NGBuffered *b) {
  CSOUND *csound=host;
  if(!csound||!b||key<1||key>16||b->host||b->claimed||b->used)return 0;
  NGBufferRegistry *r=csound->QueryGlobalVariable(csound,NG_BUFFER_REGISTRY);
  if(!r) {
    if(csound->CreateGlobalVariable(csound,NG_BUFFER_REGISTRY,sizeof(*r)))return 0;
    r=csound->QueryGlobalVariable(csound,NG_BUFFER_REGISTRY);
    if(!r)return 0;
    memset(r,0,sizeof(*r));
    if(csound->RegisterResetCallback(csound,r,buffer_registry_reset)) {
      csound->DestroyGlobalVariable(csound,NG_BUFFER_REGISTRY);return 0;
    }
  }
  if(r->entries[key-1])return 0;
  r->entries[key-1]=b;b->host=host;return 1;
}
static int buffer_unregister(void *host,NGBuffered *b) {
  CSOUND *csound=host;
  if(!csound||!b||b->host!=host||b->claimed)return 0;
  NGBufferRegistry *r=csound->QueryGlobalVariable(csound,NG_BUFFER_REGISTRY);
  if(!r)return 0;
  for(unsigned i=0;i<16;++i)if(r->entries[i]==b) {
    r->entries[i]=NULL;b->host=NULL;return 1;
  }
  return 0;
}
PUBLIC const NGNativeBufferedAPI *naviergrain_native_buffered_api(void) {
  static const NGNativeBufferedAPI api={1,buffer_create,buffer_destroy,
    buffer_register,buffer_unregister};
  return &api;
}
typedef struct {
  OPDS h;
  MYFLT *left,*right;
  ARRAYDAT *stats;
  MYFLT *gpu,*underruns,*queued,*played;
  MYFLT *key;
  ARRAYDAT *controls;
  MYFLT *enable,*mode;
  NGBuffered *buffer;
  double audio[NG_GPU_WORKER_SLOTS][2u*NG_GRAIN_PLAN_MAX_FRAMES];
  uint32_t head,count,offset,inflight;
  uint64_t sequence,received,consumed,starvations;
  int running,recovering,reset_high,reset_pending,reset_needs_low,disabled,failed;
  uint32_t fade_left,fade_in;
  double last[2],fade_origin[2];
} NGBUFFEROP;
static int32_t buffered_init(CSOUND *csound,NGBUFFEROP *p) {
  if(p->buffer||!isfinite(*p->key)||*p->key<1||*p->key>16||floor(*p->key)!=*p->key||
     !vector(p->controls,NG_CONTROL_COUNT))
    return csound->InitError(csound,"naviergrain_buffered: invalid key/controls or reinit");
  NGBufferRegistry *r=csound->QueryGlobalVariable(csound,NG_BUFFER_REGISTRY);
  NGBuffered *b=r?r->entries[(unsigned)*p->key-1]:NULL;
  if(!b||b->claimed||b->used||b->sample_rate!=CS_ESR||CS_KSMPS>b->frames)
    return csound->InitError(csound,"naviergrain_buffered: needs fresh host-prepared binding at orchestra sample rate and batch >= ksmps");
  if(tabinit(csound,p->stats,NG_STAT_COUNT,p->h.insdshead)!=OK||!vector(p->stats,NG_STAT_COUNT))
    return csound->InitError(csound,"naviergrain_buffered: stats allocation failed");
  /* Opcode memory may be reused by Csound for a later note. */
  memset(&p->head,0,sizeof(*p)-offsetof(NGBUFFEROP,head));
  memset(p->stats->data,0,NG_STAT_COUNT*sizeof(MYFLT));
  *p->gpu=0;*p->underruns=0;*p->queued=0;*p->played=0;
  b->claimed=1;b->used=1;p->buffer=b;return OK;
}
static int32_t buffered_perf(CSOUND *csound,NGBUFFEROP *p) {
  uint32_t n=CS_KSMPS,start=p->h.insdshead->ksmps_offset;
  uint32_t end=n-p->h.insdshead->ksmps_no_end;
  memset(p->left,0,n*sizeof(MYFLT));memset(p->right,0,n*sizeof(MYFLT));
  if(!p->buffer||!vector(p->controls,NG_CONTROL_COUNT)||!vector(p->stats,NG_STAT_COUNT)||
     (*p->mode!=0&&*p->mode!=1&&*p->mode!=2))
    return csound->PerfError(csound,&p->h,"naviergrain_buffered: invalid controls or mode");
  if(p->failed)return OK;
  NGBuffered *b=p->buffer;
  double reset=p->controls->data[NG_CONTROL_RESET];
  if(reset==0||reset==1) {
    if(reset==1&&!p->reset_high)p->reset_pending=1;
    p->reset_high=(int)reset;
  }
  if(*p->enable!=1)p->disabled=1;
  /* Fixed bounds: no OS calls, solver, allocation or CUDA on this path. */
  for(unsigned i=0;i<NG_GPU_WORKER_SLOTS&&p->inflight;++i) {
    NGGPUResult result;
    unsigned tail=(p->head+p->count)%NG_GPU_WORKER_SLOTS;
    int read=ng_gpu_worker_read(b->worker,p->audio[tail],2u*b->frames,&result);
    if(!read)break;
    if(read<0||!result.ok||result.frames!=b->frames||result.sequence!=p->sequence||result.first_frame!=p->received) {
      p->failed=1;
      return csound->PerfError(csound,&p->h,"naviergrain_buffered: worker stream failed");
    }
    ++p->sequence;p->received+=result.frames;--p->inflight;++p->count;
    *p->gpu=(MYFLT)result.gpu_active;
    for(unsigned s=0;s<NG_STAT_COUNT;++s)p->stats->data[s]=(MYFLT)result.stats[s];
  }
  if(*p->mode==1&&end>start) {
    double controls[NG_CONTROL_COUNT];
    for(unsigned i=0;i<NG_CONTROL_COUNT;++i)controls[i]=p->controls->data[i];
    for(unsigned i=0;i<NG_GPU_WORKER_SLOTS&&p->count+p->inflight<NG_GPU_WORKER_SLOTS;++i) {
      controls[NG_CONTROL_RESET]=p->reset_pending&&!p->reset_needs_low?1:0;
      int accepted=ng_gpu_worker_submit(b->worker,controls,b->frames,!p->disabled);
      if(accepted!=1)break;
      if(controls[NG_CONTROL_RESET]!=0) {
        p->reset_pending=0;p->reset_needs_low=1;
      } else p->reset_needs_low=0;
      ++p->inflight;
    }
  }
  if(*p->mode!=0)for(uint32_t f=start;f<end;++f) {
    if(!p->running&&!p->fade_left&&
       (p->count>=2||(*p->mode==2&&p->count))) {
      p->running=1;
      if(p->recovering)p->fade_in=NG_BUFFER_FADE;
      p->recovering=0;
    }
    if(p->running&&!p->count) {
      p->running=0;
      if(*p->mode!=2||p->inflight) {
        ++p->starvations;p->recovering=1;p->fade_left=NG_BUFFER_FADE;
        p->fade_origin[0]=p->last[0];p->fade_origin[1]=p->last[1];
      }
    }
    double left=0,right=0;
    if(p->running) {
      const double *audio=p->audio[p->head]+2u*p->offset;
      double gain=p->fade_in?(double)(NG_BUFFER_FADE-p->fade_in+1)/NG_BUFFER_FADE:1;
      if(p->fade_in)--p->fade_in;
      left=audio[0]*gain;right=audio[1]*gain;
      ++p->consumed;
      if(++p->offset==b->frames){p->offset=0;p->head=(p->head+1)%NG_GPU_WORKER_SLOTS;--p->count;}
    } else if(p->fade_left) {
      double gain=(double)(--p->fade_left)/NG_BUFFER_FADE;
      left=p->fade_origin[0]*gain;right=p->fade_origin[1]*gain;
    }
    p->left[f]=(MYFLT)left;p->right[f]=(MYFLT)right;
    p->last[0]=left;p->last[1]=right;
  }
  *p->underruns=(MYFLT)p->starvations;
  *p->queued=(MYFLT)((p->count+p->inflight)*b->frames-p->offset);
  *p->played=(MYFLT)p->consumed;
  return OK;
}
static int32_t buffered_deinit(CSOUND *csound,NGBUFFEROP *p) {
  (void)csound;
  if(p->buffer){p->buffer->claimed=0;p->buffer=NULL;}
  return OK;
}
