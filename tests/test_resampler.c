#include "naviergrain_resampler.h"
#include "naviergrain_oscillator.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PI 3.14159265358979323846
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/* Deliberately scalar, per-tap reference: no span splitting or lane sums.
 * This checks indexing/arithmetic independently of the optimized reader. */
static double reference_read(const NGResampler *r, const double *s, size_t n,
                             double phase, double increment, int loop,
                             double edge) {
  if (loop) {
    phase = fmod(phase, (double)n);
    if (phase < 0)
      phase += (double)n;
  } else if (phase < 0 || phase > (double)(n - 1))
    return 0;
  double integer = floor(phase);
  double fractional = (phase - integer) * NG_SINC_PHASES;
  unsigned p = (unsigned)fmin(NG_SINC_PHASES - 1u, floor(fractional));
  double location = r->log_range > 0 ? log(fmax(1, increment)) *
                                           (NG_SINC_BANDS - 1u) / r->log_range
                                     : 0;
  location = fmin(NG_SINC_BANDS - 1u, fmax(0, location));
  unsigned band = (unsigned)floor(location);
  double values[2] = {0, 0};
  for (unsigned k = 0; k < 2; ++k) {
    unsigned current = band + k;
    if (current >= NG_SINC_BANDS)
      break;
    unsigned taps = 2u * r->radius[current];
    const double *a = r->coefficients[current] + p * taps;
    for (unsigned t = 0; t < taps; ++t) {
      double index = integer - r->radius[current] + 1 + t;
      if (loop) {
        index = fmod(index, (double)n);
        if (index < 0)
          index += (double)n;
      } else if (index < 0 || index >= (double)n)
        continue;
      values[k] +=
          s[(size_t)index] * (a[t] + (fractional - p) * (a[t + taps] - a[t]));
    }
  }
  double value = values[0] + (location - band) * (values[1] - values[0]);
  if (!loop) {
    edge = fmin(fmax(1, edge), fmax(1, .5 * (double)(n - 1)));
    double ramp = fmin(1, fmax(0, fmin(phase, (double)(n - 1) - phase) / edge));
    value *= .5 - .5 * cos(PI * ramp);
  }
  return value;
}

static void scalar_agreement(void) {
  const double maxima[] = {.5, 1, 4, 8, 64};
  const size_t lengths[] = {1, 2, 3, 5, 31, 64, 65, 127, 257, 997, 4096};
  uint32_t rng = 12345;
  double worst = 0;
  unsigned checks = 0;
  for (unsigned m = 0; m < sizeof(maxima) / sizeof(*maxima); ++m) {
    double *storage = malloc(ng_resampler_doubles(maxima[m]) * sizeof(double));
    CHECK(storage);
    NGResampler r;
    ng_resampler_init(&r, storage, maxima[m]);
    for (unsigned j = 0; j < sizeof(lengths) / sizeof(*lengths); ++j) {
      size_t n = lengths[j];
      /* Exact-sized allocation exposes an overread to ASan, even for n=1. */
      double *source = malloc(n * sizeof(double));
      CHECK(source);
      for (size_t i = 0; i < n; ++i) {
        rng = rng * UINT32_C(1664525) + UINT32_C(1013904223);
        source[i] = (double)rng / UINT32_MAX * 2 - 1;
      }
      for (unsigned b = 0; b < NG_SINC_BANDS; ++b) {
        double increment = exp(r.log_range * fmin(32, b + .37) / 32);
        /* Include every band, partial SIMD tails, both edges, repeated short
         * wraps and non-power-of-two sources. No guard sample is allocated. */
        const double phases[] = {-2.7 * (double)n,
                                 -1e-8,
                                 0,
                                 .371,
                                 .37 * (double)n,
                                 (double)n - 1.001,
                                 (double)n - 1,
                                 (double)n - 1e-8,
                                 (double)n,
                                 3.17 * (double)n};
        for (unsigned q = 0; q < sizeof(phases) / sizeof(*phases); ++q) {
          for (int loop = 0; loop <= 1; ++loop) {
            double actual = ng_read_bandlimited(&r, source, n, phases[q],
                                                increment, loop, 96);
            double expected =
                reference_read(&r, source, n, phases[q], increment, loop, 96);
            CHECK(isfinite(actual));
            double error = fabs(actual - expected);
            worst = fmax(worst, error);
            CHECK(error < 5e-13);
            ++checks;
          }
        }
      }
      free(source);
    }
    free(storage);
  }
  printf("resampler: %u scalar comparisons, maximum error %.9g\n", checks,
         worst);
}

static void boundaries(NGResampler *r) {
  double s[] = {.7, -.2, .1, .8, -.3};
  for (size_t n = 1; n <= 5; ++n) {
    for (int i = -40; i < 100; ++i) {
      double phase = i * .13;
      double value = ng_read_bandlimited(r, s, n, phase, 3.17, 1, 10);
      CHECK(isfinite(value));
      CHECK(fabs(value - ng_read_bandlimited(r, s, n, phase + 7 * (double)n,
                                             3.17, 1, 10)) < 1e-12);
      if (n == 1)
        CHECK(fabs(value - .7) < 1e-12);
      CHECK(isfinite(ng_read_bandlimited(r, s, n, phase, 3.17, 0, 10)));
    }
    CHECK(ng_read_bandlimited(r, s, n, 0, 1, 0, 10) == 0);
    CHECK(ng_read_bandlimited(r, s, n, (double)n - 1, 1, 0, 10) == 0);
    CHECK(ng_read_bandlimited(r, s, n, (double)n, 1, 0, 10) == 0);
    CHECK(ng_read_bandlimited(r, s, n, -1, 1, 0, 10) == 0);
  }
  CHECK(ng_read_bandlimited(r, s, 0, 0, 1, 1, 10) == 0);
  CHECK(ng_read_bandlimited(r, s, 5, NAN, 1, 1, 10) == 0);
  CHECK(ng_read_bandlimited(r, s, 5, 0, INFINITY, 1, 10) == 0);
  CHECK(ng_read_bandlimited(r, s, 5, 0, 0, 1, 10) == 0);
}

