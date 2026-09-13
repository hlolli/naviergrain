#ifndef NAVIERGRAIN_NATIVE_GPU_H
#define NAVIERGRAIN_NATIVE_GPU_H
#include "naviergrain_grain_plan.h"
typedef struct NGNativeGPU NGNativeGPU;
/* Prepared, owner-thread-only OFFLINE integration. Borrows the engine until
 * destroy. GPU absence/preparation failure selects C playback automatically.
 * Controls are set on the engine between render calls. No host or UI needed.
 * GPU failure recovers the SAME plan, then selects C for subsequent batches.
 * This API performs synchronous GPU work: never call from a live callback. */
NGNativeGPU *ng_native_gpu_create(NGEngine *, const NGConfig *,
                                  uint32_t max_frames, int device);
int ng_native_gpu_render(NGNativeGPU *, uint32_t frames, double *stereo);
int ng_native_gpu_active(const NGNativeGPU *);
void ng_native_gpu_disable(NGNativeGPU *);
void ng_native_gpu_destroy(NGNativeGPU *);
#endif
