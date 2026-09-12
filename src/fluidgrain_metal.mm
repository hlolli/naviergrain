#include "fluidgrain_device_gpu.h"
#import <Metal/Metal.h>
#include <cmath>
#include <cstring>
#include <new>
#include "fluidgrain_metal_source.h"

struct FGDeviceGPU {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> grains, reduce;
  id<MTLBuffer> resources, packet, partial, output;
  uint32_t capacity, max_frames;
  size_t packet_capacity;
};
extern "C" void fg_device_gpu_destroy(FGDeviceGPU *g) { delete g; }
extern "C" FGDeviceGPU *fg_device_gpu_create(const void *resources,size_t bytes,
    uint32_t capacity,uint32_t frames,int device) {
  @autoreleasepool {
    if(!resources||bytes<328||bytes%4||!capacity||capacity>4096||!frames||frames>512||device<0)return nullptr;
    uint32_t h[4];std::memcpy(h,resources,sizeof(h));
    if(h[0]!=0x52474746u||(h[1]!=1&&h[1]!=2)||!h[2])return nullptr;
    NSArray<id<MTLDevice>> *devices=MTLCopyAllDevices();
    if((NSUInteger)device>=devices.count)return nullptr;
    auto *g=new(std::nothrow) FGDeviceGPU{};
    if(!g)return nullptr;
    g->device=devices[(NSUInteger)device];g->capacity=capacity;g->max_frames=frames;
    g->packet_capacity=((64+4*(size_t)frames+63)&~(size_t)63)+(size_t)capacity*frames*64+192;
    MTLCompileOptions *options=[MTLCompileOptions new];
    options.fastMathEnabled=NO;
    NSError *error=nil;
    id<MTLLibrary> library=[g->device newLibraryWithSource:@(fg_metal_source) options:options error:&error];
    if(!library){fprintf(stderr,"naviergrain Metal: %s\n",error.localizedDescription.UTF8String);delete g;return nullptr;}
    g->grains=[g->device newComputePipelineStateWithFunction:[library newFunctionWithName:@"grains"] error:&error];
    g->reduce=[g->device newComputePipelineStateWithFunction:[library newFunctionWithName:@"reduce"] error:&error];
    g->queue=[g->device newCommandQueue];
    g->resources=[g->device newBufferWithBytes:resources length:bytes options:MTLResourceStorageModeShared];
    g->packet=[g->device newBufferWithLength:g->packet_capacity options:MTLResourceStorageModeShared];
    g->partial=[g->device newBufferWithLength:(size_t)capacity*frames*8 options:MTLResourceStorageModeShared];
    g->output=[g->device newBufferWithLength:(size_t)frames*8 options:MTLResourceStorageModeShared];
    if(!g->grains||!g->reduce||g->grains.maxTotalThreadsPerThreadgroup<64||
       g->reduce.maxTotalThreadsPerThreadgroup<64||!g->queue||!g->resources||!g->packet||!g->partial||!g->output){delete g;return nullptr;}
    return g;
  }
}
extern "C" int fg_device_gpu_render(FGDeviceGPU *g,const void *packet,size_t bytes,
    uint32_t frames,double *stereo) {
  @autoreleasepool {
    if(!g||!packet||!stereo||!frames||frames>g->max_frames||bytes<192||bytes>g->packet_capacity)return 0;
    uint32_t h[16];std::memcpy(h,packet,sizeof(h));size_t offset=h[1]==2?h[10]:192;
    if(h[0]!=0x50474746u||(h[1]!=1&&h[1]!=2)||h[2]!=frames||h[4]!=g->capacity||
       h[5]!=bytes||offset<64+4*(size_t)frames||offset%64||
       offset+(size_t)h[3]*64!=bytes||h[3]>(size_t)g->capacity*frames)return 0;
    std::memcpy(g->packet.contents,packet,bytes);
    std::memset(g->partial.contents,0,(size_t)frames*g->capacity*8);
    id<MTLCommandBuffer> command=[g->queue commandBuffer];
    if(!command)return 0;
    if(h[3]) {
      id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
      if(!encoder)return 0;
      [encoder setComputePipelineState:g->grains];
      [encoder setBuffer:g->resources offset:0 atIndex:0];
      [encoder setBuffer:g->packet offset:0 atIndex:1];
      [encoder setBuffer:g->partial offset:0 atIndex:2];
      [encoder dispatchThreadgroups:MTLSizeMake((h[3]+63)/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
      [encoder endEncoding];
    }
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    if(!encoder)return 0;
    [encoder setComputePipelineState:g->reduce];
    [encoder setBuffer:g->packet offset:0 atIndex:0];
    [encoder setBuffer:g->partial offset:0 atIndex:1];
    [encoder setBuffer:g->output offset:0 atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(frames,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [encoder endEncoding];[command commit];
    // This blocks only the render worker. The audio callback reads queued PCM.
    [command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)return 0;
    const float *out=(const float *)g->output.contents;
    for(uint32_t i=0;i<2*frames;++i)if(!std::isfinite(out[i]))return 0;
    for(uint32_t i=0;i<2*frames;++i)stereo[i]=out[i];
    return 1;
  }
}
