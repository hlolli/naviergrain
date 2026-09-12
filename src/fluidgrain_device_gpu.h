#ifndef FLUIDGRAIN_DEVICE_GPU_H
#define FLUIDGRAIN_DEVICE_GPU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct FGDeviceGPU FGDeviceGPU;
/* Prepared synchronous renderer, called only by the synthesis owner/worker.
 * Trusted fg_pack packets. A failed render leaves output untouched. */
FGDeviceGPU *fg_device_gpu_create(const void *, size_t, uint32_t, uint32_t, int);
int fg_device_gpu_render(FGDeviceGPU *, const void *, size_t, uint32_t, double *);
void fg_device_gpu_destroy(FGDeviceGPU *);
#ifdef __cplusplus
}
#endif
#endif
