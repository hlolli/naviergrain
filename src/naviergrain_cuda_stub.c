/* CPU-only native builds retain the same worker and recovery path without
 * linking CUDA or requiring its toolkit/driver. No device work is attempted. */
#include "naviergrain_cuda.h"
NGCuda *ng_cuda_create(const void *resources, size_t bytes, uint32_t capacity,
                      uint32_t frames, int device) {
  (void)resources; (void)bytes; (void)capacity; (void)frames; (void)device;
  return NULL;
}
int ng_cuda_render(NGCuda *cuda, const void *packet, size_t bytes,
                   uint32_t frames, double *stereo) {
  (void)cuda; (void)packet; (void)bytes; (void)frames; (void)stereo;
  return 0;
}
void ng_cuda_destroy(NGCuda *cuda) { (void)cuda; }
