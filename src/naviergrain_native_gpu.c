#include "naviergrain_native_gpu.h"
#include "naviergrain_device_gpu.h"
#include "naviergrain_pack.h"
#include <stdlib.h>

struct NGNativeGPU {
  NGEngine *engine;
  NGDeviceGPU *device_gpu;
  NGGrainPlan *plan;
  void *packet, *scratch;
  size_t packet_bytes, scratch_bytes;
  uint32_t max_frames;
  int active;
};
void ng_native_gpu_destroy(NGNativeGPU *g) {
  if(!g)return;
  ng_device_gpu_destroy(g->device_gpu);
  free(g->plan);free(g->packet);free(g->scratch);free(g);
}
NGNativeGPU *ng_native_gpu_create(NGEngine *engine,const NGConfig *config,
                                  uint32_t frames,int device) {
  if(!engine||!config||!frames||frames>NG_GRAIN_PLAN_MAX_FRAMES)return NULL;
  NGNativeGPU *g=calloc(1,sizeof(*g));
  if(!g)return NULL;
  g->engine=engine;g->max_frames=frames;
  NGGrainResources r;
  if(device<0||!ng_engine_grain_resources(engine,&r))return g;
  size_t resource_bytes=ng_pack_resources_size(&r);
  void *resources=resource_bytes?malloc(resource_bytes):NULL;
  if(!resources)return g;
  uint32_t capacity=(uint32_t)config->value[NG_CONFIG_MAX_GRAINS];
  if(ng_pack_resources(&r,resources,resource_bytes))
    g->device_gpu=ng_device_gpu_create(resources,resource_bytes,capacity,frames,device);
  free(resources);
  if(!g->device_gpu)return g;
  size_t bytes=ng_grain_plan_size(config,frames);
  void *storage=malloc(bytes);
  g->plan=ng_grain_plan_init(storage,bytes,config,frames);
  if(!g->plan)free(storage);
  g->packet_bytes=ng_pack_size_frames(capacity,frames);
  g->scratch_bytes=ng_pack_scratch_size_frames(capacity,frames);
  g->packet=malloc(g->packet_bytes);g->scratch=malloc(g->scratch_bytes);
  if(g->plan&&g->packet&&g->scratch)g->active=1;
  else {ng_device_gpu_destroy(g->device_gpu);g->device_gpu=NULL;}
  return g;
}
int ng_native_gpu_active(const NGNativeGPU *g) {return g&&g->active;}
void ng_native_gpu_disable(NGNativeGPU *g) {if(g)g->active=0;}
int ng_native_gpu_render(NGNativeGPU *g,uint32_t frames,double *stereo) {
  if(!g||!stereo||!frames||frames>g->max_frames)return 0;
  if(!g->active) {
    for(uint32_t f=0;f<frames;++f)ng_sample(g->engine,&stereo[2*f],&stereo[2*f+1]);
    return 1;
  }
  if(!ng_grain_plan_begin(g->engine,g->plan))return 0;
  for(uint32_t f=0;f<frames;++f)
    if(!ng_grain_plan_sample(g->engine,g->plan))return 0;
  if(!ng_grain_plan_seal(g->engine,g->plan))return 0;
  size_t used=ng_pack(g->plan,g->packet,g->packet_bytes,g->scratch,g->scratch_bytes);
  if(!used||!ng_device_gpu_render(g->device_gpu,g->packet,used,frames,stereo)) {
    g->active=0;
    if(!ng_grain_plan_render(g->plan,stereo,2u*frames))return 0;
  }
  return ng_grain_plan_commit(g->engine,g->plan,stereo,2u*frames);
}
