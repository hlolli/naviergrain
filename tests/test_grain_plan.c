#include "naviergrain_grain_plan.h"
#include "naviergrain_pack.h"
#include "naviergrain_provider.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct {
  NGEngine *engine;
  NGProvider *provider;
} Fixture;
static Fixture make(const NGConfig *config, double rate, double ratio) {
  size_t bytes = ng_memory_size(config, 997, rate * ratio, rate);
  Fixture f = {.engine = ng_init(malloc(bytes), bytes, config, 997, rate * ratio, rate)};
  CHECK(f.engine);
  for (unsigned i = 0; i < 997; ++i)
    ng_source(f.engine)[i] = .3 * sin(6.283185307179586 * i / 37);
  if (config->value[NG_CONFIG_BACKEND]) {
    bytes = ng_provider_size(config);
    f.provider = ng_provider_init(malloc(bytes), bytes, config, rate, 17);
    CHECK(f.provider && ng_provider_attach(f.provider, f.engine));
  }
  return f;
}
static void dispose(Fixture f) {
  if (f.provider) { ng_provider_release(f.provider); free(f.provider); }
  free(f.engine);
}
static void controls(double *c, unsigned f) {
  c[NG_CONTROL_GRAIN_RATE] = f < 2200 ? 2000 : 713;
  c[NG_CONTROL_GRAIN_MS] = f < 1703 ? 45 : 5;
  c[NG_CONTROL_PITCH_RATIO] = f < 1203 ? .25 : 4;
  c[NG_CONTROL_STEREO_WIDTH] = f < 1111 ? .9 : .2;
  c[NG_CONTROL_FREEZE] = f >= 1777 && f < 2013;
  c[NG_CONTROL_RESET] = f >= 2501 && f < 2509;
  c[NG_CONTROL_GAIN] = f == 117 ? NAN : .15;
}
static void compare(unsigned grid, unsigned backend, unsigned scheduler,
                     unsigned looping, double rate, double ratio, uint32_t batch) {
  double values[NG_CONFIG_COUNT], c[NG_CONTROL_COUNT];
  ng_defaults(values, c);
  values[NG_CONFIG_GRID_SIZE] = grid;
  values[NG_CONFIG_EMITTER_COUNT] = 32;
  values[NG_CONFIG_MAX_GRAINS] = 64;
  values[NG_CONFIG_SOURCE_LOOP] = looping;
  values[NG_CONFIG_BACKEND] = backend;
  values[NG_CONFIG_INSTANCE_ID] = 7;
  NGConfig config;
  CHECK(!ng_config_parse(&config, values, NG_CONFIG_COUNT));
  c[NG_CONTROL_SCHEDULER] = scheduler;
  Fixture reference = make(&config, rate, ratio), split = make(&config, rate, ratio);
  size_t bytes = ng_grain_plan_size(&config, batch);
  NGGrainPlan *plan = ng_grain_plan_init(malloc(bytes), bytes, &config, batch);
  CHECK(plan);
  size_t pack_capacity=ng_pack_size_frames(64,batch), scratch_bytes=ng_pack_scratch_size_frames(64,batch);
  void *packed=malloc(pack_capacity), *scratch=malloc(scratch_bytes);
  void *repacked=malloc(pack_capacity);
  CHECK(packed && scratch && repacked);
  size_t exact_bytes=0, packed_bytes=0; double peak_error=0;
  uint64_t packet_hash = UINT64_C(14695981039346656037);
  double energy = 0;
  size_t captured = 0;
  for (unsigned start = 0; start < 6000;) {
    unsigned count = batch;
    if (count > 6000-start) count = 6000-start;
    double expected[1024], output[1024], retry[1024], a[NG_STAT_COUNT], b[NG_STAT_COUNT];
    CHECK(ng_grain_plan_begin(split.engine, plan));
    CHECK(!ng_grain_plan_begin(reference.engine, plan));
    CHECK(!ng_grain_plan_begin(split.engine, plan));
    CHECK(!ng_grain_plan_seal(split.engine, plan));
    CHECK(!ng_grain_plan_render(plan, output, 1024));
    for (unsigned i = 0; i < count; ++i) {
      controls(c, start+i);
      ng_controls(reference.engine, c);
      ng_controls(split.engine, c);
      ng_sample(reference.engine, &expected[2*i], &expected[2*i+1]);
      CHECK(ng_grain_plan_sample(split.engine, plan));
      if (backend && !(start+i >= 701 && start+i < 1701)) {
        CHECK(ng_provider_work(reference.provider) >= 0);
        CHECK(ng_provider_work(split.provider) >= 0);
      }
    }
    CHECK(ng_grain_plan_seal(split.engine, plan));
    CHECK(!ng_grain_plan_sample(split.engine, plan));
    NGGrainPlanView view;
    CHECK(ng_grain_plan_view(plan, &view));
    CHECK(view.start_frame == start && view.frames == count);
    for (unsigned f = 0; f < count; ++f) {
      CHECK(view.frame[f].offset + view.frame[f].count <= view.grains);
      CHECK(view.frame[f].count <= 64);
    }
    captured += view.grains;
    CHECK(!ng_grain_plan_render(plan, output, count*2-1));
    CHECK(ng_grain_plan_render(plan, output, 1024));
    size_t used=ng_pack(plan,packed,pack_capacity,scratch,scratch_bytes);
    CHECK(used && ng_pack_render(plan,packed,used,retry,1024));
    /* Reusing a sealed plan must not depend on previous scratch contents or
     * coordinate-cache state, and must not move the pending audio clock. */
    memset(scratch, 0xa5, scratch_bytes);
    CHECK(ng_pack(plan,repacked,pack_capacity,scratch,scratch_bytes) == used);
    CHECK(!memcmp(packed,repacked,used));
    for (size_t k = 0; k < used; ++k) {
      packet_hash ^= ((const unsigned char *)packed)[k];
      packet_hash *= UINT64_C(1099511628211);
    }
    for(unsigned k=0;k<count*2;++k) {
      peak_error=fmax(peak_error,fabs(output[k]-retry[k]));
      CHECK(fabs(output[k]-retry[k])<1e-5);
    }
    exact_bytes+=view.grains*sizeof(NGPlannedGrain);packed_bytes+=used;
    /* A failed GPU/readback can retry C without advancing any state. */
    CHECK(ng_grain_plan_render(plan, retry, 1024));
    CHECK(!memcmp(output, retry, count*2*sizeof(double)));
    double left = 1, right = 1;
    ng_sample(split.engine, &left, &right);
    CHECK(left == 0 && right == 0);
    CHECK(!ng_grain_plan_commit(reference.engine, plan, output, 1024));
    CHECK(!ng_grain_plan_commit(split.engine, plan, output, count*2-1));
    CHECK(ng_grain_plan_commit(split.engine, plan, output, 1024));
    CHECK(!ng_grain_plan_commit(split.engine, plan, output, 1024));
    CHECK(!ng_grain_plan_view(plan, &view));
    CHECK(!ng_grain_plan_render(plan, output, 1024));
    CHECK(!memcmp(expected, output, count*2*sizeof(double)));
    ng_stats(reference.engine, a); ng_stats(split.engine, b);
    CHECK(!memcmp(a, b, sizeof(a)));
    for (unsigned i = 0; i < count*2; ++i) energy += output[i]*output[i];
    start += count;
  }
  CHECK(captured > 0 && energy > 0);
  CHECK(ng_counters(split.engine).births == ng_counters(reference.engine).births);
  CHECK(ng_counters(split.engine).epoch == 1);
  /* Return to the ordinary opcode path without losing a sample. */
  for (unsigned i = 0; i < 17; ++i) {
    double a, b, c0, d;
    ng_sample(reference.engine, &a, &b); ng_sample(split.engine, &c0, &d);
    CHECK(a == c0 && b == d);
  }
  printf("batch=%u rate=%g ratio=%g max_error=%.9g bytes=%zu/%zu\n",batch,rate,ratio,peak_error,packed_bytes,exact_bytes);
  printf("packet_hash=%016llx\n", (unsigned long long)packet_hash);
  free(repacked);free(packed);free(scratch);free(plan); dispose(reference); dispose(split);
}
static void maximum_pool(void) {
  double values[NG_CONFIG_COUNT], c[NG_CONTROL_COUNT];
  ng_defaults(values, c);
  values[NG_CONFIG_GRID_SIZE] = 16;
  values[NG_CONFIG_MAX_GRAINS] = 4096;
  NGConfig config; CHECK(!ng_config_parse(&config, values, NG_CONFIG_COUNT));
  size_t bytes = ng_grain_plan_size(&config, 32);
  CHECK(bytes > 4096u * 32u * sizeof(NGPlannedGrain));
  NGGrainPlan *plan = ng_grain_plan_init(malloc(bytes), bytes, &config, 32);
  Fixture f = make(&config, 4800, 1);
  CHECK(plan && ng_grain_plan_begin(f.engine, plan));
  for (unsigned i = 0; i < 32; ++i) CHECK(ng_grain_plan_sample(f.engine, plan));
  CHECK(!ng_grain_plan_sample(f.engine, plan));
  CHECK(ng_grain_plan_seal(f.engine, plan));
  double output[64];
  CHECK(ng_grain_plan_render(plan, output, 1024));
  /* Unusable complete output is muted per stereo frame, like ordinary DSP. */
  output[0] = INFINITY; output[7] = NAN;
  CHECK(ng_grain_plan_commit(f.engine, plan, output, 1024));
  CHECK(output[0] == 0 && output[1] == 0 && output[6] == 0 && output[7] == 0);
  CHECK(ng_counters(f.engine).numeric_interventions == 2);
  free(plan); dispose(f);
}
static void validation(void) {
  double values[NG_CONFIG_COUNT], c[NG_CONTROL_COUNT];
  ng_defaults(values, c);
  NGConfig config; CHECK(!ng_config_parse(&config, values, NG_CONFIG_COUNT));
  CHECK(!ng_grain_plan_size(NULL, 1));
  CHECK(!ng_grain_plan_size(&config, 0));
  CHECK(!ng_grain_plan_size(&config, 513));
  size_t bytes = ng_grain_plan_size(&config, 32);
  void *memory = malloc(bytes);
  CHECK(!ng_grain_plan_init(memory, bytes-1, &config, 32));
  CHECK(!ng_grain_plan_init(NULL, bytes, &config, 32));
  config.value[NG_CONFIG_MAX_GRAINS] = INFINITY;
  CHECK(!ng_grain_plan_size(&config, 32));
  free(memory);
}
int main(void) {
  validation();
  maximum_pool();
  unsigned cases = 0;
  const uint32_t batches[] = {1, 7, 32, 128, 512};
  for (unsigned b = 0; b < 5; ++b)
    for (unsigned mode = 0; mode < 4; ++mode) {
      compare(16, mode/2, mode%2, mode%2, 4800, mode%2 ? 2 : .5, batches[b]);
      ++cases;
    }
  compare(32, 0, 1, 1, 48000, 1, 32); ++cases;
  compare(64, 0, 0, 0, 96000, 1, 7); ++cases;
  printf("%u real-scheduler cases: exact PCM/stats, retry, ownership, clocks, reset, external stalls\n", cases);
  return 0;
}
