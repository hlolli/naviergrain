#define _POSIX_C_SOURCE 200809L
#include "fluidgrain_gpu_worker.h"
#include "fluidgrain_native_gpu.h"
#include "fluidgrain_grain_plan.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

enum { SLOT_FREE, SLOT_REQUEST, SLOT_READY };
typedef struct {
  atomic_uint state;
  uint32_t frames;
  int enable;
  double controls[FG_CONTROL_COUNT];
  double audio[2u * FG_GRAIN_PLAN_MAX_FRAMES];
  FGGPUResult result;
} FGWorkSlot;
struct FGGPUWorker {
  FGEngine *engine;
  FGConfig config;
  uint32_t max_frames;
  int device;
  pthread_t thread;
  pthread_mutex_t prepare_lock;
  pthread_cond_t prepared;
  int preparation_done, preparation_ok;
  atomic_uint stop;
  /* Host-owned indices: only slot states are shared. */
  unsigned submit_slot, read_slot;
  FGWorkSlot slots[FG_GPU_WORKER_SLOTS];
};
static void *worker_main(void *data) {
  FGGPUWorker *w=data;
  FGNativeGPU *renderer=fg_native_gpu_create(w->engine,&w->config,w->max_frames,w->device);
  pthread_mutex_lock(&w->prepare_lock);
  w->preparation_ok=renderer!=NULL;w->preparation_done=1;
  pthread_cond_signal(&w->prepared);
  pthread_mutex_unlock(&w->prepare_lock);
  unsigned next=0;
  uint64_t sequence=0,frame=0;
  int failed=0;
  while(renderer&&!atomic_load_explicit(&w->stop,memory_order_acquire)) {
    FGWorkSlot *slot=&w->slots[next];
    if(atomic_load_explicit(&slot->state,memory_order_acquire)!=SLOT_REQUEST) {
      /* Worker-only polling avoids callback-side OS wakeups/locks. */
      const struct timespec idle={0,250000};nanosleep(&idle,NULL);continue;
    }
    if(!slot->enable)fg_native_gpu_disable(renderer);
    fg_controls(w->engine,slot->controls);
    int ok=!failed&&fg_native_gpu_render(renderer,slot->frames,slot->audio);
    if(!ok){failed=1;memset(slot->audio,0,2u*slot->frames*sizeof(double));}
    slot->result=(FGGPUResult){.sequence=sequence++,.first_frame=frame,
      .frames=slot->frames,.gpu_active=fg_native_gpu_active(renderer),.ok=ok};
    fg_stats(w->engine,slot->result.stats);
    frame+=slot->frames;
    atomic_store_explicit(&slot->state,SLOT_READY,memory_order_release);
    next=(next+1u)%FG_GPU_WORKER_SLOTS;
  }
  fg_native_gpu_destroy(renderer);
  return NULL;
}
void fg_gpu_worker_destroy(FGGPUWorker *w) {
  if(!w)return;
  atomic_store_explicit(&w->stop,1,memory_order_release);
  pthread_join(w->thread,NULL);
  pthread_cond_destroy(&w->prepared);pthread_mutex_destroy(&w->prepare_lock);
  free(w->engine);free(w);
}
FGGPUWorker *fg_gpu_worker_create(const FGConfig *config,const double *source,
    size_t length,double source_sr,double engine_sr,uint32_t frames,int device) {
  FGConfig checked;
  if(!config||!source||!length||!frames||frames>FG_GRAIN_PLAN_MAX_FRAMES||device< -1||
     !isfinite(source_sr)||source_sr<=0||!isfinite(engine_sr)||engine_sr<=0||
     fg_config_parse(&checked,config->value,FG_CONFIG_COUNT)||
     checked.value[FG_CONFIG_BACKEND]!=0)return NULL;
  for(size_t i=0;i<length;++i)if(!isfinite(source[i]))return NULL;
  FGGPUWorker *w=calloc(1,sizeof(*w));if(!w)return NULL;
  w->config=checked;w->max_frames=frames;w->device=device;
  atomic_init(&w->stop,0);
  if(!atomic_is_lock_free(&w->stop)){free(w);return NULL;}
  for(unsigned i=0;i<FG_GPU_WORKER_SLOTS;++i) {
    atomic_init(&w->slots[i].state,SLOT_FREE);
    if(!atomic_is_lock_free(&w->slots[i].state)){free(w);return NULL;}
  }
  size_t bytes=fg_memory_size(&checked,length,source_sr,engine_sr);
  void *storage=bytes?malloc(bytes):NULL;
  w->engine=storage?fg_init(storage,bytes,&checked,length,source_sr,engine_sr):NULL;
  if(!w->engine){free(storage);free(w);return NULL;}
  memcpy(fg_source(w->engine),source,length*sizeof(double));
  if(pthread_mutex_init(&w->prepare_lock,NULL)){free(w->engine);free(w);return NULL;}
  if(pthread_cond_init(&w->prepared,NULL)) {
    pthread_mutex_destroy(&w->prepare_lock);free(w->engine);free(w);return NULL;
  }
  if(pthread_create(&w->thread,NULL,worker_main,w)) {
    pthread_cond_destroy(&w->prepared);pthread_mutex_destroy(&w->prepare_lock);
    free(w->engine);free(w);return NULL;
  }
  pthread_mutex_lock(&w->prepare_lock);
  while(!w->preparation_done)pthread_cond_wait(&w->prepared,&w->prepare_lock);
  int ok=w->preparation_ok;
  pthread_mutex_unlock(&w->prepare_lock);
  if(!ok){fg_gpu_worker_destroy(w);return NULL;}
  return w;
}
int fg_gpu_worker_submit(FGGPUWorker *w,const double *controls,uint32_t frames,int enable) {
  if(!w||!controls||!frames||frames>w->max_frames||(enable!=0&&enable!=1))return -1;
  FGWorkSlot *slot=&w->slots[w->submit_slot];
  if(atomic_load_explicit(&slot->state,memory_order_acquire)!=SLOT_FREE)return 0;
  memcpy(slot->controls,controls,sizeof(slot->controls));
  slot->frames=frames;slot->enable=enable;
  atomic_store_explicit(&slot->state,SLOT_REQUEST,memory_order_release);
  w->submit_slot=(w->submit_slot+1u)%FG_GPU_WORKER_SLOTS;
  return 1;
}
int fg_gpu_worker_read(FGGPUWorker *w,double *audio,size_t samples,FGGPUResult *result) {
  if(!w||!audio||!result)return -1;
  FGWorkSlot *slot=&w->slots[w->read_slot];
  if(atomic_load_explicit(&slot->state,memory_order_acquire)!=SLOT_READY)return 0;
  if(samples<2u*slot->frames)return -1;
  memcpy(audio,slot->audio,2u*slot->frames*sizeof(double));*result=slot->result;
  atomic_store_explicit(&slot->state,SLOT_FREE,memory_order_release);
  w->read_slot=(w->read_slot+1u)%FG_GPU_WORKER_SLOTS;
  return 1;
}
