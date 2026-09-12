#ifndef FLUIDGRAIN_RESAMPLER_H
#define FLUIDGRAIN_RESAMPLER_H
#include <stddef.h>

#define FG_SINC_BANDS 33u
#define FG_SINC_PHASES 128u
#define FG_SINC_MAX_RADIUS 512u

typedef struct {
  double *coefficients[FG_SINC_BANDS];
  unsigned radius[FG_SINC_BANDS];
  double log_range;
  int cosine; /* Explicit analytic single-cycle source, set before playback. */
} FGResampler;

/* The arena and all coefficients are instance-owned, prepared before playback.
 * max_increment is the largest allowed pitch * source_sr / engine_sr. */
size_t fg_resampler_doubles(double max_increment);
void fg_resampler_init(FGResampler *reader, double *storage,
                       double max_increment);
double fg_read_bandlimited(const FGResampler *reader, const double *source,
                           size_t length, double phase, double increment,
                           int loop, double edge_samples);
/* Cubic is retained only as a comparison fixture, never the opcode reader. */
double fg_read_preview(const double *source, size_t length, double phase,
                       int loop, double edge_samples);
#endif
