#define _POSIX_C_SOURCE 200809L
#include "naviergrain_provider.h"
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct {
  NGConfig config;
  NGEngine *engine;
  NGProvider *provider;
  double controls[NG_CONTROL_COUNT];
} Fixture;
static Fixture make(unsigned n, uint64_t key) {
  Fixture f;
  double values[NG_CONFIG_COUNT];
  ng_defaults(values, f.controls);
  values[NG_CONFIG_GRID_SIZE] = n;
  values[NG_CONFIG_BACKEND] = 1;
  values[NG_CONFIG_INSTANCE_ID] = (double)key;
  values[NG_CONFIG_EMITTER_COUNT] = 32;
  values[NG_CONFIG_MAX_GRAINS] = 128;
  values[NG_CONFIG_SOURCE_LOOP] = 1;
  CHECK(!ng_config_parse(&f.config, values, NG_CONFIG_COUNT));
  size_t bytes = ng_memory_size(&f.config, 997, 4800, 4800);
  f.engine = ng_init(malloc(bytes), bytes, &f.config, 997, 4800, 4800);
  CHECK(f.engine);
  bytes = ng_provider_size(&f.config);
  f.provider = ng_provider_init(malloc(bytes), bytes, &f.config, 4800, 17);
  CHECK(f.provider);
  for (unsigned i = 0; i < 997; ++i)
    ng_source(f.engine)[i] = .3 * sin(6.283185307179586 * i / 37);
  f.controls[NG_CONTROL_SCHEDULER] = 0;
  ng_controls(f.engine, f.controls);
  return f;
}
static void dispose(Fixture *f) {
  ng_provider_release(f->provider);
  free(f->provider); free(f->engine);
}
static double samples(Fixture *f, unsigned count, int work) {
  double energy = 0;
  for (unsigned i = 0; i < count; ++i) {
    double l, r;
    ng_sample(f->engine, &l, &r);
    CHECK(isfinite(l) && isfinite(r));
    energy += l*l + r*r;
    if (work) CHECK(ng_provider_work(f->provider) >= 0);
  }
  return energy;
}
static unsigned status(Fixture *f) {
  double s[NG_STAT_COUNT];
  ng_stats(f->engine, s);
  return (unsigned)s[NG_STAT_STATUS];
}
static void inject(Fixture *f, uint64_t epoch, uint64_t sequence,
                    uint64_t frame, double velocity) {
  double storage[16*16*NG_FIELD_ARRAYS];
  NGField field;
  ng_field_init(&field, 16, storage, 0);
  field.sequence = sequence;
  /* Deliberately false header speed bound, consumer derives actual bound. */
  field.max_speed = 0;
  for (unsigned i = 0; i < 256; ++i) field.u[i] = velocity;
  unsigned char packet[NG_PACKET_HEADER + 16*16*16];
  CHECK(ng_packet_encode(packet, sizeof(packet), &field,
                         ng_provider_key(f->provider), epoch, frame) == NG_PACKET_OK);
  CHECK(naviergrain_push_field(f->provider, packet, sizeof(packet)) == NG_PACKET_OK);
}
static double velocity(Fixture *f) {
  double view[NG_VIEW_SIZE];
  CHECK(ng_view(f->engine, view, NG_VIEW_SIZE));
  return view[NG_VIEW_HEADER];
}
static void transitions(void) {
  Fixture f = make(16, 1);
  CHECK(status(&f) & NG_STATUS_NOT_PREPARED);
  CHECK(samples(&f, 200, 0) == 0);
  CHECK(ng_provider_attach(f.provider, f.engine));
  CHECK(!ng_provider_attach(f.provider, f.engine));
  inject(&f, 17, 1, 0, .5);
  samples(&f, 20, 0);
  CHECK(velocity(&f) == 0); /* first accepted field starts at old field */
  samples(&f, 20, 0);
  CHECK(fabs(velocity(&f) - .125) < 1e-12);
  samples(&f, 60, 0);
  CHECK(fabs(velocity(&f) - .5) < 1e-12);
  CHECK(!(status(&f) & NG_STATUS_STALE_FIELD));
  samples(&f, 400, 0);
  CHECK(status(&f) & NG_STATUS_STALE_FIELD);
  CHECK(samples(&f, 1400, 0) > 0); /* playback survives lost provider */
  CHECK(status(&f) & NG_STATUS_PROVIDER_LOST);
  CHECK(ng_provider_stats(f.provider).motion_gain < 1e-10);
  CHECK(ng_provider_stats(f.provider).command_drops > 0);
  /* Late arrival has an old target: it cannot masquerade as fresh. */
  inject(&f, 17, 2, 1, .25);
  samples(&f, 20, 0);
  CHECK(status(&f) & NG_STATUS_STALE_FIELD);
  inject(&f, 17, 3, 1920, .25);
  samples(&f, 200, 0);
  CHECK(!(status(&f) & NG_STATUS_STALE_FIELD));
  CHECK(ng_provider_stats(f.provider).motion_gain > .9);
  /* Finite maliciously large field is rejected, never reaches particles. */
  double before = velocity(&f);
  inject(&f, 17, 4, 2120, 1e20);
  samples(&f, 100, 0);
  CHECK(ng_provider_stats(f.provider).unsafe_fields == 1);
  CHECK(velocity(&f) == before);
  f.controls[NG_CONTROL_FREEZE] = 1;
  ng_controls(f.engine, f.controls);
  before = velocity(&f);
  CHECK(samples(&f, 2400, 0) > 0);
  CHECK(velocity(&f) == before);
  CHECK((status(&f) & NG_STATUS_FROZEN) &&
        !(status(&f) & (NG_STATUS_PROVIDER_LOST | NG_STATUS_STALE_FIELD)));
  f.controls[NG_CONTROL_FREEZE] = 0;
  ng_controls(f.engine, f.controls);
  samples(&f, 1, 0);
  CHECK(ng_provider_stats(f.provider).epoch == 18);
  /* Obsolete full command queue drains; later full-state command recovers. */
  CHECK(samples(&f, 1000, 1) > 0);
  CHECK(ng_provider_stats(f.provider).obsolete == 4);
  CHECK(ng_provider_stats(f.provider).applied > 3);
  CHECK(!(status(&f) & NG_STATUS_STALE_FIELD));
  double simulation_before_resume = ng_provider_stats(f.provider).worker_simulation_time;
  CHECK(ng_provider_resume(f.provider));
  samples(&f, 200, 1);
  CHECK(ng_provider_stats(f.provider).worker_simulation_time > simulation_before_resume);
  CHECK(ng_provider_stats(f.provider).epoch == 19);
  f.controls[NG_CONTROL_RESET] = 1;
  ng_controls(f.engine, f.controls);
  samples(&f, 1500, 1);
  CHECK(ng_counters(f.engine).epoch == 1);
  CHECK(ng_provider_stats(f.provider).epoch == 20);
  CHECK(!(status(&f) & NG_STATUS_PROVIDER_LOST));
  dispose(&f);
}
static void grids(void) {
  for (unsigned n = 16; n <= 64; n *= 2) {
    Fixture a = make(n, n), b = make(n, n + 1);
    CHECK(!ng_provider_attach(a.provider, b.engine));
    CHECK(ng_provider_attach(a.provider, a.engine));
    CHECK(ng_provider_attach(b.provider, b.engine));
    double difference = 0;
    for (unsigned i = 0; i < 2400; ++i) {
      /* Identical audio-clock worker service; UI/host block grouping irrelevant. */
      double al, ar, bl, br;
      ng_sample(a.engine, &al, &ar); ng_sample(b.engine, &bl, &br);
      CHECK(isfinite(al) && isfinite(ar));
      CHECK(al == bl && ar == br);
      CHECK(ng_provider_work(a.provider) >= 0);
      CHECK(ng_provider_work(b.provider) >= 0);
      difference += al*al + ar*ar;
    }
    NGProviderStats s = ng_provider_stats(a.provider);
    CHECK(difference > 0 && s.applied > 20 && s.command_drops == 0 &&
          s.publish_failures == 0 && s.unsafe_fields == 0);
    printf("grid %u: %llu fields, exact independent-instance audio\n",
           n, (unsigned long long)s.applied);
    dispose(&a); dispose(&b);
  }
}
static void reset_overflow(void) {
  Fixture f = make(16, 94);
  CHECK(ng_provider_attach(f.provider, f.engine));
  samples(&f, 1000, 0);
  CHECK(ng_provider_stats(f.provider).command_drops > 0);
  f.controls[NG_CONTROL_RESET] = 1;
  f.controls[NG_CONTROL_DRIVE] = 1.5;
  ng_controls(f.engine, f.controls);
  samples(&f, 1500, 0);
  CHECK(ng_provider_stats(f.provider).epoch == 18);
  /* The worker never saw reset=1 as an edge; epoch and full state suffice. */
  samples(&f, 1500, 1);
  NGProviderStats stats = ng_provider_stats(f.provider);
  CHECK(stats.obsolete == 4 && stats.processed > 10 && stats.applied > 10);
  CHECK(!(status(&f) & NG_STATUS_PROVIDER_LOST));
  dispose(&f);
}
static void fixed_mapping(void) {
  Fixture external = make(16, 95);
  NGConfig c = external.config;
  c.value[NG_CONFIG_BACKEND] = 0;
  size_t bytes = ng_memory_size(&c, 997, 4800, 4800);
  NGEngine *internal = ng_init(malloc(bytes), bytes, &c, 997, 4800, 4800);
  CHECK(internal);
  memcpy(ng_source(internal), ng_source(external.engine), 997*sizeof(double));
  external.controls[NG_CONTROL_MAPPING_MIX] = 0;
  ng_controls(internal, external.controls);
  /* Use a fresh engine so both sides initialize their smoothing identically. */
  free(external.engine);
  bytes = ng_memory_size(&external.config, 997, 4800, 4800);
  external.engine = ng_init(malloc(bytes), bytes, &external.config, 997, 4800, 4800);
  CHECK(external.engine);
  memcpy(ng_source(external.engine), ng_source(internal), 997*sizeof(double));
  ng_controls(external.engine, external.controls);
  CHECK(ng_provider_attach(external.provider, external.engine));
  for (unsigned i = 0; i < 2400; ++i) {
    double al, ar, bl, br;
    ng_sample(internal, &al, &ar);
    ng_sample(external.engine, &bl, &br);
    CHECK(al == bl && ar == br);
    CHECK(ng_provider_work(external.provider) >= 0);
  }
  free(internal); dispose(&external);
  puts("mapping_mix=0: internal and worker audio identical");
}
typedef struct { NGProvider *provider; atomic_int stop; } Worker;
static void *run_worker(void *context) {
  Worker *w = context;
  while (!atomic_load_explicit(&w->stop, memory_order_acquire)) {
    int result = ng_provider_work(w->provider);
    if (!result) sched_yield();
  }
  return NULL;
}
static void concurrent(void) {
  Fixture f = make(16, 75);
  CHECK(ng_provider_attach(f.provider, f.engine));
  Worker w = {.provider = f.provider};
  atomic_init(&w.stop, 0);
  pthread_t thread;
  CHECK(!pthread_create(&thread, NULL, run_worker, &w));
  double energy = 0;
  for (unsigned i = 0; i < 30000; ++i) {
    if (i % 1000 == 0) {
      f.controls[NG_CONTROL_RESET] = i % 4000 == 1000;
      f.controls[NG_CONTROL_FREEZE] = i % 4000 == 2000;
      f.controls[NG_CONTROL_DRIVE] = i % 4000 < 2000 ? .2 : .8;
      ng_controls(f.engine, f.controls);
    }
    if (i == 15555) CHECK(ng_provider_resume(f.provider));
    energy += samples(&f, 1, 0);
    if (i % 64 == 0) sched_yield();
  }
  atomic_store_explicit(&w.stop, 1, memory_order_release);
  CHECK(!pthread_join(thread, NULL));
  NGProviderStats s = ng_provider_stats(f.provider);
  CHECK(energy > 0 && s.processed > 0 && s.applied > 0 && s.epoch > 20);
  printf("threaded audio/reset/resume: %llu applied, %llu command drops, epoch %llu\n",
    (unsigned long long)s.applied, (unsigned long long)s.command_drops,
    (unsigned long long)s.epoch);
  dispose(&f);
}
int main(void) {
  transitions(); grids(); reset_overflow(); fixed_mapping(); concurrent();
  puts("provider binding, blending, timestamp staleness, freeze/reset/recovery pass");
  return 0;
}
