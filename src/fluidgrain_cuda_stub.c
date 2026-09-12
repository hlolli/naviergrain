/* CPU-only native builds retain the same worker and recovery path without
 * linking CUDA or requiring its toolkit/driver. No device work is attempted. */
#include "fluidgrain_cuda.h"
FGCuda *fg_cuda_create(const void *resources, size_t bytes, uint32_t capacity,
                      uint32_t frames, int device) {
  (void)resources; (void)bytes; (void)capacity; (void)frames; (void)device;
  return NULL;
}
int fg_cuda_render(FGCuda *cuda, const void *packet, size_t bytes,
                   uint32_t frames, double *stereo) {
  (void)cuda; (void)packet; (void)bytes; (void)frames; (void)stereo;
  return 0;
}
void fg_cuda_destroy(FGCuda *cuda) { (void)cuda; }
