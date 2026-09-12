#ifndef FLUIDGRAIN_LIVE_H
#define FLUIDGRAIN_LIVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct FGLive FGLive;
/* Single render-owner API, shared by native desktop and browser WASM.
 * All preparation and rendering happens outside the audio callback. */
FGLive *fg_live_create(double sample_rate, int device);
#define FG_LIVE_LOW_HZ 3u
#define FG_LIVE_HIGH_HZ 6u
#define FG_LIVE_SPATIAL_FLOOR_HZ 55.0
#define FG_LIVE_SPATIAL_CEILING_HZ 12000.0
/* Retired slots 3/6 accept only the fixed spatial calibration, for host-array
 * compatibility. Live callers cannot rescale the vertical frequency axis.
 * Core/opcode controls retain their original ratio/depth meanings. */
void fg_live_defaults(double out[24]);
int fg_live_control_valid(unsigned index,double value);
void fg_live_destroy(FGLive *);
int fg_live_control(FGLive *, unsigned index, double value);
int fg_live_render(FGLive *, unsigned frames);
const double *fg_live_audio(FGLive *);
const double *fg_live_view(FGLive *);
const double *fg_live_stats(FGLive *);
/* Header: version, stride, count, capacity, sample rate, simulation time,
 * epoch low/high, current low/high Hz. Records: id low/high, x/y,
 * unwrapped x/y, vx/vy, speed, frequency Hz, age, duration ms,
 * shared world x/y/z, pan. Version 2. */
#define FG_LIVE_PARTICLE_HEADER 10u
#define FG_LIVE_PARTICLE_STRIDE 16u
#define FG_LIVE_PARTICLE_CAPACITY 256u
#define FG_LIVE_PARTICLE_SIZE (FG_LIVE_PARTICLE_HEADER+FG_LIVE_PARTICLE_CAPACITY*FG_LIVE_PARTICLE_STRIDE)
#define FG_LIVE_SPECTRUM_BINS 96u
const double *fg_live_particles(FGLive *);
/* Log-spaced frequency bands from 55 Hz to min(12000, .45*sample_rate), dBFS.
 * Hann-windowed FFT of the most recent 2048 contiguous stereo-mixed frames. */
const double *fg_live_spectrum(FGLive *);
int fg_live_gpu(FGLive *);
void fg_live_cpu(FGLive *);
#ifdef __cplusplus
}
#endif
#endif
