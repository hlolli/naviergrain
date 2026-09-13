#include "naviergrain_resampler.h"
#include "naviergrain_oscillator.h"
#include <float.h>
#include <math.h>

#define NG_PI 3.14159265358979323846

static double band_ratio(double log_range, unsigned band) {
  /* One-band lookahead keeps BOTH interpolated filters below the required
   * source cutoff. The final band is duplicated to keep the endpoint smooth. */
  return fmin(DBL_MAX, exp(log_range * (fmin(band + 1.0, NG_SINC_BANDS - 1.0) /
                                        (NG_SINC_BANDS - 1.0))));
}
static unsigned radius_for(double ratio) {
  return (unsigned)fmin(NG_SINC_MAX_RADIUS, ceil(32 * ratio));
}
size_t ng_resampler_doubles(double max_increment) {
  if (!isfinite(max_increment) || max_increment <= 0)
    return 0;
  double range = log(fmax(1, max_increment));
  size_t count = 0;
  for (unsigned b = 0; b < NG_SINC_BANDS; ++b)
    count += 2u * radius_for(band_ratio(range, b)) * (NG_SINC_PHASES + 1u);
  return count;
}
void ng_resampler_init(NGResampler *r, double *storage, double max_increment) {
  r->cosine = 0;
  r->log_range = log(fmax(1, max_increment));
  for (unsigned b = 0; b < NG_SINC_BANDS; ++b) {
    double ratio = band_ratio(r->log_range, b), cutoff = .45 / ratio;
    unsigned radius = radius_for(ratio), taps = 2u * radius;
    r->radius[b] = radius;
    r->coefficients[b] = storage;
    for (unsigned p = 0; p <= NG_SINC_PHASES; ++p) {
      double sum = 0, phase = (double)p / NG_SINC_PHASES;
      for (unsigned t = 0; t < taps; ++t) {
        double x = (double)t - radius + 1 - phase;
        double angle = 2 * NG_PI * cutoff * x;
        double sinc = fabs(angle) < 1e-12 ? 1 : sin(angle) / angle;
        double window = .42 + .5 * cos(NG_PI * x / radius) +
                        .08 * cos(2 * NG_PI * x / radius);
        storage[t] = 2 * cutoff * sinc * window;
        sum += storage[t];
      }
      for (unsigned t = 0; t < taps; ++t)
        storage[t] /= sum;
      storage += taps;
    }
  }
}
static double edge_gain(size_t n, double phase, double edge) {
  edge = fmin(fmax(1, edge), fmax(1, .5 * (double)(n - 1)));
  double distance = fmin(phase, (double)(n - 1) - phase);
  if (distance >= edge)
    return 1;
  double ramp = fmin(1, fmax(0, distance / edge));
  return .5 - .5 * cos(NG_PI * ramp);
}
/* Interpolate the two convolution results, not every coefficient. Linearity
 * saves per-tap subtraction and multiplication without changing the filters.
 * Four lanes per phase allow vector arithmetic without fast-math or intrinsics.
 * Only floating-point rounding/summation order changes.
 * Source/coefficients are contiguous and valid for count samples. */
static double convolve_span(const double *s, const double *a, const double *b,
                            unsigned count, double mix) {
  double first[4] = {0, 0, 0, 0}, next[4] = {0, 0, 0, 0};
  unsigned t = 0;
  for (; t + 4u <= count; t += 4u) {
    first[0] += s[t] * a[t];
    first[1] += s[t + 1] * a[t + 1];
    first[2] += s[t + 2] * a[t + 2];
    first[3] += s[t + 3] * a[t + 3];
    next[0] += s[t] * b[t];
    next[1] += s[t + 1] * b[t + 1];
    next[2] += s[t + 2] * b[t + 2];
    next[3] += s[t + 3] * b[t + 3];
  }
  double left = (first[0] + first[1]) + (first[2] + first[3]);
  double right = (next[0] + next[1]) + (next[2] + next[3]);
  for (; t < count; ++t) {
    left += s[t] * a[t];
    right += s[t] * b[t];
  }
  return left + mix * (right - left);
}
/* Adjacent cutoff filters share their central source samples. This interior
 * path reads each central sample once for all four phase/band dot products.
 * Unequal radii leave short outer spans in the wider filter. Boundary reads
 * still use convolve(), including repeated wraps on tiny source tables. */
