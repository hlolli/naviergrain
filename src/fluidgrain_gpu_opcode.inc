/* Explicit native CUDA opcode. Synchronous, OFFLINE only: no worker or
 * hidden buffering changes Csound score/control timing. Ordinary fluidgrain
 * remains CPU-only. kGPU is 1 while CUDA is active, 0 after C fallback.
 * iDevice=-1 selects C; kEnable=0 permanently downgrades this instance.
 * Reprepare a new instance to retry GPU after a downgrade. */
#include "fluidgrain_native_gpu.h"
typedef struct {
  OPDS h;
  MYFLT *left,*right;
  ARRAYDAT *stats;
  MYFLT *active;
  MYFLT *table,*source_sr;
  ARRAYDAT *config,*control;
  MYFLT *device,*enable;
  FLUIDGRAIN owner;
  FGNativeGPU *renderer;
  AUXCH output;
} FGGPUOP;
static int32_t gpu_opcode_init(CSOUND *csound,FGGPUOP *p) {
  const OPARMS *options=csound->GetOParms(csound);
  if(options&&(options->realtime||
      (options->sfwrite&&options->outfilename&&!strncmp(options->outfilename,"dac",3))||
      (options->sfread&&options->infilename&&!strncmp(options->infilename,"adc",3))))
    return csound->InitError(csound,"fluidgrain_gpu: synchronous offline renderer; use fluidgrain for live audio");
  if(p->renderer||p->owner.engine)
    return csound->InitError(csound,"fluidgrain_gpu: reinit requires a fresh instance");
  if(CS_KSMPS<1||CS_KSMPS>FG_GRAIN_PLAN_MAX_FRAMES)
    return csound->InitError(csound,"fluidgrain_gpu: offline block size must be 1..512");
  if(!isfinite(*p->device)||*p->device< -1||*p->device>INT32_MAX||
     floor(*p->device)!=*p->device)
    return csound->InitError(csound,"fluidgrain_gpu: invalid CUDA device");
  p->owner.h=p->h;p->owner.stats=p->stats;p->owner.table=p->table;
  p->owner.source_sr=p->source_sr;p->owner.config=p->config;p->owner.control=p->control;
  int32_t result=init(csound,&p->owner);
  if(result!=OK)return result;
  double values[FG_CONFIG_COUNT];
  for(unsigned i=0;i<FG_CONFIG_COUNT;++i)values[i]=p->config->data[i];
  FGConfig config;
  if(fg_config_parse(&config,values,FG_CONFIG_COUNT))return NOTOK;
  csound->AuxAlloc(csound,2u*CS_KSMPS*sizeof(double),&p->output);
  if(!p->output.auxp) {
    deinit(csound,&p->owner);
    return csound->InitError(csound,"fluidgrain_gpu: output allocation failed");
  }
  p->renderer=fg_native_gpu_create(p->owner.engine,&config,CS_KSMPS,(int)*p->device);
  if(!p->renderer) {
    deinit(csound,&p->owner);
    return csound->InitError(csound,"fluidgrain_gpu: renderer allocation failed");
  }
  *p->active=(MYFLT)fg_native_gpu_active(p->renderer);
  return OK;
}
static int32_t gpu_opcode_perf(CSOUND *csound,FGGPUOP *p) {
  uint32_t n=CS_KSMPS,start=p->h.insdshead->ksmps_offset;
  uint32_t end=n-p->h.insdshead->ksmps_no_end;
  memset(p->left,0,n*sizeof(MYFLT));memset(p->right,0,n*sizeof(MYFLT));
  if(!p->renderer||!vector(p->control,FG_CONTROL_COUNT)||!vector(p->stats,FG_STAT_COUNT))
    return csound->PerfError(csound,&p->h,"fluidgrain_gpu: invalid live arrays");
  if(*p->enable!=1)fg_native_gpu_disable(p->renderer);
  double controls[FG_CONTROL_COUNT];
  for(unsigned i=0;i<FG_CONTROL_COUNT;++i)controls[i]=p->control->data[i];
  fg_controls(p->owner.engine,controls);
  double *output=p->output.auxp;
  if(end>start&&!fg_native_gpu_render(p->renderer,end-start,output))
    return csound->PerfError(csound,&p->h,"fluidgrain_gpu: plan recovery failed");
  for(uint32_t f=start;f<end;++f){p->left[f]=(MYFLT)output[2*(f-start)];p->right[f]=(MYFLT)output[2*(f-start)+1];}
  double stats[FG_STAT_COUNT];fg_stats(p->owner.engine,stats);
  for(unsigned i=0;i<FG_STAT_COUNT;++i)p->stats->data[i]=(MYFLT)stats[i];
  *p->active=(MYFLT)fg_native_gpu_active(p->renderer);
  return OK;
}
static int32_t gpu_opcode_deinit(CSOUND *csound,FGGPUOP *p) {
  fg_native_gpu_destroy(p->renderer);p->renderer=NULL;
  return deinit(csound,&p->owner);
}
