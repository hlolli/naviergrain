#ifndef FLUIDGRAIN_CUDA_H
#define FLUIDGRAIN_CUDA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct FGCuda FGCuda;
/* Owner-thread-only, synchronous OFFLINE renderer. Preparation allocates all
 * device buffers. Input is the trusted output of fg_pack_resources/fg_pack;
 * no engine, source or Csound pointer is retained by the device.
 * Failure leaves caller output untouched. Destroy only after render returns.
 * No cudaDeviceReset: other plugin instances may share the device. */
FGCuda *fg_cuda_create(const void *resources, size_t resource_bytes,
                       uint32_t capacity, uint32_t max_frames, int device);
int fg_cuda_render(FGCuda *, const void *packet, size_t bytes,
                    uint32_t frames, double *stereo);
void fg_cuda_destroy(FGCuda *);
#ifdef __cplusplus
}
#endif
#endif