static double convolve_pair(const NGResampler *r, unsigned band, const double *s,
                            unsigned phase, double mix, double blend) {
  unsigned count = 2u * r->radius[band];
  unsigned wide_count = 2u * r->radius[band + 1u];
  unsigned margin = (wide_count - count) / 2u;
  const double *a = r->coefficients[band] + phase * count;
  const double *b = a + count;
  const double *c = r->coefficients[band + 1u] + phase * wide_count;
  const double *d = c + wide_count;
  double outer = convolve_span(s, c, d, margin, mix);
  s += margin;
  c += margin;
  d += margin;
  double first[4] = {0}, next[4] = {0};
  double wide_first[4] = {0}, wide_next[4] = {0};
  unsigned t = 0;
  for (; t + 4u <= count; t += 4u) {
    double sample0 = s[t];
    first[0] += sample0 * a[t];
    next[0] += sample0 * b[t];
    wide_first[0] += sample0 * c[t];
    wide_next[0] += sample0 * d[t];
    double sample1 = s[t + 1];
    first[1] += sample1 * a[t + 1];
    next[1] += sample1 * b[t + 1];
    wide_first[1] += sample1 * c[t + 1];
    wide_next[1] += sample1 * d[t + 1];
    double sample2 = s[t + 2];
    first[2] += sample2 * a[t + 2];
    next[2] += sample2 * b[t + 2];
    wide_first[2] += sample2 * c[t + 2];
    wide_next[2] += sample2 * d[t + 2];
    double sample3 = s[t + 3];
    first[3] += sample3 * a[t + 3];
    next[3] += sample3 * b[t + 3];
    wide_first[3] += sample3 * c[t + 3];
    wide_next[3] += sample3 * d[t + 3];
  }
  double left = (first[0] + first[1]) + (first[2] + first[3]);
  double right = (next[0] + next[1]) + (next[2] + next[3]);
  double wide_left = (wide_first[0] + wide_first[1]) +
                     (wide_first[2] + wide_first[3]);
  double wide_right = (wide_next[0] + wide_next[1]) +
                      (wide_next[2] + wide_next[3]);
  for (; t < count; ++t) {
    double sample = s[t];
    left += sample * a[t];
    right += sample * b[t];
    wide_left += sample * c[t];
    wide_right += sample * d[t];
  }
  double narrow = left + mix * (right - left);
  double wide = outer + (wide_left + mix * (wide_right - wide_left));
  wide += convolve_span(s + count, c + count, d + count, margin, mix);
  return narrow + blend * (wide - narrow);
}
static double convolve(const NGResampler *r, unsigned band, const double *s,
                       size_t n, double integer, unsigned phase, double mix,
                       int loop) {
  unsigned taps = 2u * r->radius[band];
  const double *a = r->coefficients[band] + phase * taps, *b = a + taps;
  double start = integer - r->radius[band] + 1, result = 0;
  if (loop) {
    if (start < 0 || start >= (double)n)
      start = fmod(start, (double)n);
    if (start < 0)
      start += (double)n;
    size_t index = (size_t)start;
    /* Split at source edges, not inside the dot product. Short tables may
     * wrap repeatedly; each span consumes at least one of the bounded taps. */
    for (unsigned t = 0; t < taps;) {
      unsigned count = taps - t;
      if (n - index < count)
        count = (unsigned)(n - index);
      result += convolve_span(s + index, a + t, b + t, count, mix);
      t += count;
      index = 0;
    }
  } else {
    /* Clip zero-extension once, before forming a source pointer. */
    unsigned skip = start < 0 ? (unsigned)fmin(taps, -start) : 0;
    size_t index = (size_t)fmax(0, start);
    if (skip == taps || index >= n)
      return 0;
    unsigned count = taps - skip;
    if (n - index < count)
      count = (unsigned)(n - index);
    result = convolve_span(s + index, a + skip, b + skip, count, mix);
  }
  return result;
}
double ng_read_bandlimited(const NGResampler *r, const double *s, size_t n,
                           double phase, double increment, int loop,
                           double edge) {
  if (!n || !isfinite(phase) || !isfinite(increment) || increment <= 0)
    return 0;
  if (loop) {
    if (phase < 0 || phase >= (double)n)
      phase = fmod(phase, (double)n);
    if (phase < 0)
      phase += (double)n;
  } else if (phase < 0 || phase > (double)(n - 1))
    return 0;
  if (r->cosine)
    return increment < .5 * (double)n ? ng_osc_cos(2 * NG_PI * phase / (double)n) : 0;
  double integer = floor(phase);
  double fractional = (phase - integer) * NG_SINC_PHASES;
  unsigned p = (unsigned)fmin(NG_SINC_PHASES - 1u, floor(fractional));
  double location = r->log_range > 0 ? log(fmax(1, increment)) *
                                           (NG_SINC_BANDS - 1u) / r->log_range
                                     : 0;
  location = fmin(NG_SINC_BANDS - 1u, fmax(0, location));
  unsigned band = (unsigned)floor(location);
  double blend = location - band;
  if (blend > 0 && band + 1u < NG_SINC_BANDS) {
    unsigned radius = r->radius[band + 1u];
    double start = integer - radius + 1;
    if (start >= 0 && start + 2u * radius <= (double)n) {
      double result = convolve_pair(r, band, s + (size_t)start, p,
                                    fractional - p, blend);
      return loop ? result : result * edge_gain(n, phase, edge);
    }
  }
  double result = convolve(r, band, s, n, integer, p, fractional - p, loop);
  if (blend > 0 && band + 1u < NG_SINC_BANDS) {
    double next =
        convolve(r, band + 1u, s, n, integer, p, fractional - p, loop);
    result += blend * (next - result);
  }
  return loop ? result : result * edge_gain(n, phase, edge);
}
static double tap(const double *s, size_t n, double i, int loop) {
  if (loop) {
    i = fmod(i, (double)n);
    if (i < 0)
      i += (double)n;
  } else if (i < 0 || i >= (double)n)
    return 0;
  return s[(size_t)i];
}
double ng_read_preview(const double *s, size_t n, double phase, int loop,
                       double edge) {
  if (!n || !isfinite(phase))
    return 0;
  if (loop) {
    phase = fmod(phase, (double)n);
    if (phase < 0)
      phase += (double)n;
  } else if (phase < 0 || phase > (double)(n - 1))
    return 0;
  double i = floor(phase), t = phase - i;
  double a = tap(s, n, i - 1, loop), b = tap(s, n, i, loop);
  double c = tap(s, n, i + 1, loop), d = tap(s, n, i + 2, loop);
  double result =
      b +
      .5 * t *
          (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
  return loop ? result : result * edge_gain(n, phase, edge);
}
