/* Linker instrumentation covers allocator references from our static C core,
 * not allocations hidden inside a host or a dynamically linked system library.
 */
#include "fluidgrain_core.h"
#include "fluidgrain_grain_plan.h"
#include "fluidgrain_pack.h"
#include "fluidgrain_resampler.h"
#include "fluidgrain_particles.h"
#include "fluidgrain_transport.h"
#include "fluidgrain_provider.h"
#include "fluidgrain_browser.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
static int rendering;
static unsigned calls, worker_steps, solver_on_audio;
static int external_audio, planning;
static unsigned reads_in_planner, reader_calls;
double __real_fg_read_bandlimited(const FGResampler *, const double *, size_t,
                                 double, double, int, double);
double __wrap_fg_read_bandlimited(const FGResampler *r, const double *source,
    size_t n, double phase, double increment, int loop, double edge) {
  ++reader_calls;
  if (planning) ++reads_in_planner;
  return __real_fg_read_bandlimited(r, source, n, phase, increment, loop, edge);
}
int __real_fg_field_step(FGField *, const double *, double, double, unsigned, unsigned);
int __wrap_fg_field_step(FGField *field, const double *controls, double hz,
                         double particle_hz, unsigned pressure, unsigned viscosity) {
  if (external_audio) ++solver_on_audio;
  ++worker_steps;
  return __real_fg_field_step(field, controls, hz, particle_hz, pressure, viscosity);
}
void *__wrap_malloc(size_t n) {
  if (rendering)
    ++calls;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t size) {
  if (rendering)
    ++calls;
  return __real_calloc(n, size);
}
void *__wrap_realloc(void *p, size_t n) {
  if (rendering)
    ++calls;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (rendering)
    ++calls;
  __real_free(p);
}
int main(void) {
  double config[FG_CONFIG_COUNT], control[FG_CONTROL_COUNT],
      stats[FG_STAT_COUNT];
  fg_defaults(config, control);
  config[FG_CONFIG_SOURCE_LOOP] = 1;
  control[FG_CONTROL_GRAIN_RATE] = 2000;
  FGConfig c;
  if (fg_config_parse(&c, config, FG_CONFIG_COUNT))
    return 1;
  size_t bytes = fg_memory_size(&c, 997, 48000, 48000);
  void *memory = malloc(bytes);
  FGEngine *engine = fg_init(memory, bytes, &c, 997, 48000, 48000);
  if (!engine) {
    free(memory);
    return 1;
  }
  for (unsigned i = 0; i < 997; ++i)
    fg_source(engine)[i] = sin(6.283185307179586 * i / 997);
  double field_storage[16 * 16 * FG_FIELD_ARRAYS];
  FGField field;
  fg_field_init(&field, 16, field_storage, 0);
  void *queue_memory = malloc(fg_field_queue_size());
  FGFieldQueue *queue = fg_field_queue_init(queue_memory, fg_field_queue_size(), 1, 1, 16);
  if (!queue) return 1;
  unsigned char packet[FG_PACKET_HEADER + 16 * 16 * 16];
  FGPacketInfo packet_info;
  FGParticle damaged = {.x = NAN, .y = .5, .vx = INFINITY};
  double view[FG_VIEW_SIZE];
  FGConfig external_config = c;
  external_config.value[FG_CONFIG_BACKEND] = 1;
  external_config.value[FG_CONFIG_INSTANCE_ID] = 23;
  external_config.value[FG_CONFIG_GRID_SIZE] = 16;
  size_t external_bytes = fg_memory_size(&external_config, 997, 48000, 48000);
  FGEngine *external = fg_init(malloc(external_bytes), external_bytes,
                               &external_config, 997, 48000, 48000);
  size_t provider_bytes = fg_provider_size(&external_config);
  FGProvider *provider = fg_provider_init(malloc(provider_bytes), provider_bytes,
                                           &external_config, 48000, 8);
  if (!external || !provider || !fg_provider_attach(provider, external)) return 1;
  FGProvider *browser_solver = fg_provider_init(malloc(provider_bytes), provider_bytes,
                                                  &external_config, 48000, 8);
  FGBrowserMailbox *browser_audio_mailbox = malloc(sizeof(FGBrowserMailbox));
  FGBrowserMailbox *browser_solver_mailbox = malloc(sizeof(FGBrowserMailbox));
  if (!browser_solver || !browser_audio_mailbox || !browser_solver_mailbox) return 1;
  fg_browser_init(browser_audio_mailbox, 0, 16);
  fg_browser_init(browser_solver_mailbox, 1, 16);
  size_t plan_bytes = fg_grain_plan_size(&c, 512);
  FGGrainPlan *plan = fg_grain_plan_init(malloc(plan_bytes), plan_bytes, &c, 512);
  if (!plan) return 1;
  size_t pack_bytes=fg_pack_size_frames((uint32_t)c.value[FG_CONFIG_MAX_GRAINS],512);
  size_t scratch_bytes=fg_pack_scratch_size_frames((uint32_t)c.value[FG_CONFIG_MAX_GRAINS],512);
  void *pack=malloc(pack_bytes),*scratch=malloc(scratch_bytes);
  if(!pack||!scratch)return 1;
  rendering = 1;
  unsigned recovered = fg_particle_advance(&field, &damaged, .01, 100, 0);
  int finite = 1;
  field.sequence = 1;
  if (fg_packet_encode(packet, sizeof(packet), &field, 1, 1, 0) != FG_PACKET_OK) return 1;
  for (unsigned i = 0; i < 4; ++i)
    if (fg_field_queue_push(queue, packet, sizeof(packet)) != FG_PACKET_OK) return 1;
  if (fg_field_queue_push(queue, packet, sizeof(packet)) != FG_PACKET_FULL) return 1;
  if (fg_field_queue_consume(queue, 0, &field, &packet_info) != FG_PACKET_OK) return 1;
  if (fg_field_queue_epoch(queue, 2) != FG_PACKET_OK) return 1;
  if (fg_field_queue_push(queue, packet, sizeof(packet)) != FG_PACKET_OK) return 1;
  if (fg_field_queue_consume(queue, 0, &field, &packet_info) != FG_PACKET_EMPTY) return 1;
  for (unsigned frame = 0; frame < 16000; ++frame) {
    control[FG_CONTROL_RESET] = frame >= 7109 && frame < 9109;
    control[FG_CONTROL_FREEZE] = frame >= 3111 && frame < 5999;
    control[FG_CONTROL_GAIN] = frame == 101 ? NAN : .15;
    control[FG_CONTROL_GRAIN_MS] = frame < 8000 ? 500 : 5;
    control[FG_CONTROL_PITCH_RATIO] = frame < 5000 ? .25 : 4;
    fg_controls(engine, control);
    double l, r;
    fg_sample(engine, &l, &r);
    fg_stats(engine, stats);
    if (frame % 64 == 0 && !fg_view(engine, view, FG_VIEW_SIZE)) return 1;
    finite = finite && isfinite(l) && isfinite(r);
  }
  for (unsigned batch = 0; batch < 4; ++batch) {
    double output[1024];
    if (!fg_grain_plan_begin(engine, plan)) return 1;
    planning = 1;
    for (unsigned f = 0; f < 512; ++f)
      if (!fg_grain_plan_sample(engine, plan)) return 1;
    planning = 0;
    if (!fg_grain_plan_seal(engine, plan) ||
        !fg_pack(plan,pack,pack_bytes,scratch,scratch_bytes) ||
        !fg_grain_plan_render(plan, output, 1024) ||
        !fg_grain_plan_commit(engine, plan, output, 1024)) return 1;
  }
  worker_steps = 0;
  for (unsigned frame = 0; frame < 4000; ++frame) {
    control[FG_CONTROL_RESET] = frame >= 1000 && frame < 1800;
    fg_controls(external, control);
    double l, r;
    external_audio = 1;
    fg_sample(external, &l, &r);
    external_audio = 0;
    if (fg_provider_work(provider) < 0) return 1;
  }
  if (!fg_provider_resume(provider)) return 1;
  for (unsigned frame = 0; frame < 4000; ++frame) {
    double l, r;
    external_audio = 1;
    fg_browser_service(browser_audio_mailbox, provider);
    fg_sample(external, &l, &r);
    external_audio = 0;
    if (browser_audio_mailbox->output_ready) {
      memcpy(browser_solver_mailbox->input, browser_audio_mailbox->output, FG_COMMAND_BYTES);
      browser_solver_mailbox->input_ready = 1;
      browser_audio_mailbox->output_ready = 0;
      fg_browser_service(browser_solver_mailbox, browser_solver);
      if (!browser_solver_mailbox->output_ready) return 1;
      memcpy(browser_audio_mailbox->input, browser_solver_mailbox->output,
             browser_audio_mailbox->packet_bytes);
      browser_audio_mailbox->input_ready = 1;
      browser_solver_mailbox->output_ready = 0;
    }
  }
  rendering = 0;
  printf("grain planner convolution calls: %u; 4 x 512-frame C recovery batches\n", reads_in_planner);
  if (reads_in_planner || !reader_calls) return 1;
  free(pack);free(scratch);free(plan);
  free(browser_solver); free(browser_audio_mailbox); free(browser_solver_mailbox);
  printf("external callback solver calls: %u; worker solver calls: %u\n",
         solver_on_audio, worker_steps);
  if (solver_on_audio || !worker_steps) return 1;
  fg_provider_release(provider);
  free(provider); free(external);
  unsigned long long epoch = (unsigned long long)fg_counters(engine).epoch;
  uint64_t control_events = fg_counters(engine).control_events;
  finite = finite && isfinite(damaged.x) && isfinite(damaged.vx);
  free(memory);
  free(queue_memory);
  printf("core allocator references during audio/control/stats/reset/transport: %u; "
         "epoch %llu\n",
         calls, epoch);
  return calls || !finite || epoch != 1 || !recovered || control_events != 1;
}
