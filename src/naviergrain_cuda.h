#ifndef NAVIERGRAIN_CUDA_H
#define NAVIERGRAIN_CUDA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NGCuda NGCuda;
/* Owner-thread-only, synchronous OFFLINE renderer. Preparation allocates all
 * device buffers. Input is the trusted output of ng_pack_resources/ng_pack;
 * no engine, source or Csound pointer is retained by the device.
 * Failure leaves caller output untouched. Destroy only after render returns.
 * No cudaDeviceReset: other plugin instances may share the device. */
NGCuda *ng_cuda_create(const void *resources, size_t resource_bytes,
                       uint32_t capacity, uint32_t max_frames, int device);
int ng_cuda_render(NGCuda *, const void *packet, size_t bytes,
                    uint32_t frames, double *stereo);
void ng_cuda_destroy(NGCuda *);
#ifdef __cplusplus
}
#endif
#endif
