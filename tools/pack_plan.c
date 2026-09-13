/* Packing-only microbenchmark over real, streamed scheduler plans. No GPU,
 * audio convolution, or live-capacity claim. Compare builds sequentially. */
#define _POSIX_C_SOURCE 200809L
#include "naviergrain_pack.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static double now(void) {
  struct timespec t;
  REQUIRE(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void measure(unsigned capacity, unsigned frames) {
  double values[NG_CONFIG_COUNT], controls[NG_CONTROL_COUNT];
  ng_defaults(values, controls);
  values[NG_CONFIG_GRID_SIZE] = 16;
  values[NG_CONFIG_MAX_GRAINS] = capacity;
  values[NG_CONFIG_SOURCE_LOOP] = 1;
  NGConfig config;
  REQUIRE(!ng_config_parse(&config, values, NG_CONFIG_COUNT));
  size_t bytes = ng_memory_size(&config, 997, 48000, 48000);
  NGEngine *engine = ng_init(malloc(bytes), bytes, &config, 997, 48000, 48000);
  REQUIRE(engine);
  for (unsigned i = 0; i < 997; ++i) ng_source(engine)[i] = .3*sin((double)i);
  bytes = ng_grain_plan_size(&config, frames);
  NGGrainPlan *plan = ng_grain_plan_init(malloc(bytes), bytes, &config, frames);
  size_t packet_bytes = ng_pack_size_frames(capacity, frames);
  size_t scratch_bytes = ng_pack_scratch_size_frames(capacity, frames);
  void *packet = malloc(packet_bytes), *scratch = malloc(scratch_bytes);
  REQUIRE(plan && packet && scratch);
  controls[NG_CONTROL_GRAIN_RATE] = capacity == 128 ? 800 : 2000;
  controls[NG_CONTROL_GRAIN_MS] = 500;
  controls[NG_CONTROL_GAIN] = .15;
  ng_controls(engine, controls);
  double packing = 0, capture = 0;
  size_t transferred = 0, records = 0;
  uint64_t hash = UINT64_C(14695981039346656037);
  /* Commit silence only to release ownership. This leaves all scheduling,
   * particle, overlap and packing trajectories intact; playback peaks are
   * deliberately not measured. All work outside ng_pack is untimed for it. */
  for (unsigned first = 0; first < 49152; first += frames) {
    double start = now();
    REQUIRE(ng_grain_plan_begin(engine, plan));
    for (unsigned f = 0; f < frames; ++f) REQUIRE(ng_grain_plan_sample(engine, plan));
    REQUIRE(ng_grain_plan_seal(engine, plan));
    capture += now() - start;
    NGGrainPlanView view;
    REQUIRE(ng_grain_plan_view(plan, &view));
    records += view.grains;
    start = now();
    size_t used = ng_pack(plan, packet, packet_bytes, scratch, scratch_bytes);
    packing += now() - start;
    REQUIRE(used);
    transferred += used;
    for (size_t i = 0; i < used; ++i) {
      hash ^= ((const unsigned char *)packet)[i]; hash *= UINT64_C(1099511628211);
    }
    double output[2*NG_GRAIN_PLAN_MAX_FRAMES] = {0};
    REQUIRE(ng_grain_plan_commit(engine, plan, output, 2*frames));
  }
  printf("{\"capacity\":%u,\"frames\":%u,\"audioFrames\":49152,\"records\":%zu,"
         "\"packetBytes\":%zu,\"packetFnv1a64\":\"%016llx\",\"packingSeconds\":%.9g,\"captureSeconds\":%.9g}\n",
         capacity, frames, records, transferred, (unsigned long long)hash, packing, capture);
  free(scratch); free(packet); free(plan); free(engine);
}
int main(int argc, char **argv) {
  (void)argv;
  REQUIRE(argc <= 2);
  const unsigned frames[] = {32, 128, 512};
  for (unsigned i = 0; i < 6; ++i) {
    unsigned row = argc == 2 ? 5-i : i;
    measure(row % 2 ? 1024 : 128, frames[row/2]);
  }
  return 0;
}
