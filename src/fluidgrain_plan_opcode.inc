/* Private offline host binding; never invoked by the ordinary opcode.
 * Each command is handled at a Csound performance boundary. ksmps must be 32:
 * host time for recover/commit calls is intentionally NOT instrument time.
 * No waiting, GPU calls or allocation occur in this performance function.
 * The host must stop/destroy the instance to cancel a pending plan. */
#define FG_PLAN_MAILBOX_MAGIC 0x4d504746u
typedef struct {
  uint32_t magic, version, bytes, action, result, frames, pending, packed_bytes;
  uint32_t resource_offset, resource_bytes, packet_offset, output_offset;
  uint32_t ticket_lo, ticket_hi, expected_lo, expected_hi, clock_lo, clock_hi;
  /* Additive v2 capability: action 4 captures an exact C-only plan. */
  uint32_t max_frames, capacity, packet_capacity, capabilities;
  uint32_t records, peak_voices;
  uint32_t reserved[8];
} FGPlanMailbox;
_Static_assert(sizeof(FGPlanMailbox)==128,"plan mailbox size");
typedef struct {
  OPDS h;
  MYFLT *address;
  ARRAYDAT *stats;
  MYFLT *table, *source_sr;
  ARRAYDAT *config, *control;
  MYFLT *max_frames_input;
  FLUIDGRAIN owner;
  AUXCH plan_storage, scratch_storage, mailbox_storage;
  FGGrainPlan *plan;
  FGPlanMailbox *mailbox;
  size_t packet_capacity, scratch_bytes, packet_offset;
  uint64_t ticket;
  uint32_t pending_frames, max_frames;
  int pending;
} FGPLANOP;
static int32_t plan_opcode_init(CSOUND *csound, FGPLANOP *p) {
  if(CS_KSMPS!=32 || p->owner.engine)
    return csound->InitError(csound,"fluidgrain_plan: needs fresh host and ksmps=32");
  double requested=*p->max_frames_input;
  if(requested==0)requested=32;
  if(!isfinite(requested)||requested<1||requested>FG_GRAIN_PLAN_MAX_FRAMES||floor(requested)!=requested)
    return csound->InitError(csound,"fluidgrain_plan: invalid prepared frame capacity");
  p->max_frames=(uint32_t)requested;
  p->owner.h=p->h;p->owner.stats=p->stats;p->owner.table=p->table;
  p->owner.source_sr=p->source_sr;p->owner.config=p->config;p->owner.control=p->control;
  int32_t result=init(csound,&p->owner);if(result!=OK)return result;
  double values[FG_CONFIG_COUNT];for(unsigned i=0;i<FG_CONFIG_COUNT;++i)values[i]=p->config->data[i];
  FGConfig config;if(fg_config_parse(&config,values,FG_CONFIG_COUNT))return NOTOK;
  size_t bytes=fg_grain_plan_size(&config,p->max_frames);
  csound->AuxAlloc(csound,bytes,&p->plan_storage);
  p->plan=fg_grain_plan_init(p->plan_storage.auxp,bytes,&config,p->max_frames);
  uint32_t capacity=(uint32_t)config.value[FG_CONFIG_MAX_GRAINS];
  p->packet_capacity=fg_pack_size_frames(capacity,p->max_frames);p->scratch_bytes=fg_pack_scratch_size_frames(capacity,p->max_frames);
  csound->AuxAlloc(csound,p->scratch_bytes,&p->scratch_storage);
  FGGrainResources resources;
  if(!fg_engine_grain_resources(p->owner.engine,&resources))return NOTOK;
  size_t resource_bytes=fg_pack_resources_size(&resources);
  size_t total=sizeof(FGPlanMailbox)+2u*p->max_frames*sizeof(double)+resource_bytes+p->packet_capacity;
  if(!resource_bytes || total>UINT32_MAX)return csound->InitError(csound,"fluidgrain_plan: resources too large");
  csound->AuxAlloc(csound,total,&p->mailbox_storage);
  if(!p->plan || !p->scratch_storage.auxp || !p->mailbox_storage.auxp)
    return csound->InitError(csound,"fluidgrain_plan: preparation allocation failed");
  p->mailbox=p->mailbox_storage.auxp;
  memset(p->mailbox,0,total);
  FGPlanMailbox *m=p->mailbox;
  m->magic=FG_PLAN_MAILBOX_MAGIC;m->version=2;
  m->max_frames=p->max_frames;m->capacity=capacity;m->packet_capacity=(uint32_t)p->packet_capacity;
  m->capabilities=1;m->bytes=(uint32_t)total;
  m->output_offset=sizeof(*m);m->resource_offset=sizeof(*m)+2u*p->max_frames*sizeof(double);
  m->resource_bytes=(uint32_t)resource_bytes;m->packet_offset=m->resource_offset+m->resource_bytes;
  p->packet_offset=m->packet_offset;
  if(!fg_pack_resources(&resources,(unsigned char *)m+m->resource_offset,resource_bytes))
    m->resource_bytes=0; /* finite double source outside f32 range: C-only */
  p->ticket=0;p->pending=0;
  *p->address=(MYFLT)(uintptr_t)m;
  return OK;
}
static int32_t plan_opcode_perf(CSOUND *csound, FGPLANOP *p) {
  FGPlanMailbox *m=p->mailbox;
  if(!m || !vector(p->control,FG_CONTROL_COUNT)||!vector(p->stats,FG_STAT_COUNT))
    return csound->PerfError(csound,&p->h,"fluidgrain_plan: invalid host state");
  uint32_t action=m->action;m->action=0;m->result=0;
  double *output=(double *)((unsigned char *)m+sizeof(*m));
  if(action==1 || action==4) {
    if(p->pending || !m->frames || m->frames>p->max_frames || p->ticket==UINT64_MAX) m->result=1;
    else {
      double controls[FG_CONTROL_COUNT];for(unsigned i=0;i<FG_CONTROL_COUNT;++i)controls[i]=p->control->data[i];
      fg_controls(p->owner.engine,controls);
      if(!fg_grain_plan_begin(p->owner.engine,p->plan))m->result=1;
      else {
        p->pending_frames=m->frames;
        for(unsigned i=0;i<p->pending_frames;++i)fg_grain_plan_sample(p->owner.engine,p->plan);
        fg_grain_plan_seal(p->owner.engine,p->plan);
        FGGrainPlanView v;fg_grain_plan_view(p->plan,&v);
        p->pending=1;++p->ticket;
        m->ticket_lo=(uint32_t)p->ticket;m->ticket_hi=(uint32_t)(p->ticket>>32);
        m->clock_lo=(uint32_t)v.start_frame;m->clock_hi=(uint32_t)(v.start_frame>>32);
        m->records=(uint32_t)v.grains;m->peak_voices=0;
        for(unsigned f=0;f<v.frames;++f)
          if(v.frame[f].count>m->peak_voices)m->peak_voices=v.frame[f].count;
        /* A failed GPU delivery keeps its original packed plan. Subsequent
         * CPU-only captures need neither float32 fitting nor packet writes. */
        m->packed_bytes=action==1?(uint32_t)fg_pack(p->plan,
          (unsigned char *)m+p->packet_offset,p->packet_capacity,
          p->scratch_storage.auxp,p->scratch_bytes):0;

      }
    }
  } else if(action) {
    uint64_t expected=(uint64_t)m->expected_lo|((uint64_t)m->expected_hi<<32);
    if(!p->pending||expected!=p->ticket)m->result=1;
    else if(action==2) {
      if(!fg_grain_plan_render(p->plan,output,2u*p->max_frames))m->result=1;
    } else if(action==3) {
      /* GPU output is explicitly provided by this trusted owner. CPU output
       * can be repeatedly regenerated first; failed/missing GPU must use it. */
      if(!fg_grain_plan_commit(p->owner.engine,p->plan,output,2u*p->pending_frames))m->result=1;
      else {p->pending=0;}
    } else m->result=1;
  }
  m->pending=(uint32_t)p->pending;
  double stats[FG_STAT_COUNT];fg_stats(p->owner.engine,stats);
  for(unsigned i=0;i<FG_STAT_COUNT;++i)p->stats->data[i]=(MYFLT)stats[i];
  *p->address=(MYFLT)(uintptr_t)m;
  return OK;
}
static int32_t plan_opcode_deinit(CSOUND *csound, FGPLANOP *p) {
  p->mailbox=NULL;p->plan=NULL;p->pending=0;
  return deinit(csound,&p->owner);
}
