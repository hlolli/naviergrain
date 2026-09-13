#ifndef NAVIERGRAIN_DEVICE_GPU_H
#define NAVIERGRAIN_DEVICE_GPU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NGDeviceGPU NGDeviceGPU;
/* Prepared synchronous renderer, called only by the synthesis owner/worker.
 * Trusted ng_pack packets. A failed render leaves output untouched. */
NGDeviceGPU *ng_device_gpu_create(const void *, size_t, uint32_t, uint32_t, int);
int ng_device_gpu_render(NGDeviceGPU *, const void *, size_t, uint32_t, double *);
void ng_device_gpu_destroy(NGDeviceGPU *);
#ifdef __cplusplus
}
#endif
#endif
