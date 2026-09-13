#include "naviergrain_provider.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define NG_COMMAND_SLOTS 4u
#define NG_PROVIDER_DOUBLES (NG_FIELD_ARRAYS * 64u * 64u)
typedef struct {
  atomic_uint ready;
  NGCommand value;
} NGCommandSlot;
struct NGProvider {
  max_align_t alignment;
  NGConfig config;
  double sr, fluid_hz;
  NGField worker, incoming, origin;
  double worker_data[NG_PROVIDER_DOUBLES];
  double incoming_data[NG_PROVIDER_DOUBLES];
  double origin_data[NG_PROVIDER_DOUBLES];
  unsigned char packet[NG_PACKET_MAX_BYTES];
  NGFieldQueue *queue;
  NGCommandSlot commands[NG_COMMAND_SLOTS];
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
size_t ng_provider_size(const NGConfig *config) {
  NGConfig checked;
  if (!config || ng_config_parse(&checked, config->value, NG_CONFIG_COUNT) ||
      config->value[NG_CONFIG_BACKEND] != 1) return 0;
  return sizeof(NGProvider) + ng_field_queue_size();
}
NGProvider *ng_provider_init(void *memory, size_t bytes, const NGConfig *config,
                             double sr, uint64_t generation) {
  size_t required = ng_provider_size(config);
  if (!memory || !required || bytes < required || !generation ||
      !isfinite(sr) || sr < 1 || sr > 384000) return NULL;
  NGProvider *p = memory;
  memset(p, 0, required);
  p->config = *config; p->sr = sr;
  p->fluid_hz = fmin(sr, config->value[NG_CONFIG_FLUID_HZ]);
  atomic_init(&p->epoch, generation);
  atomic_init(&p->failures, 0);
  if (!atomic_is_lock_free(&p->epoch) || !atomic_is_lock_free(&p->failures))
    return NULL;
  for (unsigned i = 0; i < NG_COMMAND_SLOTS; ++i) {
    atomic_init(&p->commands[i].ready, 0);
    if (!atomic_is_lock_free(&p->commands[i].ready)) return NULL;
  }
  unsigned n = (unsigned)config->value[NG_CONFIG_GRID_SIZE];
  uint32_t seed = (uint32_t)config->value[NG_CONFIG_SEED];
  ng_field_init(&p->worker, n, p->worker_data, seed);
  ng_field_init(&p->incoming, n, p->incoming_data, seed);
  ng_field_init(&p->origin, n, p->origin_data, seed);
  p->queue = ng_field_queue_init(p + 1, ng_field_queue_size(),
    (uint64_t)config->value[NG_CONFIG_INSTANCE_ID], generation, n);
  if (!p->queue) return NULL;
  p->was_moving = 1;
  return p;
}
uint64_t ng_provider_key(const NGProvider *p) {
  return p ? (uint64_t)p->config.value[NG_CONFIG_INSTANCE_ID] : 0;
}
int ng_provider_claimed(const NGProvider *p) { return p && p->claimed; }
void ng_provider_observe_acceptance(NGProvider *p, uint32_t words[6]) {
  if (p) p->acceptance = words;
}
int ng_provider_resume(NGProvider *p) {
  if (!p) return 0;
  uint64_t epoch = atomic_load_explicit(&p->epoch, memory_order_relaxed);
  if (epoch == UINT64_MAX) return 0;
  if (ng_field_queue_epoch(p->queue, epoch + 1) != NG_PACKET_OK) return 0;
  atomic_store_explicit(&p->epoch, epoch + 1, memory_order_release);
  p->resume_pending = 1;
  p->have_field = 0;
  return 1;
}
static void request(NGProvider *p, const NGExternalClock *clock) {
  if (p->command_sequence == UINT64_MAX) { ++p->command_drops; return; }
  uint64_t sequence = ++p->command_sequence;
  NGCommandSlot *slot = &p->commands[p->write_index];
  if (atomic_load_explicit(&slot->ready, memory_order_acquire)) {
    ++p->command_drops; return;
  }
  slot->value.epoch = atomic_load_explicit(&p->epoch, memory_order_relaxed);
  slot->value.reset_epoch = clock->reset_epoch;
  slot->value.sequence = sequence;
  slot->value.frame = clock->frame - p->origin_frame;
  memcpy(slot->value.controls, clock->controls, sizeof(slot->value.controls));
  atomic_store_explicit(&slot->ready, 1, memory_order_release);
  p->write_index = (p->write_index + 1) % NG_COMMAND_SLOTS;
}
NGPacketResult naviergrain_push_field(NGProvider *p, const void *packet, size_t bytes) {
  if (!p) return NG_PACKET_MALFORMED;
  NGPacketResult result = ng_field_queue_push(p->queue, packet, bytes);
  if (result != NG_PACKET_OK)
    atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
  return result;
}
int ng_provider_take_command(NGProvider *p, NGCommand *command) {
  if (!p || !command) return -1;
  NGCommandSlot *slot = &p->commands[p->read_index];
  if (!atomic_load_explicit(&slot->ready, memory_order_acquire)) return 0;
  *command = slot->value;
  atomic_store_explicit(&slot->ready, 0, memory_order_release);
  p->read_index = (p->read_index + 1) % NG_COMMAND_SLOTS;
  uint64_t epoch = atomic_load_explicit(&p->epoch, memory_order_acquire);
  if (command->epoch != epoch) { ++p->obsolete; return 2; }
  return 1;
}
int ng_provider_solve_command(NGProvider *p, const NGCommand *command,
                              void *packet, size_t bytes) {
  if (!p || !command || !packet || bytes != ng_packet_size(p->worker.n) ||
      !command->epoch || !command->sequence ||
      command->epoch < p->worker_epoch ||
      command->sequence <= p->worker_sequence ||
      command->reset_epoch < p->worker_reset_epoch ||
      (command->epoch == p->worker_epoch && command->frame < p->last_control))
    return 0;
  for (unsigned i = 0; i < NG_CONTROL_COUNT; ++i)
    if (!isfinite(command->controls[i]) ||
        command->controls[i] < (i == NG_CONTROL_PITCH_RATIO ?
          log2(ng_control_parameters[i].minimum) : ng_control_parameters[i].minimum) ||
        command->controls[i] > (i == NG_CONTROL_PITCH_RATIO ?
          log2(ng_control_parameters[i].maximum) : ng_control_parameters[i].maximum)) return 0;
  if (command->reset_epoch != p->worker_reset_epoch) {
    ng_field_init(&p->worker, p->worker.n, p->worker_data,
      (uint32_t)p->config.value[NG_CONFIG_SEED]);
    p->worker_reset_epoch = command->reset_epoch;
  }
  ++p->processed;
  p->last_control = command->frame;
  p->worker_epoch = command->epoch;
  p->worker_sequence = command->sequence;
  if (!ng_field_step(&p->worker, command->controls, p->fluid_hz,
      fmin(p->sr, p->config.value[NG_CONFIG_PARTICLE_HZ]),
      (unsigned)p->config.value[NG_CONFIG_PRESSURE_ITERATIONS],
      (unsigned)p->config.value[NG_CONFIG_VISCOSITY_ITERATIONS])) {
    atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
    return 0;
  }
  /* Solve one dt only, including when the command sequence has gaps. */
  p->worker.sequence = command->sequence;
  if (ng_packet_encode(packet, bytes, &p->worker, ng_provider_key(p),
                       command->epoch, command->frame) != NG_PACKET_OK) {
    atomic_fetch_add_explicit(&p->failures, 1, memory_order_relaxed);
    return 0;
  }
  return 1;
}
int ng_provider_work(NGProvider *p) {
  NGCommand command;
  int taken = ng_provider_take_command(p, &command);
  if (taken != 1) return taken == 2 ? 1 : taken;
  size_t bytes = ng_packet_size(p->worker.n);
  if (!ng_provider_solve_command(p, &command, p->packet, bytes)) return -1;
  return naviergrain_push_field(p, p->packet, bytes) == NG_PACKET_OK ? 1 : -1;
}
static void copy_published(NGField *to, const NGField *from) {
  size_t bytes = 4u * from->n * from->n * sizeof(double);
  memcpy(to->storage, from->storage, bytes);
  to->energy = from->energy; to->rms_omega = from->rms_omega;
  to->rms_strain = from->rms_strain; to->rms_divergence = from->rms_divergence;
  to->max_divergence = from->max_divergence; to->max_speed = from->max_speed;
}
/* Apply a fixed one-fluid-tick linear ramp, sampled at particle ticks.
 * Scalars are diagnostic blends; max_speed is a conservative actual bound,
 * not a trusted provider assertion. No solver runs on the consumer. */
static void blend(NGProvider *p, NGField *field, uint64_t frame) {
  double mix = fmin(1, (double)(frame - p->blend_frame) * p->fluid_hz / p->sr);
  size_t count = 4u * field->n * field->n;
  for (size_t i = 0; i < count; ++i)
    field->storage[i] = p->origin.storage[i] +
      mix * (p->incoming.storage[i] - p->origin.storage[i]);
#define NG_BLEND(name) field->name = p->origin.name + mix * (p->incoming.name - p->origin.name)
  NG_BLEND(energy); NG_BLEND(rms_omega); NG_BLEND(rms_strain);
  NG_BLEND(rms_divergence); NG_BLEND(max_divergence);
#undef NG_BLEND
  field->max_speed = fmax(p->origin.max_speed, p->incoming.max_speed);
  field->sequence = p->incoming.sequence; field->time = p->incoming.time;
  field->interventions = p->incoming.interventions; field->valid = 1;
}
static int safe_field(NGProvider *p, double flow) {
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
  double limit = 1.5 * fmin(p->sr, p->config.value[NG_CONFIG_PARTICLE_HZ]) /
                 (fmax(flow, 1e-6) * p->incoming.n);
  if (speed > limit * (1 + 1e-6)) return 0;
  p->incoming.max_speed = speed;
  return 1;
}
static NGExternalState update(void *owner, const NGExternalClock *c, NGField *field) {
  NGProvider *p = owner;
  p->last_frame = c->frame;
  if (c->reset_epoch != p->reset_epoch || (c->moving && !p->was_moving)) {
    if (!ng_provider_resume(p)) {
      return (NGExternalState){.status = NG_STATUS_PROVIDER_LOST, .motion = 0};
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
      NGPacketInfo info;
      if (ng_field_queue_consume(p->queue, c->frame - p->origin_frame,
                                 &p->incoming, &info) == NG_PACKET_OK) {
        if (safe_field(p, c->controls[NG_CONTROL_FLOW_SPEED])) {
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
  return (NGExternalState){.motion = p->motion,
    .field_frame = p->field_frame,
    .epoch = atomic_load_explicit(&p->epoch, memory_order_relaxed),
    .drops = p->command_drops +
      atomic_load_explicit(&p->failures, memory_order_relaxed) + p->unsafe,
    .status = (stale ? NG_STATUS_STALE_FIELD : 0u) |
              (stale && age >= .350 ? NG_STATUS_PROVIDER_LOST : 0u)};
}
int ng_provider_attach(NGProvider *p, NGEngine *engine) {
  if (!p || p->used || !ng_bind_external(engine, &p->config, p->sr, p, update))
    return 0;
  p->claimed = p->used = 1;
  return 1;
}
void ng_provider_release(NGProvider *p) {
  if (p) p->claimed = 0;
}
NGProviderStats ng_provider_stats(const NGProvider *p) {
  NGProviderStats s = {0};
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
  s.queue = ng_field_queue_stats(p->queue);
  return s;
}
