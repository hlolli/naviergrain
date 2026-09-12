#include "fluidgrain_device_gpu.h"
#include "fluidgrain_cuda.h"
FGDeviceGPU *fg_device_gpu_create(const void *r,size_t n,uint32_t c,uint32_t f,int d) {
  return (FGDeviceGPU *)fg_cuda_create(r,n,c,f,d);
}
int fg_device_gpu_render(FGDeviceGPU *g,const void *p,size_t n,uint32_t f,double *out) {
  return fg_cuda_render((FGCuda *)g,p,n,f,out);
}
void fg_device_gpu_destroy(FGDeviceGPU *g) {fg_cuda_destroy((FGCuda *)g);}
