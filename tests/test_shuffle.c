/* Inspect diagnostic-only state without exporting a test API in the plugin. */
#define NG_TEST_SPATIAL_SHUFFLE
#include "../src/naviergrain_core.c"
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

static void check_columns(const NGParticle *a, const NGParticle *b,
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

static NGEngine *make(unsigned count, unsigned capacity, unsigned seed) {
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  ng_defaults(config, control);
  config[NG_CONFIG_GRID_SIZE] = 16;
  config[NG_CONFIG_EMITTER_COUNT] = count;
  config[NG_CONFIG_MAX_GRAINS] = capacity;
  config[NG_CONFIG_SOURCE_LOOP] = 1;
  config[NG_CONFIG_SEED] = seed;
  NGConfig parsed;
  CHECK(!ng_config_parse(&parsed, config, NG_CONFIG_COUNT));
  size_t bytes = ng_memory_size(&parsed, 97, 8000, 8000);
  void *memory = malloc(bytes);
  CHECK(memory);
  NGEngine *e = ng_init(memory, bytes, &parsed, 97, 8000, 8000);
  CHECK(e);
  for (unsigned i = 0; i < 97; ++i)
    ng_source(e)[i] = .2 * sin(2 * NG_PI * i / 97);
  control[NG_CONTROL_GRAIN_RATE] = 80;
  ng_controls(e, control);
  return e;
}

static void population(unsigned count, unsigned capacity, unsigned seed) {
  NGEngine *e = make(count, capacity, seed);
  NGParticle *physical = malloc((count + capacity) * sizeof(NGParticle));
  NGParticle *mapped = malloc((count + capacity) * sizeof(NGParticle));
  CHECK(physical && mapped);
  for (unsigned i = 0; i < count; ++i) {
    e->emitters[i].x = (i + .25) / count;
    e->emitters[i].y = (i + .5) / count;
    e->emitters[i].omega = (double)i - count / 2.;
    e->emitters[i].strain = i;
    e->emitters[i].speed = i / 10.;
  }
  memcpy(physical, e->emitters, count * sizeof(NGParticle));
  /* Empty, single, sparse, and full voice populations. */
  for (unsigned mode = 0; mode < 4; ++mode) {
    unsigned live = 0;
    for (unsigned i = 0; i < capacity; ++i) {
      NGVoice *v = &e->voices[i];
      v->active = mode == 3 || (mode == 2 && i % 3 == 0) ||
                  (mode == 1 && i == capacity - 1);
      v->particle = e->emitters[i % count];
      if (v->active)
        physical[count + live++] = v->particle;
    }
    uint32_t schedule = e->schedule_rng, select = e->select_rng,
             emitter = e->emitter_rng;
    NGField field = e->field;
    shuffle_mappings(e);
    CHECK(!memcmp(physical, e->emitters, count * sizeof(NGParticle)));
    CHECK(!memcmp(&field, &e->field, sizeof(NGField)));
    CHECK(schedule == e->schedule_rng && select == e->select_rng &&
          emitter == e->emitter_rng);
    memcpy(mapped, e->mapping_emitters, count * sizeof(NGParticle));
    unsigned index = 0;
    for (unsigned i = 0; i < capacity; ++i) {
      if (!e->voices[i].active)
        continue;
      CHECK(!memcmp(&physical[count + index], &e->voices[i].particle,
                    sizeof(NGParticle)));
      mapped[count + index++] = e->voices[i].mapping;
    }
    const size_t emitter_columns[] = {
      offsetof(NGParticle, x), offsetof(NGParticle, y),
      offsetof(NGParticle, omega), offsetof(NGParticle, strain),
      offsetof(NGParticle, speed)};
    const size_t voice_columns[] = {offsetof(NGParticle, y), offsetof(NGParticle, omega)};
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
    CHECK(!memcmp(mapped, e->mapping_emitters, count * sizeof(NGParticle)));
    index = 0;
    for (unsigned i = 0; i < capacity; ++i)
      if (e->voices[i].active)
        CHECK(!memcmp(&mapped[count + index++], &e->voices[i].mapping,
                      sizeof(NGParticle)));
  }
  free(physical);
  free(mapped);
  free(e);
}

static void lifecycle(void) {
  NGEngine *a = make(128, 128, 17), *b = make(128, 128, 17);
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  ng_defaults(config, control);
  control[NG_CONTROL_GRAIN_RATE] = 80;
  for (unsigned i = 0; i < 16000; ++i) {
    if (i == 3000 || i == 3500 || i == 7000 || i == 8000) {
      control[NG_CONTROL_RESET] = i == 3000;
      control[NG_CONTROL_FREEZE] = i == 7000;
      ng_controls(a, control);
      ng_controls(b, control);
    }
    double al, ar, bl, br;
    ng_sample(a, &al, &ar);
    ng_sample(b, &bl, &br);
    CHECK(isfinite(al) && isfinite(ar) && al == bl && ar == br);
  }
  CHECK(ng_counters(a).epoch == 1 && ng_counters(a).births > 0);
  CHECK(!ng_counters(a).numeric_interventions && !ng_counters(a).voice_drops);
  free(a);
  free(b);
}

/* An uncached scalar oracle exercises changed, unchanged and reassigned
 * mappings while pitch-depth automation continues at audio rate. */
static void planning_mapping_oracle(void) {
  NGEngine *e = make(32, 64, 17);
  double config[NG_CONFIG_COUNT], control[NG_CONTROL_COUNT];
  ng_defaults(config, control);
  control[NG_CONTROL_FREEZE] = 1;
  control[NG_CONTROL_GRAIN_RATE] = 0;
  control[NG_CONTROL_GRAIN_MS] = 500;
  ng_controls(e, control);
  const double omega[] = {0., -0., 3., 3., -7., 100., -3.};
  for (unsigned reuse = 0; reuse < 2; ++reuse) {
    birth(e);
    NGVoice *v = &e->voices[0];
    CHECK(v->active);
    for (unsigned i = 0; i < 140; ++i) {
      v->mapping.omega = omega[(i / 5 + reuse) % 7];
      control[NG_CONTROL_PITCH_DEPTH] = i < 70 ? 18 : 2;
      ng_controls(e, control);
      double previous_pitch = v->log_pitch, previous_phase = v->phase;
      double left, right;
      ng_sample(e, &left, &right);
      double mix = e->smooth[NG_CONTROL_MAPPING_MIX] * e->modulation;
      double pitch = clamp(v->base_log_pitch +
          mix * e->smooth[NG_CONTROL_PITCH_DEPTH] *
          tanh(v->mapping.omega / 10) / 12, -2, 2);
      double expected = previous_pitch + e->smooth10 * (pitch - previous_pitch);
      CHECK(v->log_pitch == expected);
      CHECK(v->increment == exp2(expected) * (e->source_sr / e->sr));
      CHECK(v->phase == fmod(previous_phase + v->increment, (double)e->source_length));
      CHECK(isfinite(left) && isfinite(right));
    }
    v->age = v->length - 1;
    double left, right;
    ng_sample(e, &left, &right);
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
