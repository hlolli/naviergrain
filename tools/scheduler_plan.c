/* Offline real-scheduler split probe. Optional raw interleaved float64 output.
 * Run without competing benchmarks. Times are diagnostic, not live capacity. */
#define _POSIX_C_SOURCE 200809L
#include "fluidgrain_grain_plan.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)
static double now(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time)) abort();
  return (double)time.tv_sec + (double)time.tv_nsec * 1e-9;
}
int main(int argc, char **argv) {
  REQUIRE(argc <= 2);
  FILE *audio = argc == 2 ? fopen(argv[1], "wb") : NULL;
  REQUIRE(argc != 2 || audio);
  double values[FG_CONFIG_COUNT], controls[FG_CONTROL_COUNT];
  fg_defaults(values, controls);
  values[FG_CONFIG_MAX_GRAINS] = 1024;
  values[FG_CONFIG_SOURCE_LOOP] = 1;
  FGConfig config;
  REQUIRE(!fg_config_parse(&config, values, FG_CONFIG_COUNT));
  size_t bytes = fg_memory_size(&config, 997, 48000, 48000);
  FGEngine *engine = fg_init(malloc(bytes), bytes, &config, 997, 48000, 48000);
  REQUIRE(engine);
  for (unsigned i = 0; i < 997; ++i)
    fg_source(engine)[i] = .3*sin(6.283185307179586*i/37.0) + .1*cos(6.283185307179586*i/11.0);
  size_t plan_bytes = fg_grain_plan_size(&config, 32);
  FGGrainPlan *plan = fg_grain_plan_init(malloc(plan_bytes), plan_bytes, &config, 32);
  REQUIRE(plan);
  controls[FG_CONTROL_GRAIN_RATE] = 2000;
  controls[FG_CONTROL_GRAIN_MS] = 500;
  controls[FG_CONTROL_SPEED_TO_DENSITY] = 0;
  controls[FG_CONTROL_STRAIN_TO_DURATION] = 0;
  fg_controls(engine, controls);
  double planning = 0, rendering = 0, committing = 0, energy = 0, peak = 0;
  uint32_t max_live = 0;
  size_t records = 0;
  for (unsigned frame = 0; frame < 48000; frame += 32) {
    double start = now();
    REQUIRE(fg_grain_plan_begin(engine, plan));
    for (unsigned i = 0; i < 32; ++i) REQUIRE(fg_grain_plan_sample(engine, plan));
    REQUIRE(fg_grain_plan_seal(engine, plan));
    planning += now() - start;
    FGGrainPlanView view;
    REQUIRE(fg_grain_plan_view(plan, &view));
    records += view.grains;
    for (unsigned i = 0; i < 32; ++i)
      if (view.frame[i].count > max_live) max_live = view.frame[i].count;
    double output[64];
    start = now();
    REQUIRE(fg_grain_plan_render(plan, output, 64));
    rendering += now() - start;
    start = now();
    REQUIRE(fg_grain_plan_commit(engine, plan, output, 64));
    committing += now() - start;
    for (unsigned i = 0; i < 64; ++i) {
      REQUIRE(isfinite(output[i]));
      energy += output[i]*output[i];
      peak = fmax(peak, fabs(output[i]));
    }
    if (audio) REQUIRE(fwrite(output, sizeof(double), 64, audio) == 64);
  }
  FGCounters counts = fg_counters(engine);
  REQUIRE(energy > 0 && counts.numeric_interventions == 0);
  if (audio) REQUIRE(fclose(audio) == 0);
  printf("{\"scope\":\"offline native real scheduler, not a GPU or live benchmark\","
      "\"frames\":48000,\"sampleRate\":48000,\"batchFrames\":32,"
      "\"planBytes\":%zu,\"grainRecords\":%zu,\"recordBytes\":%zu,"
      "\"peakGrains\":%u,\"births\":%llu,\"voiceDrops\":%llu,"
      "\"planningSeconds\":%.9g,\"renderSeconds\":%.9g,"
      "\"commitSeconds\":%.9g,\"peak\":%.9g,\"rms\":%.9g}\n",
      plan_bytes, records, sizeof(FGPlannedGrain), max_live,
      (unsigned long long)counts.births, (unsigned long long)counts.voice_drops,
      planning, rendering, committing, peak, sqrt(energy/96000));
  free(plan); free(engine);
  return 0;
}
