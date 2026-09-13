#include "naviergrain_device_gpu.h"
#include "naviergrain_cuda.h"
NGDeviceGPU *ng_device_gpu_create(const void *r,size_t n,uint32_t c,uint32_t f,int d) {
  return (NGDeviceGPU *)ng_cuda_create(r,n,c,f,d);
}
int ng_device_gpu_render(NGDeviceGPU *g,const void *p,size_t n,uint32_t f,double *out) {
  return ng_cuda_render((NGCuda *)g,p,n,f,out);
}
void ng_device_gpu_destroy(NGDeviceGPU *g) {ng_cuda_destroy((NGCuda *)g);}
