#include "fluidgrain_provider.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define FG_COMMAND_SLOTS 4u
#define FG_PROVIDER_DOUBLES (FG_FIELD_ARRAYS * 64u * 64u)
typedef struct {
  atomic_uint ready;
  FGCommand value;
} FGCommandSlot;
struct FGProvider {
  max_align_t alignment;
  FGConfig config;
  double sr, fluid_hz;
  FGField worker, incoming, origin;
  double worker_data[FG_PROVIDER_DOUBLES];
  double incoming_data[FG_PROVIDER_DOUBLES];
  double origin_data[FG_PROVIDER_DOUBLES];
  unsigned char packet[FG_PACKET_MAX_BYTES];
  FGFieldQueue *queue;
  FGCommandSlot commands[FG_COMMAND_SLOTS];
  atomic_uint_fast64_t epoch;
  atomic_uint_fast64_t failures;
  /* Disjoint ownership: audio writes this group. */
  unsigned write_index;
  uint64_t command_sequence, reset_epoch, last_frame, origin_frame,
      field_frame, blend_frame, applied, unsafe, command_drops;
  double motion, latency_sum, latency_max;
  int claimed, used, was_moving, have_field, resume_pending;
  uint32_t *acceptance; /* audio-owned prepared observer, never worker-accessed */
  /* Worker writes this group. */
  unsigned read_index;
  uint64_t worker_reset_epoch, processed, obsolete, last_control,
      worker_epoch, worker_sequence;
};
size_t fg_provider_size(const FGConfig *config) {
  FGConfig checked;
  if (!config || fg_config_parse(&checked, config->value, FG_CONFIG_COUNT) ||
      config->value[FG_CONFIG_BACKEND] != 1) return 0;
  return sizeof(FGProvider) + fg_field_queue_size();
}
FGProvider *fg_provider_init(void *memory, size_t bytes, const FGConfig *config,
                             double sr, uint64_t generation) {
  size_t required = fg_provider_size(config);
  if (!memory || !required || bytes < required || !generation ||
      !isfinite(sr) || sr < 1 || sr > 384000) return NULL;
  FGProvider *p = memory;
  memset(p, 0, required);
  p->config = *config; p->sr = sr;
  p->fluid_hz = fmin(sr, config->value[FG_CONFIG_FLUID_HZ]);
  atomic_init(&p->epoch, generation);
  atomic_init(&p->failures, 0);
  if (!atomic_is_lock_free(&p->epoch) || !atomic_is_lock_free(&p->failures))
    return NULL;
  for (unsigned i = 0; i < FG_COMMAND_SLOTS; ++i) {
    atomic_init(&p->commands[i].ready, 0);
    if (!atomic_is_lock_free(&p->commands[i].ready)) return NULL;
  }
  unsigned n = (unsigned)config->value[FG_CONFIG_GRID_SIZE];
  uint32_t seed = (uint32_t)config->value[FG_CONFIG_SEED];
  fg_field_init(&p->worker, n, p->worker_data, seed);
  fg_field_init(&p->incoming, n, p->incoming_data, seed);
  fg_field_init(&p->origin, n, p->origin_data, seed);
  p->queue = fg_field_queue_init(p + 1, fg_field_queue_size(),
    (uint64_t)config->value[FG_CONFIG_INSTANCE_ID], generation, n);
  if (!p->queue) return NULL;
  p->was_moving = 1;
  return p;
}
uint64_t fg_provider_key(const FGProvider *p) {
  return p ? (uint64_t)p->config.value[FG_CONFIG_INSTANCE_ID] : 0;
}
int fg_provider_claimed(const FGProvider *p) { return p && p->claimed; }
void fg_provider_observe_acceptance(FGProvider *p, uint32_t words[6]) {
  if (p) p->acceptance = words;
}
int fg_provider_resume(FGProvider *p) {
  if (!p) return 0;
  uint64_t epoch = atomic_load_explicit(&p->epoch, memory_order_relaxed);
  if (epoch == UINT64_MAX) return 0;
  if (fg_field_queue_epoch(p->queue, epoch + 1) != FG_PACKET_OK) return 0;
  atomic_store_explicit(&p->epoch, epoch + 1, memory_order_release);
  p->resume_pending = 1;
  p->have_field = 0;
  return 1;
}
static void request(FGProvider *p, const FGExternalClock *clock) {
  if (p->command_sequence == UINT64_MAX) { ++p->command_drops; return; }
  uint64_t sequence = ++p->command_sequence;
  FGCommandSlot *slot = &p->commands[p->write_index];
  if (atomic_load_explicit(&slot->ready, memory_order_acquire)) {
    ++p->command_drops; return;
  }
  slot->value.epoch = atomic_load_explicit(&p->epoch, memory_order_relaxed);
  slot->value.reset_epoch = clock->reset_epoch;
  slot->value.sequence = sequence;
  slot->value.frame = clock->frame - p->origin_frame;
  memcpy(slot->value.controls, clock->controls, sizeof(slot->value.controls));
  atomic_store_explicit(&slot->ready, 1, memory_order_release);
  p->write_index = (p->write_index + 1) % FG_COMMAND_SLOTS;
}
FGPacketResult fluidgrain_push_field(FGProvider *p, const void *packet, size_t bytes) {
  if (!p) return FG_PACKET_MALFORMED;
  FGPacketResult result = fg_field_queue_push(p->queue, packet, bytes);
  if (result != FG_PACKET_OK)
    atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
  return result;
}
int fg_provider_take_command(FGProvider *p, FGCommand *command) {
  if (!p || !command) return -1;
  FGCommandSlot *slot = &p->commands[p->read_index];
  if (!atomic_load_explicit(&slot->ready, memory_order_acquire)) return 0;
  *command = slot->value;
  atomic_store_explicit(&slot->ready, 0, memory_order_release);
  p->read_index = (p->read_index + 1) % FG_COMMAND_SLOTS;
  uint64_t epoch = atomic_load_explicit(&p->epoch, memory_order_acquire);
  if (command->epoch != epoch) { ++p->obsolete; return 2; }
  return 1;
}
int fg_provider_solve_command(FGProvider *p, const FGCommand *command,
                              void *packet, size_t bytes) {
  if (!p || !command || !packet || bytes != fg_packet_size(p->worker.n) ||
      !command->epoch || !command->sequence ||
      command->epoch < p->worker_epoch ||
      command->sequence <= p->worker_sequence ||
      command->reset_epoch < p->worker_reset_epoch ||
      (command->epoch == p->worker_epoch && command->frame < p->last_control))
    return 0;
  for (unsigned i = 0; i < FG_CONTROL_COUNT; ++i)
    if (!isfinite(command->controls[i]) ||
        command->controls[i] < (i == FG_CONTROL_PITCH_RATIO ?
          log2(fg_control_parameters[i].minimum) : fg_control_parameters[i].minimum) ||
        command->controls[i] > (i == FG_CONTROL_PITCH_RATIO ?
          log2(fg_control_parameters[i].maximum) : fg_control_parameters[i].maximum)) return 0;
  if (command->reset_epoch != p->worker_reset_epoch) {
    fg_field_init(&p->worker, p->worker.n, p->worker_data,
      (uint32_t)p->config.value[FG_CONFIG_SEED]);
    p->worker_reset_epoch = command->reset_epoch;
  }
  ++p->processed;
  p->last_control = command->frame;
  p->worker_epoch = command->epoch;
  p->worker_sequence = command->sequence;
  if (!fg_field_step(&p->worker, command->controls, p->fluid_hz,
      fmin(p->sr, p->config.value[FG_CONFIG_PARTICLE_HZ]),
      (unsigned)p->config.value[FG_CONFIG_PRESSURE_ITERATIONS],
      (unsigned)p->config.value[FG_CONFIG_VISCOSITY_ITERATIONS])) {
    atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
    return 0;
  }
  /* Solve one dt only, including when the command sequence has gaps. */
  p->worker.sequence = command->sequence;
  if (fg_packet_encode(packet, bytes, &p->worker, fg_provider_key(p),
                       command->epoch, command->frame) != FG_PACKET_OK) {
    atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
    return 0;
  }
  return 1;
}
int fg_provider_work(FGProvider *p) {
  FGCommand command;
  int taken = fg_provider_take_command(p, &command);
  if (taken != 1) return taken == 2 ? 1 : taken;
  size_t bytes = fg_packet_size(p->worker.n);
  if (!fg_provider_solve_command(p, &command, p->packet, bytes)) return -1;
  return fluidgrain_push_field(p, p->packet, bytes) == FG_PACKET_OK ? 1 : -1;
}
static void copy_published(FGField *to, const FGField *from) {
  size_t bytes = 4u * from->n * from->n * sizeof(double);
  memcpy(to->storage, from->storage, bytes);
  to->energy = from->energy; to->rms_omega = from->rms_omega;
  to->rms_strain = from->rms_strain; to->rms_divergence = from->rms_divergence;
  to->max_divergence = from->max_divergence; to->max_speed = from->max_speed;
}
/* Apply a fixed one-fluid-tick linear ramp, sampled at particle ticks.
 * Scalars are diagnostic blends; max_speed is a conservative actual bound,
 * not a trusted provider assertion. No solver runs on the consumer. */
