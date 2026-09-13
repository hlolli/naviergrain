/* Deterministic boundary/recovery qualification, not a performance benchmark.
 */
#include "naviergrain_core.h"
#include "naviergrain_particles.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.28318530717958647692
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "extrema line %d: %s\n", __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static unsigned cases;
static double peak;
static uint64_t interventions;

static NGEngine *make(const double *config, double *control, double sr,
                      double source_sr, unsigned fixture) {
  NGConfig parsed;
  CHECK(!ng_config_parse(&parsed, config, NG_CONFIG_COUNT));
  size_t bytes = ng_memory_size(&parsed, 997, source_sr, sr);
  CHECK(bytes);
  void *memory = malloc(bytes);
  CHECK(memory);
  NGEngine *e = ng_init(memory, bytes, &parsed, 997, source_sr, sr);
  CHECK(e);
  uint32_t noise = 7;
  for (unsigned i = 0; i < 997; ++i) {
    double sample = 0;
    if (fixture == 1)
      sample = i % 97 == 0 ? .25 : 0; /* transient train */
    else if (fixture == 2)
      sample = .25 * sin(TAU * i / 97);
    else if (fixture == 3) {
      noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
      sample = .25 * (2 * (double)noise / UINT32_MAX - 1);
    } else if (fixture == 4)
      sample = .125 * sin(TAU * i / 97) + .075 * sin(3 * TAU * i / 97) +
               .05 * sin(7 * TAU * i / 97);
    ng_source(e)[i] = sample;
  }
  ng_controls(e, control);
  return e;
}
static void check_stats(NGEngine *e, const double *config) {
  double stats[NG_STAT_COUNT];
  ng_stats(e, stats);
  for (unsigned i = 0; i < NG_STAT_COUNT; ++i)
    CHECK(isfinite(stats[i]) && stats[i] >= 0);
  CHECK(stats[NG_STAT_LIVE_GRAINS] <= config[NG_CONFIG_MAX_GRAINS]);
  CHECK(stats[NG_STAT_EMITTER_COUNT] == config[NG_CONFIG_EMITTER_COUNT]);
  CHECK((unsigned)stats[NG_STAT_STATUS] & NG_STATUS_READY);
  CHECK(stats[NG_STAT_BACKEND] == 0 && stats[NG_STAT_SOURCE_LENGTH] == 997);
}
static double render(NGEngine *e, const double *config, unsigned frames) {
  double segment_peak = 0;
  for (unsigned frame = 0; frame < frames; ++frame) {
    double l, r;
    ng_sample(e, &l, &r);
    CHECK(isfinite(l) && isfinite(r));
    segment_peak = fmax(segment_peak, fmax(fabs(l), fabs(r)));
    if (frame % 37 == 0)
      check_stats(e, config);
  }
  for (unsigned slot = 0; slot < (unsigned)config[NG_CONFIG_MAX_GRAINS];
       ++slot) {
    NGGrainState p;
    if (!ng_grain_state(e, slot, &p))
      continue;
    CHECK(isfinite(p.x) && p.x >= 0 && p.x < 1);
    CHECK(isfinite(p.y) && p.y >= 0 && p.y < 1);
    CHECK(isfinite(p.path_x) && isfinite(p.path_y));
    CHECK(isfinite(p.velocity_x) && isfinite(p.velocity_y));
    CHECK(isfinite(p.simulation_time) && p.simulation_time >= 0);
    CHECK(p.age < p.length);
  }
  check_stats(e, config);
  peak = fmax(peak, segment_peak);
  return segment_peak;
}
static void configuration_boundaries(void) {
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  NGConfig parsed;
  for (unsigned i = 0; i < NG_CONFIG_COUNT; ++i) {
    NGParameter p = ng_config_parameters[i];
    double invalid[] = {NAN, INFINITY, -INFINITY,
                        nextafter(p.minimum, -INFINITY),
                        nextafter(p.maximum, INFINITY)};
    for (unsigned j = 0; j < 5; ++j) {
      ng_defaults(config, control);
      config[i] = invalid[j];
      CHECK(ng_config_parse(&parsed, config, NG_CONFIG_COUNT));
      ++cases;
    }
    for (unsigned end = 0; end < 2; ++end) {
      ng_defaults(config, control);
      config[i] = end ? p.maximum : p.minimum;
      const char *error = ng_config_parse(&parsed, config, NG_CONFIG_COUNT);
      CHECK((error != NULL) == (i == NG_CONFIG_BACKEND && end));
      ++cases;
    }
    if (p.discrete) {
      ng_defaults(config, control);
      config[i] = p.minimum + .5;
      CHECK(ng_config_parse(&parsed, config, NG_CONFIG_COUNT));
      ++cases;
    }
  }
  ng_defaults(config, control);
  CHECK(!ng_config_parse(&parsed, config, NG_CONFIG_COUNT));
  double invalid_rates[] = {NAN, INFINITY, -INFINITY, 0, -1};
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(!ng_memory_size(&parsed, 997, invalid_rates[i], 48000));
    CHECK(!ng_memory_size(&parsed, 997, 48000, invalid_rates[i]));
  }
  CHECK(!ng_memory_size(&parsed, 997, 48000, 384001));
  CHECK(!ng_memory_size(&parsed, SIZE_MAX, 48000, 48000));
}
static void control_boundaries(void) {
  double config[NG_CONFIG_COUNT], bad[NG_CONTROL_COUNT],
      expected[NG_CONTROL_COUNT];
  ng_defaults(config, bad);
  config[NG_CONFIG_GRID_SIZE] = 16;
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  config[NG_CONFIG_EMITTER_COUNT] = 16;
  config[NG_CONFIG_MAX_GRAINS] = 32;
  bad[NG_CONTROL_GRAIN_RATE] = 400;
  memcpy(expected, bad, sizeof(expected));
  NGEngine *a = make(config, bad, 48000, 24000, 4);
  NGEngine *b = make(config, expected, 48000, 24000, 4);
  uint64_t events = 0;
  double energy = 0;
  for (unsigned i = 0; i < NG_CONTROL_COUNT; ++i) {
    NGParameter p = ng_control_parameters[i];
    double values[] = {NAN,     INFINITY,  -INFINITY, -DBL_MAX,
                       DBL_MAX, p.minimum, p.maximum, p.minimum + .5};
    for (unsigned j = 0; j < 8; ++j) {
      memcpy(bad, expected, sizeof(bad));
      double x = values[j];
      bad[i] = x;
      if (!isfinite(x) ||
          (p.discrete && (x != floor(x) || x < p.minimum || x > p.maximum))) {
        ++events; /* hold last valid target */
      } else {
        expected[i] = fmax(p.minimum, fmin(p.maximum, x));
        events += expected[i] != x;
      }
      ng_controls(a, bad);
      ng_controls(b, expected);
      CHECK(ng_counters(a).control_events == events);
      CHECK(ng_counters(b).control_events == 0);
      for (unsigned frame = 0; frame < 97; ++frame) {
        double l, r, bl, br;
        ng_sample(a, &l, &r);
        ng_sample(b, &bl, &br);
        CHECK(isfinite(l) && isfinite(r) && l == bl && r == br);
        energy += l * l + r * r;
      }
      check_stats(a, config);
      ++cases;
    }
  }
  CHECK(energy > 0);
  CHECK(ng_counters(a).births == ng_counters(b).births);
  free(a);
  free(b);
}
static void audio_endpoints(void) {
  const double rates[] = {44100, 48000, 96000};
  const double ratios[] = {.5, 1, 2};
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  /* Each continuous control, independently at each endpoint, on all five
   * fixtures. Rate/source ratio and loop mode rotate deterministically. */
  for (unsigned fixture = 0; fixture < 5; ++fixture)
    for (unsigned i = 0; i < NG_CONTROL_FREEZE; ++i)
      for (unsigned end = 0; end < 2; ++end) {
        ng_defaults(config, control);
        config[NG_CONFIG_GRID_SIZE] = 16;
        config[NG_CONFIG_EMITTER_COUNT] = 16;
        config[NG_CONFIG_MAX_GRAINS] = 64;
        config[NG_CONFIG_SOURCE_LOOP] = (fixture + i + end) % 2;
        control[i] = end ? ng_control_parameters[i].maximum
                         : ng_control_parameters[i].minimum;
        double sr = rates[(fixture + i) % 3];
        NGEngine *e =
            make(config, control, sr, sr * ratios[(i + end) % 3], fixture);
        double segment_peak = render(e, config, (unsigned)(sr * .08));
        if (fixture == 0)
          CHECK(segment_peak == 0);
        CHECK(ng_counters(e).numeric_interventions == 0);
        free(e);
        ++cases;
      }
}
static void combined_extrema_and_reset(void) {
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  for (unsigned grid = 16; grid <= 64; grid *= 2)
    for (unsigned scheduler = 0; scheduler < 2; ++scheduler) {
      ng_defaults(config, control);
      config[NG_CONFIG_GRID_SIZE] = grid;
      config[NG_CONFIG_SOURCE_LOOP] = 1;
      config[NG_CONFIG_EMITTER_COUNT] = grid == 64 ? 4096 : 1;
      config[NG_CONFIG_MAX_GRAINS] = grid == 64 ? 4096 : 1;
      config[NG_CONFIG_FLUID_HZ] = 30;
      config[NG_CONFIG_PARTICLE_HZ] = 60;
      config[NG_CONFIG_PRESSURE_ITERATIONS] = 16;
      config[NG_CONFIG_VISCOSITY_ITERATIONS] = 4;
      for (unsigned i = 0; i < NG_CONTROL_FREEZE; ++i)
        control[i] = ng_control_parameters[i].maximum;
      control[NG_CONTROL_VISCOSITY] = 0;
      control[NG_CONTROL_EDDY_SIZE] = .05;
      control[NG_CONTROL_STRAIN_TO_DURATION] = 0;
      control[NG_CONTROL_SCHEDULER] = scheduler;
      NGEngine *e = make(config, control, 48000, 24000, 3);
      render(e, config, 4800);
      uint64_t births = ng_counters(e).births;
      CHECK(births > 0);
      control[NG_CONTROL_FREEZE] = 1;
      ng_controls(e, control);
      uint64_t proposals = births + ng_counters(e).voice_drops;
      render(e, config, 1000);
      CHECK(ng_counters(e).births + ng_counters(e).voice_drops > proposals);
      control[NG_CONTROL_RESET] = 1;
      ng_controls(e, control);
      /* Derived ceiling: both 10 ms fades plus 64 clear records/sample.
       * At N=64 this includes every field cell and both maximum pools. */
      unsigned ceiling = 2 * 480 +
                         (unsigned)ceil((config[NG_CONFIG_MAX_GRAINS] +
                                         config[NG_CONFIG_EMITTER_COUNT] +
                                         (double)ng_field_doubles(grid)) /
                                        64) +
                         2;
      render(e, config, ceiling);
      CHECK(ng_counters(e).epoch == 1);
      render(e, config, ceiling);
      CHECK(ng_counters(e).epoch == 1); /* held reset is edge-triggered */
      control[NG_CONTROL_RESET] = control[NG_CONTROL_FREEZE] = 0;
      control[NG_CONTROL_GRAIN_RATE] = 0;
      ng_controls(e, control);
      births = ng_counters(e).births;
      render(e, config, 24001);
      double stats[NG_STAT_COUNT];
      ng_stats(e, stats);
      CHECK(ng_counters(e).births == births);
      CHECK(stats[NG_STAT_LIVE_GRAINS] == 0);
      CHECK(ng_counters(e).control_events == 0);
      interventions += ng_counters(e).numeric_interventions;
      free(e);
      ++cases;
    }
  CHECK(interventions > 0); /* extrema must report actual safety limiting */
}
static void output_fault_recovery(void) {
  const double faults[] = {NAN, INFINITY, -INFINITY, DBL_MAX};
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  ng_defaults(config, control);
  config[NG_CONFIG_GRID_SIZE] = 16;
  config[NG_CONFIG_EMITTER_COUNT] = 16;
  config[NG_CONFIG_MAX_GRAINS] = 64;
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  control[NG_CONTROL_GRAIN_RATE] = 800;
  for (unsigned fault = 0; fault < 4; ++fault) {
    NGEngine *a = make(config, control, 48000, 24000, 2);
    NGEngine *b = make(config, control, 48000, 24000, 2);
    unsigned muted = 0;
    for (unsigned frame = 0; frame < 6000; ++frame) {
      /* Test-only damage to owned storage. The opcode rejects non-finite
       * source samples at preparation; this reaches the final emergency mute.
       */
      if (frame == 4800)
        for (unsigned i = 0; i < 997; ++i)
          ng_source(a)[i] = faults[fault];
      if (frame == 4937)
        memcpy(ng_source(a), ng_source(b), 997 * sizeof(double));
      double l, r, bl, br;
      ng_sample(a, &l, &r);
      ng_sample(b, &bl, &br);
      CHECK(isfinite(l) && isfinite(r));
      if (frame < 4800 || frame >= 4937)
        CHECK(l == bl && r == br);
      else if (l == 0 && r == 0 && (bl != 0 || br != 0))
        ++muted;
    }
    CHECK(muted > 0 && ng_counters(a).numeric_interventions > 0);
    CHECK(ng_counters(b).numeric_interventions == 0);
    double stats[NG_STAT_COUNT];
    ng_stats(a, stats);
    CHECK((unsigned)stats[NG_STAT_STATUS] & NG_STATUS_NUMERIC_INTERVENTION);
    check_stats(a, config);
    free(a);
    free(b);
    ++cases;
  }
}
static void rejected_field_is_transactional(void) {
  const double faults[] = {NAN, INFINITY, DBL_MAX};
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  ng_defaults(config, control);
  for (unsigned grid = 16; grid <= 64; grid *= 2) {
    NGField a, b;
    double *a_storage = calloc(ng_field_doubles(grid), sizeof(double));
    double *b_storage = calloc(ng_field_doubles(grid), sizeof(double));
    CHECK(a_storage && b_storage);
    ng_field_init(&a, grid, a_storage, 7);
    ng_field_init(&b, grid, b_storage, 7);
    size_t published_bytes = 4u * grid * grid * sizeof(double);
    for (unsigned fault = 0; fault < 3; ++fault) {
      double drive = control[NG_CONTROL_DRIVE];
      control[NG_CONTROL_DRIVE] = faults[fault];
      CHECK(!ng_field_step(&a, control, 60, 240, 64, 16));
      CHECK(!a.valid && a.interventions == fault + 1);
      CHECK(memcmp(a.u, b.u, published_bytes) == 0);
      CHECK(a.sequence == b.sequence && a.time == b.time);
      CHECK(a.energy == b.energy && a.max_speed == b.max_speed);
      CHECK(a.rms_omega == b.rms_omega && a.rms_strain == b.rms_strain);
      CHECK(a.rms_divergence == b.rms_divergence &&
            a.max_divergence == b.max_divergence);
      control[NG_CONTROL_DRIVE] = drive;
      CHECK(ng_field_step(&a, control, 60, 240, 64, 16));
      CHECK(ng_field_step(&b, control, 60, 240, 64, 16));
      CHECK(a.valid && b.valid && b.interventions == 0);
      CHECK(memcmp(a.u, b.u, published_bytes) == 0);
      CHECK(a.time == b.time && a.sequence == b.sequence);
      ++cases;
    }
    free(a_storage);
    free(b_storage);
  }
}
static void particle_fault_recovery(void) {
  NGField f;
  double *storage = calloc(ng_field_doubles(16), sizeof(double));
  CHECK(storage);
  ng_field_init(&f, 16, storage, 7);
  for (unsigned k = 0; k < 256; ++k) {
    f.u[k] = .2;
    f.v[k] = -.1;
  }
  f.max_speed = hypot(.2, .1);
  const double faults[] = {NAN, INFINITY, -INFINITY};
  for (unsigned component = 0; component < 7; ++component)
    for (unsigned fault = 0; fault < 3; ++fault) {
      NGParticle p = {.x = .3, .y = .6, .path_x = .3, .path_y = .6};
      double *values[] = {&p.x,  &p.y,  &p.path_x, &p.path_y,
                          &p.vx, &p.vy, &p.time};
      *values[component] = faults[fault];
      CHECK(ng_particle_advance(&f, &p, .01 - p.time, 100, 0) > 0);
      CHECK(isfinite(p.x) && p.x >= 0 && p.x < 1);
      CHECK(isfinite(p.y) && p.y >= 0 && p.y < 1);
      CHECK(isfinite(p.path_x) && isfinite(p.path_y));
      CHECK(isfinite(p.vx) && isfinite(p.vy) && isfinite(p.time));
      CHECK(isfinite(p.omega) && isfinite(p.strain) && isfinite(p.speed));
      CHECK(ng_particle_advance(&f, &p, .01, 100, 0) == 0);
      ++cases;
    }
  free(storage);
}
int main(void) {
  particle_fault_recovery();
  rejected_field_is_transactional();
  output_fault_recovery();
  configuration_boundaries();
  control_boundaries();
  audio_endpoints();
  combined_extrema_and_reset();
  printf("extrema: %u cases passed; un-limited peak %.9g; combined-extrema "
         "interventions %llu\n",
         cases, peak, (unsigned long long)interventions);
  return 0;
}
