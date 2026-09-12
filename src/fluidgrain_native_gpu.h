#ifndef FLUIDGRAIN_NATIVE_GPU_H
#define FLUIDGRAIN_NATIVE_GPU_H
#include "fluidgrain_grain_plan.h"
typedef struct FGNativeGPU FGNativeGPU;
/* Prepared, owner-thread-only OFFLINE integration. Borrows the engine until
 * destroy. GPU absence/preparation failure selects C playback automatically.
 * Controls are set on the engine between render calls. No host or UI needed.
 * GPU failure recovers the SAME plan, then selects C for subsequent batches.
 * This API performs synchronous GPU work: never call from a live callback. */
FGNativeGPU *fg_native_gpu_create(FGEngine *, const FGConfig *,
                                  uint32_t max_frames, int device);
int fg_native_gpu_render(FGNativeGPU *, uint32_t frames, double *stereo);
int fg_native_gpu_active(const FGNativeGPU *);
void fg_native_gpu_disable(FGNativeGPU *);
void fg_native_gpu_destroy(FGNativeGPU *);
#endif
