#ifndef NAVIERGRAIN_RESAMPLER_H
#define NAVIERGRAIN_RESAMPLER_H
#include <stddef.h>

#define NG_SINC_BANDS 33u
#define NG_SINC_PHASES 128u
#define NG_SINC_MAX_RADIUS 512u

typedef struct {
  double *coefficients[NG_SINC_BANDS];
  unsigned radius[NG_SINC_BANDS];
  double log_range;
  int cosine; /* Explicit analytic single-cycle source, set before playback. */
} NGResampler;

/* The arena and all coefficients are instance-owned, prepared before playback.
 * max_increment is the largest allowed pitch * source_sr / engine_sr. */
size_t ng_resampler_doubles(double max_increment);
void ng_resampler_init(NGResampler *reader, double *storage,
                       double max_increment);
double ng_read_bandlimited(const NGResampler *reader, const double *source,
                           size_t length, double phase, double increment,
                           int loop, double edge_samples);
/* Cubic is retained only as a comparison fixture, never the opcode reader. */
double ng_read_preview(const double *source, size_t length, double phase,
                       int loop, double edge_samples);
#endif