static void spectra(NGResampler *r) {
  enum { N = 4096, FRAMES = 1024 };
  double source[N];
  const double increments[] = {.25, .731, 1, 1.01, 1.5, 2, 2.73, 4, 6.13, 8};
  double worst_pass = 0, worst_stop = 0, worst_alias_ratio = 0;
  for (unsigned j = 0; j < sizeof(increments) / sizeof(*increments); ++j) {
    double increment = increments[j];
    for (unsigned test = 0; test < 12; ++test) {
      int stop = test >= 4;
      if (stop && increment <= 1)
        continue;
      double nyquist = .5 / fmax(1, increment);
      double frequency = stop ? nyquist + (.499 - nyquist) * (test - 4) / 7
                              : nyquist * (.08 + .16 * test);
      /* Coherent input has no loop seam; stop frequencies round UP. */
      double bin = stop ? ceil(frequency * N) : round(frequency * N);
      frequency = bin / N;
      for (unsigned k = 0; k < N; ++k)
        source[k] = sin(2 * PI * frequency * k);
      double power = 0, cubic = 0, error = 0;
      for (unsigned k = 0; k < FRAMES; ++k) {
        double phase = .371 + k * increment;
        double actual =
            ng_read_bandlimited(r, source, N, phase, increment, 1, 1);
        double preview = ng_read_preview(source, N, phase, 1, 1);
        double expected = sin(2 * PI * frequency * phase);
        power += actual * actual;
        cubic += preview * preview;
        error = fmax(error, fabs(actual - expected));
      }
      if (stop) {
        worst_stop = fmax(worst_stop, sqrt(2 * power / FRAMES));
        if (cubic > .01)
          worst_alias_ratio = fmax(worst_alias_ratio, sqrt(power / cubic));
      } else
        worst_pass = fmax(worst_pass, error);
    }
  }
  printf("resampler: passband max error %.9g, stopband %.2f dBFS, "
         "worst suppression vs cubic %.2f dB\n",
         worst_pass, 20 * log10(worst_stop), -20 * log10(worst_alias_ratio));
  CHECK(worst_pass < .001);
  CHECK(worst_stop < .001);
  CHECK(worst_alias_ratio < .002);
}

static void continuity(NGResampler *r) {
  double s[997];
  for (unsigned i = 0; i < 997; ++i)
    s[i] = sin(2 * PI * 123 * i / 997);
  double worst = 0;
  /* No filter-switch step at any cutoff boundary, unity, or phase wrap. */
  for (unsigned b = 0; b < NG_SINC_BANDS; ++b) {
    double increment = exp(r->log_range * b / (NG_SINC_BANDS - 1u));
    for (unsigned p = 0; p <= NG_SINC_PHASES; ++p) {
      double phase = 996 + (double)p / NG_SINC_PHASES;
      double before = ng_read_bandlimited(r, s, 997, phase - 1e-9,
                                          increment * (1 - 1e-9), 1, 1);
      double after = ng_read_bandlimited(r, s, 997, phase + 1e-9,
                                         increment * (1 + 1e-9), 1, 1);
      worst = fmax(worst, fabs(after - before));
    }
  }
  printf("resampler: worst band/phase-boundary difference %.9g\n", worst);
  CHECK(worst < 1e-7);
}
int main(void) {
  scalar_agreement();
  CHECK(!ng_resampler_doubles(NAN));
  CHECK(!ng_resampler_doubles(0));
  size_t count = ng_resampler_doubles(8);
  double *storage = malloc(count * sizeof(double));
  CHECK(storage);
  NGResampler r;
  ng_resampler_init(&r, storage, 8);
  boundaries(&r);
  spectra(&r);
  continuity(&r);
  printf("resampler: %.3f MiB coefficients (maximum increment 8)\n",
         (double)count * sizeof(double) / (1024 * 1024));
  for(unsigned i=0;i<=10000;++i){
    double x=-2+4.0*i/10000;
    CHECK(fabs(ng_osc_pitch(x)-exp2(x))<1e-11);
    double hz_log=5+9.0*i/10000;
    CHECK(fabs(ng_osc_pitch(hz_log)/exp2(hz_log)-1)<1e-11);
    CHECK(fabs(ng_osc_sin(x*10)-sin(x*10))<2e-14);
    CHECK(fabs(ng_osc_cos(x*10)-cos(x*10))<2e-14);
  }
  r.cosine=1;
  for(unsigned i=0;i<997;++i) {
    double phase=(double)i*.317;
    CHECK(fabs(ng_read_bandlimited(&r,NULL,240,phase,1.1,1,1)-cos(2*PI*phase/240))<1e-12);
  }
  CHECK(ng_read_bandlimited(&r,NULL,240,17,120,1,1)==0);
  free(storage);
  return 0;
}
