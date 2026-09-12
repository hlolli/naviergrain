/* Inspect diagnostic-only state without exporting a test API in the plugin. */
#define FG_TEST_SPATIAL_SHUFFLE
#include "../src/fluidgrain_core.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "shuffle line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static unsigned distributions;
static double maximum_correlation;
static int compare(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static void check_columns(const FGParticle *a, const FGParticle *b,
                           unsigned count, const size_t *columns,
                           unsigned column_count) {
  double *left = malloc((count + 1) * sizeof(double));
  double *right = malloc((count + 1) * sizeof(double));
  CHECK(left && right);
  for (unsigned c = 0; c < column_count; ++c) {
    for (unsigned i = 0; i < count; ++i) {
      memcpy(&left[i], (const char *)&a[i] + columns[c], sizeof(double));
      memcpy(&right[i], (const char *)&b[i] + columns[c], sizeof(double));
    }
    qsort(left, count, sizeof(double), compare);
    qsort(right, count, sizeof(double), compare);
    CHECK(!memcmp(left, right, count * sizeof(double)));
    ++distributions;
  }
  free(left);
  free(right);
}

static FGEngine *make(unsigned count, unsigned capacity, unsigned seed) {
  double config[FG_CONFIG_COUNT], control[FG_CONTROL_COUNT];
  fg_defaults(config, control);
  config[FG_CONFIG_GRID_SIZE] = 16;
  config[FG_CONFIG_EMITTER_COUNT] = count;
  config[FG_CONFIG_MAX_GRAINS] = capacity;
  config[FG_CONFIG_SOURCE_LOOP] = 1;
  config[FG_CONFIG_SEED] = seed;
  FGConfig parsed;
  CHECK(!fg_config_parse(&parsed, config, FG_CONFIG_COUNT));
  size_t bytes = fg_memory_size(&parsed, 97, 8000, 8000);
  void *memory = malloc(bytes);
  CHECK(memory);
  FGEngine *e = fg_init(memory, bytes, &parsed, 97, 8000, 8000);
  CHECK(e);
  for (unsigned i = 0; i < 97; ++i)
    fg_source(e)[i] = .2 * sin(2 * FG_PI * i / 97);
  control[FG_CONTROL_GRAIN_RATE] = 80;
  fg_controls(e, control);
  return e;
}

static void population(unsigned count, unsigned capacity, unsigned seed) {
  FGEngine *e = make(count, capacity, seed);
  FGParticle *physical = malloc((count + capacity) * sizeof(FGParticle));
  FGParticle *mapped = malloc((count + capacity) * sizeof(FGParticle));
  CHECK(physical && mapped);
  for (unsigned i = 0; i < count; ++i) {
    e->emitters[i].x = (i + .25) / count;
    e->emitters[i].y = (i + .5) / count;
    e->emitters[i].omega = (double)i - count / 2.;
    e->emitters[i].strain = i;
    e->emitters[i].speed = i / 10.;
  }
  memcpy(physical, e->emitters, count * sizeof(FGParticle));
  /* Empty, single, sparse, and full voice populations. */
  for (unsigned mode = 0; mode < 4; ++mode) {
    unsigned live = 0;
    for (unsigned i = 0; i < capacity; ++i) {
      FGVoice *v = &e->voices[i];
      v->active = mode == 3 || (mode == 2 && i % 3 == 0) ||
                  (mode == 1 && i == capacity - 1);
      v->particle = e->emitters[i % count];
      if (v->active)
        physical[count + live++] = v->particle;
    }
    uint32_t schedule = e->schedule_rng, select = e->select_rng,
             emitter = e->emitter_rng;
    FGField field = e->field;
    shuffle_mappings(e);
    CHECK(!memcmp(physical, e->emitters, count * sizeof(FGParticle)));
    CHECK(!memcmp(&field, &e->field, sizeof(FGField)));
    CHECK(schedule == e->schedule_rng && select == e->select_rng &&
          emitter == e->emitter_rng);
    memcpy(mapped, e->mapping_emitters, count * sizeof(FGParticle));
    unsigned index = 0;
    for (unsigned i = 0; i < capacity; ++i) {
      if (!e->voices[i].active)
        continue;
      CHECK(!memcmp(&physical[count + index], &e->voices[i].particle,
                    sizeof(FGParticle)));
      mapped[count + index++] = e->voices[i].mapping;
    }
    const size_t emitter_columns[] = {
      offsetof(FGParticle, x), offsetof(FGParticle, y),
      offsetof(FGParticle, omega), offsetof(FGParticle, strain),
      offsetof(FGParticle, speed)};
    const size_t voice_columns[] = {offsetof(FGParticle, y), offsetof(FGParticle, omega)};
    check_columns(physical, mapped, count, emitter_columns, 5);
    check_columns(physical + count, mapped + count, live, voice_columns, 2);
    if (count >= 128) {
      unsigned paired = 0;
      double covariance = 0, x_power = 0, y_power = 0;
      for (unsigned i = 0; i < count; ++i) {
        paired += fabs(mapped[i].y - mapped[i].x - .25 / count) < 1e-12;
        double x = mapped[i].x - (.5 - .25 / count);
        double y = mapped[i].y - .5;
        covariance += x * y;
        x_power += x * x;
        y_power += y * y;
      }
      CHECK(paired < count / 8); /* Originally every x/y pair was correlated. */
      double correlation = fabs(covariance / sqrt(x_power * y_power));
      CHECK(correlation < .3); /* Reassignment alone need not decorrelate. */
      maximum_correlation = fmax(maximum_correlation, correlation);
    }
    shuffle_mappings(e);
    CHECK(!memcmp(mapped, e->mapping_emitters, count * sizeof(FGParticle)));
    index = 0;
    for (unsigned i = 0; i < capacity; ++i)
      if (e->voices[i].active)
        CHECK(!memcmp(&mapped[count + index++], &e->voices[i].mapping,
                      sizeof(FGParticle)));
  }
  free(physical);
  free(mapped);
  free(e);
}

static void lifecycle(void) {
  FGEngine *a = make(128, 128, 17), *b = make(128, 128, 17);
  double config[FG_CONFIG_COUNT], control[FG_CONTROL_COUNT];
  fg_defaults(config, control);
  control[FG_CONTROL_GRAIN_RATE] = 80;
  for (unsigned i = 0; i < 16000; ++i) {
    if (i == 3000 || i == 3500 || i == 7000 || i == 8000) {
      control[FG_CONTROL_RESET] = i == 3000;
      control[FG_CONTROL_FREEZE] = i == 7000;
      fg_controls(a, control);
      fg_controls(b, control);
    }
    double al, ar, bl, br;
    fg_sample(a, &al, &ar);
    fg_sample(b, &bl, &br);
    CHECK(isfinite(al) && isfinite(ar) && al == bl && ar == br);
  }
  CHECK(fg_counters(a).epoch == 1 && fg_counters(a).births > 0);
  CHECK(!fg_counters(a).numeric_interventions && !fg_counters(a).voice_drops);
  free(a);
  free(b);
}

/* An uncached scalar oracle exercises changed, unchanged and reassigned
 * mappings while pitch-depth automation continues at audio rate. */
static void planning_mapping_oracle(void) {
  FGEngine *e = make(32, 64, 17);
  double config[FG_CONFIG_COUNT], control[FG_CONTROL_COUNT];
  fg_defaults(config, control);
  control[FG_CONTROL_FREEZE] = 1;
  control[FG_CONTROL_GRAIN_RATE] = 0;
  control[FG_CONTROL_GRAIN_MS] = 500;
  fg_controls(e, control);
  const double omega[] = {0., -0., 3., 3., -7., 100., -3.};
  for (unsigned reuse = 0; reuse < 2; ++reuse) {
    birth(e);
    FGVoice *v = &e->voices[0];
    CHECK(v->active);
    for (unsigned i = 0; i < 140; ++i) {
      v->mapping.omega = omega[(i / 5 + reuse) % 7];
      control[FG_CONTROL_PITCH_DEPTH] = i < 70 ? 18 : 2;
      fg_controls(e, control);
      double previous_pitch = v->log_pitch, previous_phase = v->phase;
      double left, right;
      fg_sample(e, &left, &right);
      double mix = e->smooth[FG_CONTROL_MAPPING_MIX] * e->modulation;
      double pitch = clamp(v->base_log_pitch +
          mix * e->smooth[FG_CONTROL_PITCH_DEPTH] *
          tanh(v->mapping.omega / 10) / 12, -2, 2);
      double expected = previous_pitch + e->smooth10 * (pitch - previous_pitch);
      CHECK(v->log_pitch == expected);
      CHECK(v->increment == exp2(expected) * (e->source_sr / e->sr));
      CHECK(v->phase == fmod(previous_phase + v->increment, (double)e->source_length));
      CHECK(isfinite(left) && isfinite(right));
    }
    v->age = v->length - 1;
    double left, right;
    fg_sample(e, &left, &right);
    CHECK(!v->active && e->free_indices[e->free_count - 1] == 0);
  }
  free(e);
}

int main(void) {
  planning_mapping_oracle();
  const unsigned counts[] = {1, 3, 128, 1024, 4096};
  for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
    for (unsigned seed = 0; seed < 3; ++seed)
      population(counts[i], counts[i], seed);
  lifecycle();
  printf("shuffle: %u exact marginal distributions; stable permutations, "
         "sparse pools, independent RNG, reset/freeze determinism pass; "
         "maximum synthetic |correlation|=%.6f (originally 1)\n",
         distributions, maximum_correlation);
  return 0;
}
