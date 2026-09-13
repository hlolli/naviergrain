#define _POSIX_C_SOURCE 200809L
#include "naviergrain_transport.h"
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static NGField make_field(unsigned n) {
  NGField f; double *storage = calloc(ng_field_doubles(n), sizeof(double));
  CHECK(storage); ng_field_init(&f, n, storage, 17); return f;
}
static void fill(NGField *f, uint64_t seq) {
  f->sequence = seq; f->time = (double)seq * .25;
  f->energy = 1; f->rms_omega = 2; f->rms_strain = 3;
  f->rms_divergence = 4; f->max_divergence = 5; f->max_speed = 6;
  f->interventions = 7;
  for (unsigned i = 0; i < f->n * f->n; ++i) {
    f->u[i] = (double)seq; f->v[i] = -(double)seq;
    f->omega[i] = i * .125 - 4; f->strain[i] = i * .0625;
  }
}
static void put32(unsigned char *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
static void reject_unchanged(const unsigned char *packet, size_t bytes,
                              NGField *out, NGPacketResult expected) {
  size_t size = ng_field_doubles(out->n) * sizeof(double);
  void *before = malloc(size); CHECK(before); memcpy(before, out->storage, size);
  NGField metadata = *out; NGPacketInfo info, old_info;
  memset(&info, 0xab, sizeof(info)); memcpy(&old_info, &info, sizeof(info));
  CHECK(ng_packet_decode(packet, bytes, out, &info) == expected);
  CHECK(!memcmp(before, out->storage, size));
  CHECK(!memcmp(&metadata, out, sizeof(metadata)));
  CHECK(!memcmp(&old_info, &info, sizeof(info)));
  free(before);
}
static void codec(void) {
  CHECK(!ng_packet_size(0) && !ng_packet_size(128) && !ng_packet_size(UINT32_MAX));
  for (unsigned n = 16; n <= 64; n *= 2) {
    NGField in = make_field(n), out = make_field(n);
    fill(&in, 19);
    size_t bytes = ng_packet_size(n);
    /* Deliberately unaligned wire buffer. */
    unsigned char *raw = malloc(bytes + 2), *packet = raw + 1;
    CHECK(raw); raw[0] = raw[bytes + 1] = 0xc7;
    CHECK(ng_packet_encode(packet, bytes, &in, 8, 9, UINT64_MAX) == NG_PACKET_OK);
    NGPacketInfo info;
    CHECK(ng_packet_decode(packet, bytes, &out, &info) == NG_PACKET_OK);
    CHECK(info.instance_id == 8 && info.epoch == 9 && info.audio_frame == UINT64_MAX);
    CHECK(info.sequence == 19 && info.n == n && info.interventions == 7);
    CHECK(!memcmp(in.u, out.u, 4u * n * n * sizeof(double)));
    CHECK(raw[0] == 0xc7 && raw[bytes + 1] == 0xc7);
    out.au[0] = 47; double phase = out.phases[0];
    CHECK(ng_packet_decode(packet, bytes, &out, &info) == NG_PACKET_OK);
    CHECK(out.au[0] == 47 && out.phases[0] == phase);
    for (size_t length = 0; length < bytes; ++length)
      CHECK(ng_packet_inspect(packet, length, &info) == NG_PACKET_MALFORMED);
    reject_unchanged(packet, 0, &out, NG_PACKET_MALFORMED);
    reject_unchanged(packet, NG_PACKET_HEADER - 1, &out, NG_PACKET_MALFORMED);
    reject_unchanged(packet, bytes - 1, &out, NG_PACKET_MALFORMED);
    reject_unchanged(packet, bytes + 1, &out, NG_PACKET_MALFORMED);
    /* Every header field, all reserved bytes, and representative payload words. */
    struct { size_t offset; uint32_t word; NGPacketResult result; } bad[] = {
      {0, 0, NG_PACKET_MALFORMED}, {4, 2, NG_PACKET_INCOMPATIBLE},
      {8, 124, NG_PACKET_MALFORMED}, {12, UINT32_MAX, NG_PACKET_MALFORMED},
      {56, 128, NG_PACKET_INCOMPATIBLE}, {60, 99, NG_PACKET_INCOMPATIBLE},
      {64, 0, NG_PACKET_INCOMPATIBLE}, {68, 2, NG_PACKET_INCOMPATIBLE},
      {72, 0xbf800000u, NG_PACKET_MALFORMED},
      {76, 0x7fc00000u, NG_PACKET_NONFINITE}, {80, 0x7f800000u, NG_PACKET_NONFINITE},
      {84, 0xff800000u, NG_PACKET_NONFINITE}, {88, 0x7fc00000u, NG_PACKET_NONFINITE},
      {92, 0xbf800000u, NG_PACKET_MALFORMED}, {52, 0x7ff00000u, NG_PACKET_NONFINITE},
      {52, 0xbff00000u, NG_PACKET_MALFORMED},
      {128, 0x7fc00000u, NG_PACKET_NONFINITE},
      {128 + 4u * n * n, 0x7f800000u, NG_PACKET_NONFINITE},
      {128 + 8u * n * n, 0xff800000u, NG_PACKET_NONFINITE},
      {bytes - 4, 0xbf800000u, NG_PACKET_MALFORMED},
      {bytes - 4, 0x7fc00000u, NG_PACKET_NONFINITE}
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      unsigned char saved[4]; memcpy(saved, packet + bad[i].offset, 4);
      put32(packet + bad[i].offset, bad[i].word);
      reject_unchanged(packet, bytes, &out, bad[i].result);
      memcpy(packet + bad[i].offset, saved, 4);
    }
    for (unsigned i = 104; i < NG_PACKET_HEADER; ++i) {
      packet[i] = 1; reject_unchanged(packet, bytes, &out, NG_PACKET_MALFORMED); packet[i] = 0;
    }
    for (unsigned i = 16; i <= 32; i += 8) {
      unsigned char saved[8]; memcpy(saved, packet + i, 8); memset(packet + i, 0, 8);
      reject_unchanged(packet, bytes, &out, NG_PACKET_MALFORMED);
      memcpy(packet + i, saved, 8);
    }
    in.u[0] = DBL_MAX;
    unsigned char *before = malloc(bytes); CHECK(before); memcpy(before, packet, bytes);
    CHECK(ng_packet_encode(packet, bytes, &in, 8, 9, 10) == NG_PACKET_NONFINITE);
    CHECK(!memcmp(before, packet, bytes));
    in.u[0] = NAN;
    CHECK(ng_packet_encode(packet, bytes, &in, 8, 9, 10) == NG_PACKET_NONFINITE);
    in.u[0] = 1; in.strain[n * n - 1] = -1;
    CHECK(ng_packet_encode(packet, bytes, &in, 8, 9, 10) == NG_PACKET_MALFORMED);
    CHECK(!memcmp(before, packet, bytes));
    free(before); free(raw); free(in.storage); free(out.storage);
  }
  /* Real solver field converts exactly once to binary32 then to core double. */
  NGField in = make_field(32), out = make_field(32);
  double controls[NG_CONTROL_COUNT];
  for (unsigned i = 0; i < NG_CONTROL_COUNT; ++i) controls[i] = ng_control_parameters[i].initial;
  for (unsigned i = 0; i < 8; ++i) CHECK(ng_field_step(&in, controls, 60, 240, 128, 20));
  unsigned char packet[NG_PACKET_HEADER + 16 * 32 * 32]; NGPacketInfo info;
  CHECK(ng_packet_encode(packet, sizeof(packet), &in, 1, 1, 6400) == NG_PACKET_OK);
  CHECK(ng_packet_decode(packet, sizeof(packet), &out, &info) == NG_PACKET_OK);
  double error = 0;
  for (unsigned i = 0; i < 4 * 32 * 32; ++i) {
    CHECK(out.u[i] == (double)(float)in.u[i]);
    error = fmax(error, fabs(out.u[i] - in.u[i]));
  }
  printf("solver float32 conversion max error %.9g\n", error);
  free(in.storage); free(out.storage);
}
static NGFieldQueue *make_queue(uint64_t id, uint64_t epoch) {
  void *memory = malloc(ng_field_queue_size()); CHECK(memory);
  NGFieldQueue *q = ng_field_queue_init(memory, ng_field_queue_size(), id, epoch, 16);
  CHECK(q); return q;
}
static NGPacketResult send(NGFieldQueue *q, NGField *in, uint64_t id,
                            uint64_t epoch, uint64_t sequence, uint64_t frame) {
  unsigned char packet[NG_PACKET_HEADER + 16 * 16 * 16];
  fill(in, sequence);
  CHECK(ng_packet_encode(packet, sizeof(packet), in, id, epoch, frame) == NG_PACKET_OK);
  NGPacketResult result = ng_field_queue_push(q, packet, sizeof(packet));
  memset(packet, 0x55, sizeof(packet)); /* Caller can immediately reuse its bytes. */
  return result;
}
static void queue_cases(void) {
  NGField in = make_field(16), out = make_field(16); NGPacketInfo info;
  NGFieldQueue *q = make_queue(8, 9);
  CHECK(ng_field_queue_consume(q, 0, &out, &info) == NG_PACKET_EMPTY);
  for (uint64_t seq = 1; seq <= 4; ++seq) CHECK(send(q, &in, 8, 9, seq, 100 * seq) == NG_PACKET_OK);
  CHECK(send(q, &in, 8, 9, 5, 500) == NG_PACKET_FULL);
  CHECK(ng_field_queue_consume(q, 0, &out, &info) == NG_PACKET_EMPTY);
  CHECK(ng_field_queue_consume(q, 250, &out, &info) == NG_PACKET_OK && info.sequence == 2);
  CHECK(send(q, &in, 8, 9, 5, 500) == NG_PACKET_OK); /* Reuses low slot before future 3/4. */
  CHECK(ng_field_queue_consume(q, 350, &out, &info) == NG_PACKET_OK && info.sequence == 3);
  CHECK(ng_field_queue_consume(q, 500, &out, &info) == NG_PACKET_OK && info.sequence == 5);
  CHECK(out.u[0] == 5 && out.v[255] == -5);
  CHECK(ng_field_queue_consume(q, 500, &out, &info) == NG_PACKET_EMPTY);
  CHECK(send(q, &in, 8, 9, 4, 600) == NG_PACKET_OK);
  CHECK(send(q, &in, 8, 9, 6, 400) == NG_PACKET_OK); /* New seq, backwards timestamp. */
  CHECK(send(q, &in, 88, 9, 6, 600) == NG_PACKET_OK);
  CHECK(send(q, &in, 8, 8, 6, 600) == NG_PACKET_OK);
  CHECK(ng_field_queue_consume(q, 600, &out, &info) == NG_PACKET_EMPTY);
  CHECK(out.sequence == 5);
  CHECK(ng_field_queue_consume(q, 599, &out, &info) == NG_PACKET_ORDER);
  CHECK(send(q, &in, 8, 9, 10, UINT64_MAX) == NG_PACKET_OK);
  CHECK(ng_field_queue_consume(q, 600, &out, &info) == NG_PACKET_EMPTY);
  CHECK(ng_field_queue_epoch(q, 10) == NG_PACKET_OK);
  CHECK(ng_field_queue_epoch(q, 10) == NG_PACKET_EPOCH);
  CHECK(ng_field_queue_epoch(q, 9) == NG_PACKET_EPOCH);
  CHECK(send(q, &in, 8, 9, 11, UINT64_MAX) == NG_PACKET_OK); /* Old producer in flight. */
  CHECK(send(q, &in, 8, 10, 1, 0) == NG_PACKET_OK);
  CHECK(ng_field_queue_consume(q, 0, &out, &info) == NG_PACKET_OK);
  CHECK(info.epoch == 10 && info.sequence == 1 && out.u[0] == 1);
  NGFieldQueueStats stats = ng_field_queue_stats(q);
  CHECK(stats.full == 1 && stats.applied == 4 && stats.superseded == 2);
  CHECK(stats.rejected[NG_PACKET_ORDER] == 3);
  CHECK(stats.rejected[NG_PACKET_INSTANCE] == 1 && stats.rejected[NG_PACKET_EPOCH] == 3);
  unsigned char packet[NG_PACKET_HEADER + 16 * 16 * 16];
  fill(&in, 2);
  CHECK(ng_packet_encode(packet, sizeof(packet), &in, 8, 10, 0) == NG_PACKET_OK);
  put32(packet + sizeof(packet) - 4, 0x7f800000u);
  CHECK(ng_field_queue_push(q, packet, sizeof(packet)) == NG_PACKET_OK);
  packet[0] = 0;
  CHECK(ng_field_queue_push(q, packet, sizeof(packet)) == NG_PACKET_OK);
  CHECK(ng_field_queue_push(q, packet, NG_PACKET_MAX_BYTES + 1) == NG_PACKET_MALFORMED);
  CHECK(ng_field_queue_consume(q, 0, &out, &info) == NG_PACKET_EMPTY);
  CHECK(out.sequence == 1);
  stats = ng_field_queue_stats(q);
  CHECK(stats.bad_size == 1 && stats.rejected[NG_PACKET_MALFORMED] == 1 &&
        stats.rejected[NG_PACKET_NONFINITE] == 1);
  /* Separate bindings cannot consume another instance's packet. */
  NGFieldQueue *other = make_queue(88, 10);
  CHECK(send(other, &in, 8, 10, 2, 0) == NG_PACKET_OK);
  CHECK(ng_field_queue_consume(other, 0, &out, &info) == NG_PACKET_EMPTY);
  CHECK(ng_field_queue_stats(other).rejected[NG_PACKET_INSTANCE] == 1);
  free(other); free(q); free(in.storage); free(out.storage);
}
#define ITERATIONS 30000u
typedef struct { NGFieldQueue *queue; atomic_int done; } Stress;
static void *producer(void *arg) {
  Stress *s = arg; NGField in = make_field(16);
  unsigned char packet[NG_PACKET_HEADER + 16 * 16 * 16];
  for (uint64_t seq = 1; seq <= ITERATIONS; ++seq) {
    fill(&in, seq);
    CHECK(ng_packet_encode(packet, sizeof(packet), &in, 8, 9, seq) == NG_PACKET_OK);
    NGPacketResult result;
    do {
      result = ng_field_queue_push(s->queue, packet, sizeof(packet));
      CHECK(result == NG_PACKET_OK || result == NG_PACKET_FULL);
      if (result == NG_PACKET_FULL) sched_yield();
    } while (result == NG_PACKET_FULL);
    memset(packet, 0xa5, sizeof(packet));
    if (seq % 29 == 0) sched_yield();
  }
  free(in.storage); atomic_store_explicit(&s->done, 1, memory_order_release); return NULL;
}
static void stress(void) {
  Stress s; s.queue = make_queue(8, 9); atomic_init(&s.done, 0);
  pthread_t thread; CHECK(!pthread_create(&thread, NULL, producer, &s));
  NGField out = make_field(16); NGPacketInfo info; uint64_t previous = 0;
  for (;;) {
    int done = atomic_load_explicit(&s.done, memory_order_acquire);
    NGPacketResult result = ng_field_queue_consume(s.queue, UINT64_MAX, &out, &info);
    if (result == NG_PACKET_EMPTY) { if (done) break; sched_yield(); continue; }
    CHECK(result == NG_PACKET_OK && info.sequence > previous);
    previous = info.sequence;
    CHECK(out.time == (double)previous * .25 && info.audio_frame == previous);
    for (unsigned i = 0; i < 256; ++i)
      CHECK(out.u[i] == (double)previous && out.v[i] == -(double)previous &&
            out.omega[i] == i * .125 - 4 && out.strain[i] == i * .0625);
  }
  CHECK(!pthread_join(thread, NULL)); CHECK(previous == ITERATIONS);
  NGFieldQueueStats stats = ng_field_queue_stats(s.queue);
  CHECK(stats.published == ITERATIONS);
  CHECK(stats.applied + stats.superseded == ITERATIONS);
  for (unsigned i = 0; i < NG_PACKET_RESULT_COUNT; ++i)
    CHECK(stats.rejected[i] == 0);
  printf("concurrency: %u publications, %llu applied, %llu superseded, %llu obsolete, %llu full retries\n",
         ITERATIONS, (unsigned long long)stats.applied, (unsigned long long)stats.superseded,
         (unsigned long long)stats.rejected[NG_PACKET_ORDER], (unsigned long long)stats.full);
  free(out.storage); free(s.queue);
}
static void reset_stress(void) {
  Stress s; s.queue = make_queue(8, 9); atomic_init(&s.done, 0);
  pthread_t thread; CHECK(!pthread_create(&thread, NULL, producer, &s));
  NGField out = make_field(16); NGPacketInfo info; unsigned calls = 0;
  uint64_t epoch = 9;
  for (;;) {
    int done = atomic_load_explicit(&s.done, memory_order_acquire);
    if (++calls % 7 == 0) CHECK(ng_field_queue_epoch(s.queue, ++epoch) == NG_PACKET_OK);
    NGPacketResult result = ng_field_queue_consume(s.queue, UINT64_MAX, &out, &info);
    CHECK(result == NG_PACKET_EMPTY || result == NG_PACKET_OK);
    if (result == NG_PACKET_OK) CHECK(info.epoch == epoch);
    if (done && result == NG_PACKET_EMPTY) break;
  }
  CHECK(!pthread_join(thread, NULL));
  NGFieldQueueStats stats = ng_field_queue_stats(s.queue);
  CHECK(stats.published == ITERATIONS && epoch > 9 && stats.rejected[NG_PACKET_EPOCH]);
  CHECK(stats.applied + stats.superseded + stats.rejected[NG_PACKET_EPOCH] == ITERATIONS);
  /* Rebind the producer clock after reset and prove queue reuse. */
  CHECK(ng_field_queue_epoch(s.queue, ++epoch) == NG_PACKET_OK);
  NGField in = make_field(16);
  CHECK(send(s.queue, &in, 8, epoch, 1, 0) == NG_PACKET_OK);
  CHECK(ng_field_queue_consume(s.queue, 0, &out, &info) == NG_PACKET_OK);
  CHECK(info.sequence == 1 && info.epoch == epoch);
  free(in.storage); free(out.storage); free(s.queue);
  puts("concurrent reset: old in-flight publications rejected; new epoch recovered");
}
static void wire(const char *path) {
  unsigned char packet[NG_PACKET_HEADER + 16 * 16 * 16], encoded[sizeof(packet)];
  FILE *file = fopen(path, "rb"); CHECK(file);
  CHECK(fread(packet, 1, sizeof(packet), file) == sizeof(packet));
  CHECK(fgetc(file) == EOF); CHECK(!fclose(file));
  NGField out = make_field(16); NGPacketInfo info;
  CHECK(ng_packet_decode(packet, sizeof(packet), &out, &info) == NG_PACKET_OK);
  CHECK(info.instance_id == UINT64_C(0xfedcba9876543210));
  CHECK(info.epoch == UINT64_C(0x1020304050607080));
  CHECK(info.sequence == UINT64_C(0x20000000000001));
  CHECK(info.audio_frame == UINT64_C(0xf123456789abcdef));
  CHECK(info.interventions == UINT64_C(0x8877665544332211));
  CHECK(out.time == 1.25 && out.energy == 1 && out.rms_omega == 2 &&
        out.rms_strain == 3 && out.rms_divergence == 4 && out.max_divergence == 5 &&
        out.max_speed == 6);
  for (unsigned a = 0; a < 4; ++a)
    for (unsigned i = 0; i < 256; ++i)
      CHECK(out.u[a * 256 + i] == (a == 1 ? -1 : 1) * ((double)a + i * .125));
  CHECK(ng_packet_encode(encoded, sizeof(encoded), &out, info.instance_id,
        info.epoch, info.audio_frame) == NG_PACKET_OK);
  CHECK(!memcmp(packet, encoded, sizeof(packet)));
  free(out.storage); puts("independent Python/C wire fixture: exact bytes and uint64 words passed");
}
static double now(void) {
  struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static int compare_time(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
static void benchmark(void) {
  enum { RUNS = 512 };
  double timer_total = 0;
  for (unsigned i = 0; i < RUNS; ++i) {
    double begin = now(); timer_total += now() - begin;
  }
  printf("{\"queue_bytes\":%zu,\"runs_per_case\":%d,\"timer_pair_mean_us\":%.6f,\"cases\":[",
         ng_field_queue_size(), RUNS, timer_total * 1e6 / RUNS);
  for (unsigned n = 16; n <= 64; n *= 2) {
    for (unsigned slots = 1; slots <= 4; slots += 3) {
      NGField in = make_field(n), out = make_field(n); fill(&in, 1);
      void *memory = malloc(ng_field_queue_size()); CHECK(memory);
      NGFieldQueue *q = ng_field_queue_init(memory, ng_field_queue_size(), 8, 9, n); CHECK(q);
      size_t bytes = ng_packet_size(n); unsigned char *packet = malloc(bytes); CHECK(packet);
      double timings[RUNS], push_total = 0, consume_total = 0; NGPacketInfo info;
      uint64_t seq = 0;
      for (unsigned run = 0; run < RUNS; ++run) {
        for (unsigned s = 0; s < slots; ++s) {
          in.sequence = ++seq; in.time = (double)seq / 60;
          CHECK(ng_packet_encode(packet, bytes, &in, 8, 9, seq) == NG_PACKET_OK);
          double begin = now();
          CHECK(ng_field_queue_push(q, packet, bytes) == NG_PACKET_OK);
          push_total += now() - begin;
        }
        double begin = now();
        CHECK(ng_field_queue_consume(q, UINT64_MAX, &out, &info) == NG_PACKET_OK);
        timings[run] = (now() - begin) * 1e6; consume_total += timings[run];
        CHECK(info.sequence == seq);
      }
      qsort(timings, RUNS, sizeof(double), compare_time);
      printf("%s{\"grid\":%u,\"ready_slots\":%u,\"packet_bytes\":%zu,"
             "\"push_mean_us\":%.6f,\"consume_mean_us\":%.6f,"
             "\"consume_p99_us\":%.6f,\"consume_max_us\":%.6f}",
             n == 16 && slots == 1 ? "" : ",", n, slots, bytes,
             push_total * 1e6 / (RUNS * slots), consume_total / RUNS,
             timings[506], timings[RUNS - 1]);
      free(packet); free(memory); free(in.storage); free(out.storage);
    }
  }
  puts("]}");
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--benchmark")) { benchmark(); return 0; }
  if (argc == 2) { wire(argv[1]); return 0; }
  codec(); queue_cases(); stress(); reset_stress();
  puts("transport: codec, rejection atomicity, queue pressure/future/reset/multi-instance and concurrency passed");
}
