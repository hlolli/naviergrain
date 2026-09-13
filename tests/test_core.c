#include "naviergrain_core.h"
#include "naviergrain_particles.h"
#include "naviergrain_resampler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
static NGEngine *make(double sr, double source_sr, size_t n) {
  NGConfig c;
  CHECK(!ng_config_parse(&c, config, NG_CONFIG_COUNT));
  size_t bytes = ng_memory_size(&c, n, source_sr, sr);
  CHECK(bytes);
  void *memory = malloc(bytes);
  CHECK(memory);
  NGEngine *e = ng_init(memory, bytes, &c, n, source_sr, sr);
  CHECK(e);
  for (size_t i = 0; i < n; ++i)
    ng_source(e)[i] = sin(6.283185307179586 * (double)i / 37.0);
  ng_controls(e, control);
  return e;
}
static void render(NGEngine *e, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    double l, r;
    ng_sample(e, &l, &r);
    CHECK(isfinite(l) && isfinite(r));
  }
}
static void validation(void) {
  ng_defaults(config, control);
  NGConfig c;
  CHECK(!ng_config_parse(&c, config, NG_CONFIG_COUNT));
  CHECK(ng_config_parse(&c, config, 12));
  config[NG_CONFIG_BACKEND] = 1;
  CHECK(strstr(ng_config_parse(&c, config, 13), "external"));
  config[NG_CONFIG_BACKEND] = 0;
  config[NG_CONFIG_GRID_SIZE] = 24;
  CHECK(ng_config_parse(&c, config, 13));
  config[NG_CONFIG_GRID_SIZE] = 32;
  config[NG_CONFIG_SEED] = NAN;
  CHECK(ng_config_parse(&c, config, 13));
  config[NG_CONFIG_SEED] = 0;
  CHECK(!ng_config_parse(&c, config, 13));
  CHECK(!ng_memory_size(&c, SIZE_MAX, 48000, 48000));
  CHECK(!ng_memory_size(&c, 0, 48000, 48000));
  NGEngine *e = make(48000, 48000, 17);
  control[0] = INFINITY;
  control[1] = -1;
  control[23] = .5;
  ng_controls(e, control);
  CHECK(ng_counters(e).control_events == 3);
  render(e, 100);
  free(e);
}
static void interpolation(void) {
  double s[] = {0, 1, 2, 3, 4};
  CHECK(fabs(ng_read_preview(s, 5, 2.5, 1, 1) - 2.5) < 1e-12);
  CHECK(ng_read_preview(s, 5, 5, 1, 1) == 0);
  CHECK(ng_read_preview(s, 5, -1, 1, 1) == 4);
  CHECK(ng_read_preview(s, 5, 0, 0, 1) == 0);
  CHECK(ng_read_preview(s, 5, 4, 0, 1) == 0);
  CHECK(ng_read_preview(s, 5, 5, 0, 1) == 0);
  CHECK(ng_read_preview(s, 5, NAN, 1, 1) == 0);
  double one[] = {.7};
  CHECK(fabs(ng_read_preview(one, 1, 33.3, 1, 1) - .7) < 1e-12);
  for (size_t n = 1; n <= 5; ++n)
    for (int i = -20; i < 100; ++i)
      CHECK(isfinite(ng_read_preview(s, n, i * .17, 1, 5)));
}
static void clocks(void) {
  const int rates[] = {44100, 48000, 96000}, blocks[] = {1, 16, 32, 64, 37};
  for (size_t rate = 0; rate < 3; ++rate) {
    ng_defaults(config, control);
    config[NG_CONFIG_SEED] = 0;
    config[NG_CONFIG_SOURCE_LOOP] = 1;
    control[NG_CONTROL_SCHEDULER] = 0;
    control[NG_CONTROL_GRAIN_RATE] = 123;
    control[NG_CONTROL_MAPPING_MIX] = 0;
    for (size_t block = 0; block < 5; ++block) {
      NGEngine *a = make(rates[rate], 22050, 997),
               *b = make(rates[rate], 22050, 997);
      double energy = 0;
      for (int i = 0; i < rates[rate]; ++i) {
        if (i % blocks[block] == 0)
          ng_controls(b, control);
        double l, r, bl, br;
        ng_sample(a, &l, &r);
        ng_sample(b, &bl, &br);
        CHECK(l == bl && r == br);
        CHECK(fabs(l - r) < 1e-12);
        energy += l * l + r * r;
      }
      CHECK(energy > 0);
      CHECK(ng_counters(a).births == 123);
      uint64_t births = ng_counters(a).births;
      control[NG_CONTROL_GRAIN_RATE] = 0;
      ng_controls(a, control);
      render(a, (size_t)rates[rate]);
      CHECK(ng_counters(a).births == births);
      double stats[NG_STAT_COUNT];
      ng_stats(a, stats);
      CHECK(stats[NG_STAT_LIVE_GRAINS] == 0);
      free(a);
      free(b);
      control[NG_CONTROL_GRAIN_RATE] = 123;
    }
  }
}
static void source_rate_and_window(void) {
  for (int ratio = 1; ratio <= 2; ++ratio) {
    ng_defaults(config, control);
    config[NG_CONFIG_SOURCE_LOOP] = 1;
    control[NG_CONTROL_GRAIN_RATE] = 1;
    control[NG_CONTROL_SCHEDULER] = 0;
    control[NG_CONTROL_GRAIN_MS] = 500;
    control[NG_CONTROL_MAPPING_MIX] = 0;
    control[NG_CONTROL_POSITION_CENTER] = 0;
    control[NG_CONTROL_GAIN] = 1;
    NGEngine *e = make(1000, 1000 * ratio, 37);
    render(e, 999);
    for (int age = 0; age < 500; ++age) {
      double l, r;
      ng_sample(e, &l, &r);
      double envelope = .5 - .5 * cos(6.283185307179586 * age / 499);
      double expected = sin(6.283185307179586 * ((age * ratio) % 37) / 37) *
                        envelope / sqrt(2.0);
      CHECK(fabs(l - expected) < 5e-5 && fabs(r - expected) < 5e-5);
      if (age == 0 || age == 499)
        CHECK(l == 0 && r == 0);
    }
    CHECK(ng_counters(e).births == 1);
    free(e);
  }
}
static void poisson_and_caps(void) {
  ng_defaults(config, control);
  control[NG_CONTROL_MAPPING_MIX] = 0;
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  NGEngine *e = make(48000, 48000, 97);
  render(e, 48000 * 30);
  /* Poisson(3600), fixed seed: six standard deviations is a conservative
   * regression gate. */
  CHECK(fabs((double)ng_counters(e).births - 3600) < 360);
  free(e);
  config[NG_CONFIG_MAX_GRAINS] = 1;
  control[NG_CONTROL_GRAIN_RATE] = 2000;
  e = make(48000, 48000, 97);
  render(e, 48000);
  CHECK(ng_counters(e).voice_drops > 1000);
  free(e);
  config[NG_CONFIG_MAX_GRAINS] = 512;
  control[NG_CONTROL_SCHEDULER] = 0;
  e = make(100, 100, 7);
  render(e, 100);
  CHECK(ng_counters(e).births == 400);
  CHECK(ng_counters(e).cap_drops == 1600);
  free(e);
  control[NG_CONTROL_SCHEDULER] = 1;
  e = make(100, 100, 7);
  render(e, 100);
  CHECK(ng_counters(e).births <= 400);
  CHECK(ng_counters(e).cap_drops > 0);
  CHECK(ng_counters(e).discarded_hazard > 0);
  free(e);
}
static void reset_and_freeze(void) {
  ng_defaults(config, control);
  control[NG_CONTROL_MAPPING_MIX] = 0;
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  NGEngine *a = make(48000, 48000, 83), *b = make(48000, 48000, 83);
  control[NG_CONTROL_FREEZE] = 1;
  ng_controls(b, control);
  for (int i = 0; i < 10000; ++i) {
    double l, r, bl, br;
    ng_sample(a, &l, &r);
    ng_sample(b, &bl, &br);
    CHECK(l == bl && r == br);
  }
  double saved = ng_source(a)[5];
  control[NG_CONTROL_RESET] = 1;
  ng_controls(a, control);
  render(a, 2000);
  CHECK(ng_counters(a).epoch == 1);
  CHECK(ng_source(a)[5] == saved);
  render(a, 2000);
  CHECK(ng_counters(a).epoch == 1);
  control[NG_CONTROL_RESET] = 0;
  ng_controls(a, control);
  control[NG_CONTROL_RESET] = 1;
  ng_controls(a, control);
  render(a, 2000);
  CHECK(ng_counters(a).epoch == 2);
  free(a);
  free(b);
}
static void fluid_audio_and_density(void) {
  ng_defaults(config, control);
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  config[NG_CONFIG_MAX_GRAINS] = 2048;
  control[NG_CONTROL_GRAIN_RATE] = 2000;
  control[NG_CONTROL_GRAIN_MS] = 500;
  control[NG_CONTROL_GAIN] = .03;
  control[NG_CONTROL_SCHEDULER] = 0;
  NGEngine *a = make(48000, 48000, 997), *b = make(48000, 48000, 997);
  double expected = 0, peak_live = 0, stats[NG_STAT_COUNT], left_energy = 0,
         side = 0;
  for (unsigned frame = 0; frame < 96000; ++frame) {
    if (frame % 37 == 0)
      ng_controls(b, control);
    double l, r, bl, br;
    ng_sample(a, &l, &r);
    ng_sample(b, &bl, &br);
    CHECK(l == bl && r == br && isfinite(l) && isfinite(r));
    left_energy += l * l;
    side += (l - r) * (l - r);
    ng_stats(a, stats);
    expected += stats[NG_STAT_EFFECTIVE_RATE] / 48000;
    peak_live = fmax(peak_live, stats[NG_STAT_LIVE_GRAINS]);
    CHECK(stats[NG_STAT_NUMERIC_INTERVENTIONS] == 0);
  }
  CHECK(ng_counters(a).births == (uint64_t)floor(expected + 1e-8));
  CHECK(peak_live > 750 && ng_counters(a).voice_drops == 0);
  CHECK(left_energy > 0 && side > 0);
  CHECK(stats[NG_STAT_KINETIC_ENERGY] > 0 && stats[NG_STAT_RMS_VORTICITY] > 0);
  CHECK(stats[NG_STAT_RMS_STRAIN] > 0 &&
        stats[NG_STAT_SNAPSHOT_SEQUENCE] == 120);
  printf("dense core: peak %.0f live grains, %llu births, zero "
         "drops/interventions\n",
         peak_live, (unsigned long long)ng_counters(a).births);
  double energy = stats[NG_STAT_KINETIC_ENERGY],
         sequence = stats[NG_STAT_SNAPSHOT_SEQUENCE];
  uint64_t births = ng_counters(a).births;
  control[NG_CONTROL_FREEZE] = 1;
  ng_controls(a, control);
  render(a, 1000);
  ng_stats(a, stats);
  CHECK(stats[NG_STAT_KINETIC_ENERGY] == energy &&
        stats[NG_STAT_SNAPSHOT_SEQUENCE] == sequence);
  CHECK(ng_counters(a).births > births && stats[NG_STAT_LIVE_GRAINS] > 0);
  control[NG_CONTROL_FREEZE] = 0;
  ng_controls(a, control);
  render(a, 1000);
  ng_stats(a, stats);
  CHECK(stats[NG_STAT_SNAPSHOT_SEQUENCE] > sequence);
  /* Freeze from initialization must be reversible, too. */
  free(b);
  control[NG_CONTROL_FREEZE] = 1;
  b = make(48000, 48000, 997);
  render(b, 1000);
  ng_stats(b, stats);
  CHECK(stats[NG_STAT_SNAPSHOT_SEQUENCE] == 0);
  control[NG_CONTROL_FREEZE] = 0;
  ng_controls(b, control);
  render(b, 1000);
  ng_stats(b, stats);
  CHECK(stats[NG_STAT_SNAPSHOT_SEQUENCE] > 0);
  /* The zero-speed endpoint also freezes immediately, without a catch-up tick.
   */
  ng_stats(a, stats);
  sequence = stats[NG_STAT_SNAPSHOT_SEQUENCE];
  control[NG_CONTROL_FLOW_SPEED] = 0;
  ng_controls(a, control);
  render(a, 1000);
  ng_stats(a, stats);
  CHECK(stats[NG_STAT_SNAPSHOT_SEQUENCE] == sequence);
  free(a);
  free(b);
}
static void newborn_particle_age(void) {
  ng_defaults(config, control);
  config[NG_CONFIG_MAX_GRAINS] = 1;
  control[NG_CONTROL_SCHEDULER] = 0;
  control[NG_CONTROL_GRAIN_RATE] = 1234;
  control[NG_CONTROL_GRAIN_MS] = 500;
  control[NG_CONTROL_DRIVE] = 0;
  control[NG_CONTROL_ATTRACTION] = 1;
  NGEngine *e = make(48000, 48000, 997);
  NGGrainState initial, actual;
  CHECK(!ng_grain_state(e, 0, &initial));
  render(e, 39); /* first periodic birth, between the 0 and 200-sample ticks */
  CHECK(ng_grain_state(e, 0, &initial) && initial.age == 1);
  CHECK(!ng_grain_state(e, 1, &actual));
  CHECK(fabs(initial.simulation_time - 39.0 / 48000) < 1e-14);
  double *storage = calloc(ng_field_doubles(32), sizeof(double));
  CHECK(storage);
  NGField zero;
  ng_field_init(&zero, 32, storage, 0);
  NGParticle expected = {.x = initial.x,
                         .y = initial.y,
                         .path_x = initial.path_x,
                         .path_y = initial.path_y,
                         .vx = initial.velocity_x,
                         .vy = initial.velocity_y};
  CHECK(!ng_particle_advance(&zero, &expected, 161.0 / 48000, 0, 1));
  render(e, 161);
  CHECK(ng_grain_state(e, 0, &actual) && actual.id == initial.id &&
        actual.age == 162);
  CHECK(fabs(actual.x - expected.x) < 1e-14 &&
        fabs(actual.y - expected.y) < 1e-14);
  CHECK(!ng_particle_advance(&zero, &expected, 200.0 / 48000, 0, 1));
  render(e, 200);
  CHECK(ng_grain_state(e, 0, &actual) && actual.id == initial.id);
  CHECK(fabs(actual.x - expected.x) < 1e-14 &&
        fabs(actual.y - expected.y) < 1e-14);
  control[NG_CONTROL_FREEZE] = 1;
  ng_controls(e, control);
  render(e, 200);
  NGGrainState frozen;
  CHECK(ng_grain_state(e, 0, &frozen));
  CHECK(frozen.x == actual.x && frozen.y == actual.y &&
        frozen.age == actual.age + 200);
  CHECK(frozen.length == 24000);
  free(storage);
  free(e);
}
static void timestamped_controls(void) {
  ng_defaults(config, control);
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  control[NG_CONTROL_GRAIN_RATE] = 450;
  NGEngine *a = make(48000, 96000, 997), *b = make(48000, 96000, 997);
  for (unsigned frame = 0; frame < 24000; ++frame) {
    int event = 1;
    switch (frame) {
    case 137:
      control[NG_CONTROL_PITCH_RATIO] = 4;
      break;
    case 1701:
      control[NG_CONTROL_SCHEDULER] = 0;
      break;
    case 3107:
      control[NG_CONTROL_FREEZE] = 1;
      break;
    case 5011:
      control[NG_CONTROL_FREEZE] = 0;
      break;
    case 7013:
      control[NG_CONTROL_RESET] = 1;
      break;
    case 9109:
      control[NG_CONTROL_RESET] = 0;
      break;
    case 11003:
      control[NG_CONTROL_PITCH_RATIO] = .25;
      break;
    case 13001:
      control[NG_CONTROL_GRAIN_RATE] = 0;
      break;
    case 19001:
      control[NG_CONTROL_GRAIN_RATE] = 800;
      break;
    default:
      event = 0;
      break;
    }
    /* Deliver events at their actual sample, not a block-quantized substitute.
     * Re-sending unchanged controls at different host boundaries has no effect.
     */
    ng_controls(a, control);
    if (event || frame % 37 == 0)
      ng_controls(b, control);
    double l, r, bl, br;
    ng_sample(a, &l, &r);
    ng_sample(b, &bl, &br);
    CHECK(l == bl && r == br && isfinite(l) && isfinite(r));
  }
  CHECK(ng_counters(a).epoch == 1 && ng_counters(b).epoch == 1);
  CHECK(ng_counters(a).births == ng_counters(b).births);
  free(a);
  free(b);
}
static void extreme_source_rate(void) {
  /* Positive finite source rates have no schema upper limit. Form the ratio
   * before multiplying by pitch, avoiding an overflowing intermediate. This
   * only checks finite/DC playback, not anti-alias quality at such a ratio. */
  ng_defaults(config, control);
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  config[NG_CONFIG_MAX_GRAINS] = 4;
  control[NG_CONTROL_PITCH_RATIO] = 4;
  control[NG_CONTROL_GRAIN_RATE] = 2000;
  control[NG_CONTROL_MAPPING_MIX] = 0;
  NGEngine *e = make(48000, 1e308, 5);
  double energy = 0;
  for (unsigned i = 0; i < 1000; ++i) {
    double l, r;
    ng_sample(e, &l, &r);
    CHECK(isfinite(l) && isfinite(r));
    energy += l * l + r * r;
  }
  CHECK(energy > 0);
  free(e);
}
int main(void) {
  validation();
  interpolation();
  clocks();
  source_rate_and_window();
  poisson_and_caps();
  reset_and_freeze();
  fluid_audio_and_density();
  newborn_particle_age();
  timestamped_controls();
  extreme_source_rate();
  puts("core: validation, interpolation, clocks, Poisson, overflow, freeze and "
       "reset passed");
  return 0;
}
