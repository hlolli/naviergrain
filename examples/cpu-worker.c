/* Prepared native CPU worker host + lifecycle regression. No audio device:
 * callback timing is measured, but sleeps/pacing, WAV copies and I/O are host
 * work outside callbacks. Run from the repository root:
 * build/fluidgrain_worker_host build/libfluidgrain.so [output.wav]
 */
#define _POSIX_C_SOURCE 200809L
#include <csound.h>
#include "fluidgrain_provider.h"
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct {
  const FGNativeAPI *api;
  FGProvider *provider;
  atomic_int stop, pause;
} Worker;
static void *run(void *context) {
  Worker *w = context;
  const struct timespec nap = {.tv_nsec = 100000};
  while (!atomic_load_explicit(&w->stop, memory_order_acquire)) {
    if (atomic_load_explicit(&w->pause, memory_order_relaxed) ||
        !w->api->work(w->provider))
      nanosleep(&nap, NULL);
  }
  return NULL;
}
static double clock_ms(void) {
  struct timespec t;
  CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
  return 1000 * (double)t.tv_sec + 1e-6 * (double)t.tv_nsec;
}
static double channel(CSOUND *host, const char *name) {
  int32_t error;
  double value = csoundGetControlChannel(host, name, &error);
  CHECK(!error && isfinite(value));
  return value;
}
static void options(CSOUND *host, const char *module) {
  char option[4096];
  CHECK(snprintf(option, sizeof(option), "--opcode-lib=%s", module) < (int)sizeof(option));
  CHECK(!csoundSetOption(host, option));
}
static void u32(FILE *file, uint32_t value) {
  unsigned char bytes[4];
  for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (8*i));
  CHECK(fwrite(bytes, 1, 4, file) == 4);
}
static void wav(const char *path, const float *audio, unsigned frames) {
  FILE *file = fopen(path, "wb"); CHECK(file);
  CHECK(fwrite("RIFF", 1, 4, file) == 4); u32(file, 36 + 8*frames);
  CHECK(fwrite("WAVEfmt ", 1, 8, file) == 8); u32(file, 16);
  u32(file, 0x00020003); /* float32, stereo */
  u32(file, 48000); u32(file, 48000*8); u32(file, 0x00200008);
  CHECK(fwrite("data", 1, 4, file) == 4); u32(file, 8*frames);
  for (unsigned i = 0; i < frames*2; ++i) {
    uint32_t bits; memcpy(&bits, audio + i, 4); u32(file, bits);
  }
  CHECK(!fclose(file));
}
int main(int argc, char **argv) {
  CHECK(argc == 2 || argc == 3);
  CHECK(!csoundInitialize(CSOUNDINIT_NO_SIGNAL_HANDLER | CSOUNDINIT_NO_ATEXIT));
  void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!module) { fprintf(stderr, "%s\n", dlerror()); return 1; }
  const FGNativeAPI *(*get_api)(void);
  void *symbol = dlsym(module, "fluidgrain_native_api");
  CHECK(symbol && sizeof(symbol) == sizeof(get_api));
  memcpy(&get_api, &symbol, sizeof(get_api));
  const FGNativeAPI *api = get_api();
  CHECK(api && api->version == 1 && dlsym(module, "fluidgrain_push_field"));
  FGConfig config;
  for (unsigned i = 0; i < FG_CONFIG_COUNT; ++i)
    config.value[i] = fg_config_parameters[i].initial;
  config.value[FG_CONFIG_GRID_SIZE] = 16;
  config.value[FG_CONFIG_BACKEND] = 1;
  config.value[FG_CONFIG_INSTANCE_ID] = 1;
  size_t bytes = api->size(&config);
  CHECK(bytes);
  void *memory = malloc(bytes), *other_memory = malloc(bytes);
  CHECK(memory && other_memory);
  FGProvider *provider = api->prepare(memory, bytes, &config, 48000, 100);
  FGProvider *other = api->prepare(other_memory, bytes, &config, 48000, 200);
  CHECK(provider && other);
  CSOUND *host = csoundCreate(NULL, NULL), *second = csoundCreate(NULL, NULL);
  CHECK(host && second);
  options(host, argv[1]); options(second, argv[1]);
  CHECK(!csoundCompileCSD(host, "examples/cpu-worker.csd", 0, 0));
  CHECK(!csoundCompileCSD(second, "examples/cpu-worker.csd", 0, 0));
  CHECK(api->register_provider(host, provider));
  CHECK(!api->register_provider(host, other)); /* duplicate key */
  CHECK(api->register_provider(second, other)); /* per-CSOUND isolation */
  CHECK(!csoundStart(host) && !csoundStart(second));
  csoundSetControlChannel(host, "fg.drive", .5);
  csoundSetControlChannel(second, "fg.drive", .5);
  CHECK(!csoundPerformKsmps(host) && !csoundPerformKsmps(second));
  CHECK(channel(host, "fg.status") == FG_STATUS_READY);
  CHECK(channel(host, "fg.backend") == 1 && channel(second, "fg.backend") == 1);
  for (unsigned i = 0; i < 128; ++i) CHECK(csoundGetSpout(host)[i] == 0);
  CHECK(!api->unregister_provider(host, provider)); /* live claim */
  unsigned blocks = argc == 3 ? 4500 : 1600;
  float *audio = calloc((size_t)blocks * 128, sizeof(float));
  CHECK(audio);
  Worker worker = {.api = api, .provider = provider};
  atomic_init(&worker.stop, 0); atomic_init(&worker.pause, 0);
  pthread_t thread;
  CHECK(!pthread_create(&thread, NULL, run, &worker));
  csoundSetControlChannel(host, "fg.run", 1);
  double total = 0, maximum = 0, peak = 0, energy = 0;
  int lost = 0, recovered = 0, frozen = 0;
  const struct timespec period = {.tv_nsec = 1333333};
  for (unsigned block = 0; block < blocks; ++block) {
    atomic_store_explicit(&worker.pause, block >= 150 && block < 500,
                           memory_order_relaxed);
    csoundSetControlChannel(host, "fg.freeze", block >= 550 && block < 800);
    csoundSetControlChannel(host, "fg.reset", block >= 1000 && block < 1020);
    csoundSetControlChannel(host, "fg.drive", block < 1100 ? .5 : 1.2);
    if (block == 840) CHECK(api->resume(provider));
    double start = clock_ms();
    CHECK(!csoundPerformKsmps(host));
    double elapsed = clock_ms() - start;
    total += elapsed; maximum = fmax(maximum, elapsed);
    const MYFLT *out = csoundGetSpout(host);
    for (unsigned i = 0; i < 128; ++i) {
      CHECK(isfinite(out[i]));
      audio[block*128+i] = (float)out[i];
      peak = fmax(peak, fabs(out[i])); energy += out[i]*out[i];
    }
    unsigned state = (unsigned)channel(host, "fg.status");
    if (block >= 450 && block < 500) lost |= !!(state & FG_STATUS_PROVIDER_LOST);
    if (block > 530 && block < 550) recovered |= !(state & FG_STATUS_STALE_FIELD);
    if (block == 790) {
      CHECK((state & FG_STATUS_FROZEN) &&
            !(state & (FG_STATUS_STALE_FIELD | FG_STATUS_PROVIDER_LOST)));
      frozen = 1;
    }
    nanosleep(&period, NULL); /* host pacing, NOT inside audio callback */
  }
  atomic_store_explicit(&worker.stop, 1, memory_order_release);
  CHECK(!pthread_join(thread, NULL));
  FGProviderStats stats = api->stats(provider);
  CHECK(energy > 0 && peak < 1 && lost && recovered && frozen);
  CHECK(stats.applied > 20 && stats.epoch >= 103 && stats.unsafe_fields == 0);
  CHECK(api->stats(other).commands == 0 && api->stats(other).applied == 0);
  printf("CPU worker: %llu fields, epoch %llu, command drops %llu, "
    "publish failures %llu, peak %.6f, callback mean/max %.6f/%.6f ms\n",
    (unsigned long long)stats.applied, (unsigned long long)stats.epoch,
    (unsigned long long)stats.command_drops,
    (unsigned long long)stats.publish_failures, peak, total/blocks, maximum);
  printf("provider bytes: %zu; control-to-applied-field mean/max %.6f/%.6f ms (includes stall)\n",
    bytes, stats.mean_latency_ms, stats.max_latency_ms);
  if (argc == 3) wav(argv[2], audio, blocks*64);
  /* No worker sees Csound or source/audio storage. Join before either owner
   * can be destroyed. Cleanup triggers opcode deinit/release, not worker join. */
  MYFLT turnoff[] = {-1, 0, 0};
  csoundSetControlChannel(host, "fg.run", 0);
  csoundEvent(host, CS_INSTR_EVENT, turnoff, 3, 0);
  csoundEvent(second, CS_INSTR_EVENT, turnoff, 3, 0);
  CHECK(!csoundPerformKsmps(host) && !csoundPerformKsmps(second));
  CHECK(api->unregister_provider(host, provider));
  CHECK(api->unregister_provider(second, other));
  csoundReset(host); csoundReset(second);
  csoundDestroy(host); csoundDestroy(second);
  free(audio); free(memory); free(other_memory);
  CHECK(!dlclose(module));
  puts("prepared registry, independent hosts, worker stall/freeze/reset/resume/teardown pass");
  return 0;
}
