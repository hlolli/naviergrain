#ifndef NAVIERGRAIN_PACK_H
#define NAVIERGRAIN_PACK_H
#include "naviergrain_grain_plan.h"
/* Private little-endian GPU layout v1. Native pointers never leave the owner.
 * 64-byte header, 32 f32 frame gains, then 64-byte adaptive segments.
 * v2 (over 32 frames): header plus frames f32 gains, padded to 64 bytes;
 * header word 10 gives the segment byte offset. Segments still cover <=32 frames.
 * Header words: magic, version, frames, segments, capacity, bytes, clock lo/hi;
 * then original record count and peak live count; remaining words zero. A segment is slot/first/count/i32 phase base
 * followed by four quadratic polynomials (phase residual, increment, pan, env).
 * Coefficients use normalized t=(frame-first)/(count-1). */
#define NG_PACK_HEADER 192u
#define NG_PACK_SEGMENT 64u
#define NG_PACK_MAGIC 0x5047474eu
/* GPU immutable resource layout: 16 header words, 33 (radius,offset) pairs,
 * f32 source then coefficients. All offsets count 32-bit words.
 * Resource v2 uses word 6 = 1 for a unit analytic cosine cycle of word 2
 * samples. Resource v1 keeps sample convolution. Packet versions are separate. */
size_t ng_pack_resources_size(const NGGrainResources *);
int ng_pack_resources(const NGGrainResources *, void *, size_t);
size_t ng_pack_size(uint32_t capacity);
size_t ng_pack_size_frames(uint32_t capacity, uint32_t frames);
size_t ng_pack_scratch_size(uint32_t capacity);
size_t ng_pack_scratch_size_frames(uint32_t capacity, uint32_t frames);
/* Writes only caller-owned storage. On failure the packet must not be used;
 * the sealed exact plan remains valid for native C fallback. */
size_t ng_pack(const NGGrainPlan *, void *packet, size_t bytes,
               void *scratch, size_t scratch_bytes);
/* Test/reference decoder of trusted locally packed packets. */
int ng_pack_render(const NGGrainPlan *, const void *packet, size_t bytes,
                    double *stereo, size_t samples);
#endif