static void blend(FGProvider *p, FGField *field, uint64_t frame) {
  double mix = fmin(1, (double)(frame - p->blend_frame) * p->fluid_hz / p->sr);
  size_t count = 4u * field->n * field->n;
  for (size_t i = 0; i < count; ++i)
    field->storage[i] = p->origin.storage[i] +
      mix * (p->incoming.storage[i] - p->origin.storage[i]);
#define FG_BLEND(name) field->name = p->origin.name + mix * (p->incoming.name - p->origin.name)
  FG_BLEND(energy); FG_BLEND(rms_omega); FG_BLEND(rms_strain);
  FG_BLEND(rms_divergence); FG_BLEND(max_divergence);
#undef FG_BLEND
  field->max_speed = fmax(p->origin.max_speed, p->incoming.max_speed);
  field->sequence = p->incoming.sequence; field->time = p->incoming.time;
  field->interventions = p->incoming.interventions; field->valid = 1;
}
static int safe_field(FGProvider *p, double flow) {
  /* A finite packet can still claim a false speed bound. Derive it and
   * reject values outside the solver's fixed particle safety envelope.
   * Scalar magnitudes are bounded to avoid overflow in downstream mappings. */
  double u = 0, v = 0;
  size_t count = (size_t)p->incoming.n * p->incoming.n;
  for (size_t i = 0; i < count; ++i) {
    u = fmax(u, fabs(p->incoming.u[i]));
    v = fmax(v, fabs(p->incoming.v[i]));
    if (fabs(p->incoming.omega[i]) > 1e6 || p->incoming.strain[i] > 1e6)
      return 0;
  }
  double speed = hypot(u, v);
  /* Match the current particle travel budget. Later flow-speed changes
   * remain covered by the particle integrator's bounded substeps. */
  double limit = 1.5 * fmin(p->sr, p->config.value[FG_CONFIG_PARTICLE_HZ]) /
                 (fmax(flow, 1e-6) * p->incoming.n);
  if (speed > limit * (1 + 1e-6)) return 0;
  p->incoming.max_speed = speed;
  return 1;
}
static FGExternalState update(void *owner, const FGExternalClock *c, FGField *field) {
  FGProvider *p = owner;
  p->last_frame = c->frame;
  if (c->reset_epoch != p->reset_epoch || (c->moving && !p->was_moving)) {
    if (!fg_provider_resume(p)) {
      return (FGExternalState){.status = FG_STATUS_PROVIDER_LOST, .motion = 0};
    }
    p->reset_epoch = c->reset_epoch;
  }
  p->was_moving = c->moving;
  if (p->resume_pending) {
    p->origin_frame = p->field_frame = c->frame;
    p->blend_frame = c->frame;
    p->resume_pending = 0;
  }
  if (!c->resetting && c->moving) {
    if (c->fluid_tick) request(p, c);
    if (c->particle_tick) {
      /* Finish the current interpolation before saving its origin. */
      if (p->have_field) blend(p, field, c->frame);
      FGPacketInfo info;
      if (fg_field_queue_consume(p->queue, c->frame - p->origin_frame,
                                 &p->incoming, &info) == FG_PACKET_OK) {
        if (safe_field(p, c->controls[FG_CONTROL_FLOW_SPEED])) {
          copy_published(&p->origin, field);
          p->blend_frame = c->frame;
          p->field_frame = p->origin_frame + info.audio_frame;
          p->have_field = 1; ++p->applied;
          if (p->acceptance) {
            const uint64_t values[3] = {c->frame, info.epoch, info.sequence};
            for (unsigned i = 0; i < 3; ++i) {
              p->acceptance[2 * i] = (uint32_t)values[i];
              p->acceptance[2 * i + 1] = (uint32_t)(values[i] >> 32);
            }
          }
          double latency = 1000 * (double)(c->frame - p->field_frame) / p->sr;
          p->latency_sum += latency;
          p->latency_max = fmax(p->latency_max, latency);
        } else {
          ++p->unsafe;
          /* Incoming storage was overwritten; stop its use, hold the last
           * applied field until a fresh qualified snapshot arrives. */
          p->have_field = 0;
        }
      }
    }
  }
  double age = (double)(c->frame - p->field_frame) / p->sr;
  int stale = c->moving && !c->resetting && age > .100;
  double target = stale ? fmax(0, 1 - (age - .100) / .250) : 1;
  if (!p->have_field || c->resetting) target = 0;
  /* Linear loss ramp and 20ms recovery slew. Freeze holds the last gain. */
  if (c->moving && !c->resetting)
    p->motion += fmax(-1 / (.250 * p->sr),
                      fmin(1 / (.020 * p->sr), target - p->motion));
  if (c->resetting) p->motion = 0;
  return (FGExternalState){.motion = p->motion,
    .field_frame = p->field_frame,
    .epoch = atomic_load_explicit(&p->epoch, memory_order_relaxed),
    .drops = p->command_drops +
      atomic_load_explicit(&p->failures, memory_order_relaxed) + p->unsafe,
    .status = (stale ? FG_STATUS_STALE_FIELD : 0u) |
              (stale && age >= .350 ? FG_STATUS_PROVIDER_LOST : 0u)};
}
int fg_provider_attach(FGProvider *p, FGEngine *engine) {
  if (!p || p->used || !fg_bind_external(engine, &p->config, p->sr, p, update))
    return 0;
  p->claimed = p->used = 1;
  return 1;
}
void fg_provider_release(FGProvider *p) {
  if (p) p->claimed = 0;
}
FGProviderStats fg_provider_stats(const FGProvider *p) {
  FGProviderStats s = {0};
  if (!p) return s;
  s.commands = p->command_sequence; s.command_drops = p->command_drops;
  s.processed = p->processed; s.obsolete = p->obsolete;
  s.publish_failures = atomic_load_explicit(&p->failures, memory_order_relaxed);
  s.unsafe_fields = p->unsafe;
  s.epoch = atomic_load_explicit(&p->epoch, memory_order_relaxed);
  s.applied = p->applied; s.last_target = p->field_frame;
  s.last_control = p->last_control; s.motion_gain = p->motion;
  s.worker_simulation_time = p->worker.time;
  s.mean_latency_ms = p->applied ? p->latency_sum / (double)p->applied : 0;
  s.max_latency_ms = p->latency_max;
  s.queue = fg_field_queue_stats(p->queue);
  return s;
}
