#ifndef NAVIERGRAIN_LIVE_H
#define NAVIERGRAIN_LIVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NGLive NGLive;
/* Single render-owner API, shared by native desktop and browser WASM.
 * All preparation and rendering happens outside the audio callback. */
NGLive *ng_live_create(double sample_rate, int device);
#define NG_LIVE_LOW_HZ 3u
#define NG_LIVE_HIGH_HZ 6u
#define NG_LIVE_SPATIAL_FLOOR_HZ 55.0
#define NG_LIVE_SPATIAL_CEILING_HZ 12000.0
/* Retired slots 3/6 accept only the fixed spatial calibration, for host-array
 * compatibility. Live callers cannot rescale the vertical frequency axis.
 * Core/opcode controls retain their original ratio/depth meanings. */
void ng_live_defaults(double out[24]);
int ng_live_control_valid(unsigned index,double value);
void ng_live_destroy(NGLive *);
int ng_live_control(NGLive *, unsigned index, double value);
int ng_live_render(NGLive *, unsigned frames);
const double *ng_live_audio(NGLive *);
const double *ng_live_view(NGLive *);
const double *ng_live_stats(NGLive *);
/* Header: version, stride, count, capacity, sample rate, simulation time,
 * epoch low/high, current low/high Hz. Records: id low/high, x/y,
 * unwrapped x/y, vx/vy, speed, frequency Hz, age, duration ms,
 * shared world x/y/z, pan. Version 2. */
#define NG_LIVE_PARTICLE_HEADER 10u
#define NG_LIVE_PARTICLE_STRIDE 16u
#define NG_LIVE_PARTICLE_CAPACITY 256u
#define NG_LIVE_PARTICLE_SIZE (NG_LIVE_PARTICLE_HEADER+NG_LIVE_PARTICLE_CAPACITY*NG_LIVE_PARTICLE_STRIDE)
#define NG_LIVE_SPECTRUM_BINS 96u
const double *ng_live_particles(NGLive *);
/* Log-spaced frequency bands from 55 Hz to min(12000, .45*sample_rate), dBFS.
 * Hann-windowed FFT of the most recent 2048 contiguous stereo-mixed frames. */
const double *ng_live_spectrum(NGLive *);
int ng_live_gpu(NGLive *);
void ng_live_cpu(NGLive *);
#ifdef __cplusplus
}
#endif
#endif
