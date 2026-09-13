#include "naviergrain_core.h"
#include "naviergrain_oscillator.h"
#include "naviergrain_mapping.h"
#include "naviergrain_grain_plan.h"
#include "naviergrain_particles.h"
#include "naviergrain_resampler.h"
#include <math.h>
#include <string.h>

#define NG_PI 3.14159265358979323846
#define NG_RESET_SLICE 64u

typedef struct {
  double phase, increment, pan, base_log_pitch, log_pitch;
  double pitch_omega, pitch_bend;
  double space[3],space_target[3],space_cell[2];
  NGParticle particle;
#ifdef NG_TEST_SPATIAL_SHUFFLE
  NGParticle mapping;
#endif
  uint64_t id;
  uint32_t age, length;
  int active;
} NGVoice;
typedef NGParticle NGEmitter;

struct NGEngine {
  NGConfig config;
  double sr, source_sr;
  size_t source_length;
  double *source, *weights;
  NGField field;
  void *provider;
  NGExternalUpdate external_update;
  NGExternalState external;
  NGResampler reader;
  double fluid_phase, particle_phase, simulation_time;
  double mean_speed, effective_rate, modulation;
  int sound_space;
  double low_log,high_log,target_low_log,target_high_log;
  uint64_t field_frame, frame;
  NGVoice *voices;
  NGEmitter *emitters;
#ifdef NG_TEST_SPATIAL_SHUFFLE
  NGEmitter *mapping_emitters;
  uint32_t *mapping_indices;
#endif
  uint32_t *free_indices;
  uint32_t capacity, emitter_count, free_count;
  uint32_t schedule_rng, select_rng, emitter_rng;
  double target[NG_CONTROL_COUNT], smooth[NG_CONTROL_COUNT];
  double smooth20, smooth10, smooth50;
  double phase, hazard, threshold, overlap, peak;
  double reset_gain;
  uint32_t reset_stage, reset_cursor, fade_samples, fade_cursor;
  NGCounters counters;
  unsigned status;
  int controls_initialized;
  NGGrainPlan *pending_plan;
};

struct NGGrainPlan {
  NGEngine *owner;
  uint64_t start_frame;
  uint32_t capacity, max_frames, frames;
  size_t grains;
  int sealed;
  NGPlannedFrame *frame;
  NGPlannedGrain *grain;
};

static double clamp(double x, double lo, double hi) {
  return fmin(hi, fmax(lo, x));
}
static uint32_t random_u32(uint32_t *state) {
  /* Full-period LCG + output permutation (PCG RXS-M-XS 32). Seed zero is valid.
   */
  *state = *state * UINT32_C(747796405) + UINT32_C(2891336453);
  uint32_t word =
      ((*state >> ((*state >> 28u) + 4u)) ^ *state) * UINT32_C(277803737);
  return (word >> 22u) ^ word;
}
static double uniform(uint32_t *state) {
  return ((double)random_u32(state) + 0.5) / 4294967296.0;
}
#ifdef NG_TEST_SPATIAL_SHUFFLE
#include "../tests/spatial_shuffle.inc"
#endif
static const NGParticle *emitter_mapping(const NGEngine *e, uint32_t i) {
#ifdef NG_TEST_SPATIAL_SHUFFLE
  return &e->mapping_emitters[i];
#else
  return &e->emitters[i];
#endif
}
static const NGParticle *voice_mapping(const NGVoice *v) {
#ifdef NG_TEST_SPATIAL_SHUFFLE
  return &v->mapping;
#else
  return &v->particle;
#endif
}
/* Particle mappings change at control ticks, not at the audio rate. Key the
 * cached nonlinear term by its actual input so shuffle/recovery also invalidate
 * it. Bit comparison preserves signed zero and does not approximate tanh. */
static double voice_pitch_bend(NGVoice *v, double omega) {
  if (memcmp(&omega, &v->pitch_omega, sizeof(omega))) {
    v->pitch_omega = omega;
    v->pitch_bend = tanh(omega / 10);
  }
  return v->pitch_bend;
}
static void seed_streams(NGEngine *e) {
  uint32_t seed = (uint32_t)e->config.value[NG_CONFIG_SEED];
  e->schedule_rng = seed ^ UINT32_C(0xa511e9b3);
  e->select_rng = seed ^ UINT32_C(0x63d83595);
  e->emitter_rng = seed ^ UINT32_C(0x9e3779b9);
  e->phase = e->hazard = 0;
  e->threshold = -log(uniform(&e->schedule_rng));
}
static void seed_emitter(NGEngine *e, uint32_t i) {
  /* Equal-area vertical strata with independently jittered y; any population
   * size. */
  memset(&e->emitters[i], 0, sizeof(e->emitters[i]));
  e->emitters[i].x = (i + uniform(&e->emitter_rng)) / e->emitter_count;
  e->emitters[i].y = uniform(&e->emitter_rng);
  e->emitters[i].path_x = e->emitters[i].x;
  e->emitters[i].path_y = e->emitters[i].y;
  e->weights[i] = 0;
}
void ng_defaults(double *config, double *controls) {
  for (size_t i = 0; i < NG_CONFIG_COUNT; ++i)
    config[i] = ng_config_parameters[i].initial;
  for (size_t i = 0; i < NG_CONTROL_COUNT; ++i)
    controls[i] = ng_control_parameters[i].initial;
}
const char *ng_config_parse(NGConfig *out, const double *v, size_t count) {
  if (!out || !v || count != NG_CONFIG_COUNT)
    return "config must have exactly 13 entries";
  for (size_t i = 0; i < count; ++i) {
    NGParameter p = ng_config_parameters[i];
    if (!isfinite(v[i]) || v[i] < p.minimum || v[i] > p.maximum ||
        (p.discrete && v[i] != floor(v[i])))
      return "config contains an invalid range, enum, or integer";
  }
  if (v[NG_CONFIG_GRID_SIZE] != 16 && v[NG_CONFIG_GRID_SIZE] != 32 &&
      v[NG_CONFIG_GRID_SIZE] != 64)
    return "grid_size must be 16, 32, or 64";
  if (v[NG_CONFIG_BACKEND] == 1 && v[NG_CONFIG_INSTANCE_ID] == 0)
    return "external backend needs a prepared instance ID";
  memcpy(out->value, v, sizeof(out->value));
  return NULL;
}
static int add_storage(size_t *n, size_t count, size_t width) {
  if (count > (SIZE_MAX - *n) / width)
    return 0;
  *n += count * width;
  return 1;
}
size_t ng_memory_size(const NGConfig *c, size_t n, double source_sr,
                      double sr) {
  NGConfig checked;
  if (!c || !n || !isfinite(sr) || sr < 1 || sr > 384000 ||
      !isfinite(source_sr) || source_sr <= 0 ||
      !ng_resampler_doubles(4 * (source_sr / sr)) ||
      ng_config_parse(&checked, c->value, NG_CONFIG_COUNT))
    return 0;
  size_t bytes = sizeof(NGEngine);
#ifdef NG_TEST_SPATIAL_SHUFFLE
  if (!add_storage(&bytes, (size_t)c->value[NG_CONFIG_EMITTER_COUNT],
                   sizeof(NGEmitter)) ||
      !add_storage(&bytes, (size_t)c->value[NG_CONFIG_MAX_GRAINS],
                   sizeof(uint32_t)))
    return 0;
#endif
  if (!add_storage(&bytes, (size_t)c->value[NG_CONFIG_MAX_GRAINS],
                   sizeof(NGVoice)) ||
      !add_storage(&bytes, (size_t)c->value[NG_CONFIG_EMITTER_COUNT],
                   sizeof(NGEmitter)) ||
      !add_storage(&bytes, n, sizeof(double)) ||
      !add_storage(&bytes, (size_t)c->value[NG_CONFIG_EMITTER_COUNT],
                   sizeof(double)) ||
      !add_storage(&bytes,
                   ng_field_doubles((unsigned)c->value[NG_CONFIG_GRID_SIZE]),
                   sizeof(double)) ||
      !add_storage(&bytes, ng_resampler_doubles(4 * (source_sr / sr)),
                   sizeof(double)) ||
      !add_storage(&bytes, (size_t)c->value[NG_CONFIG_MAX_GRAINS],
                   sizeof(uint32_t)))
    return 0;
  return bytes;
}
NGEngine *ng_init(void *memory, size_t bytes, const NGConfig *c, size_t n,
                  double source_sr, double sr) {
  size_t required = ng_memory_size(c, n, source_sr, sr);
  if (!memory || !required || bytes < required)
    return NULL;
  memset(memory, 0, required);
  NGEngine *e = memory;
  e->config = *c;
  e->sr = sr;
  e->source_sr = source_sr;
  e->source_length = n;
  e->capacity = (uint32_t)c->value[NG_CONFIG_MAX_GRAINS];
  e->emitter_count = (uint32_t)c->value[NG_CONFIG_EMITTER_COUNT];
  e->voices = (NGVoice *)(e + 1);
  e->emitters = (NGEmitter *)(e->voices + e->capacity);
  e->source = (double *)(e->emitters + e->emitter_count);
#ifdef NG_TEST_SPATIAL_SHUFFLE
  e->mapping_emitters = (NGEmitter *)e->source;
  e->source = (double *)(e->mapping_emitters + e->emitter_count);
#endif
  e->weights = e->source + n;
  double *field_storage = e->weights + e->emitter_count;
  ng_field_init(&e->field, (unsigned)c->value[NG_CONFIG_GRID_SIZE],
                field_storage, (uint32_t)c->value[NG_CONFIG_SEED]);
  double *coefficients = field_storage + ng_field_doubles(e->field.n);
  ng_resampler_init(&e->reader, coefficients, 4 * (source_sr / sr));
  e->free_indices =
      (uint32_t *)(coefficients + ng_resampler_doubles(4 * (source_sr / sr)));
  e->free_count = e->capacity;
#ifdef NG_TEST_SPATIAL_SHUFFLE
  e->mapping_indices = e->free_indices + e->capacity;
#endif
  for (uint32_t i = 0; i < e->capacity; ++i)
    e->free_indices[i] = e->capacity - 1 - i;
  seed_streams(e);
  for (uint32_t i = 0; i < e->emitter_count; ++i)
    seed_emitter(e, i);
#ifdef NG_TEST_SPATIAL_SHUFFLE
  shuffle_mappings(e);
#endif
  for (size_t i = 0; i < NG_CONTROL_COUNT; ++i)
    e->target[i] = e->smooth[i] = ng_control_parameters[i].initial;
  e->smooth[NG_CONTROL_PITCH_RATIO] = 0; /* internal log2 ratio */
  e->smooth20 = -expm1(-1 / (.020 * sr));
  e->smooth10 = -expm1(-1 / (.010 * sr));
  e->smooth50 = -expm1(-1 / (.050 * sr));
  e->fade_samples = (uint32_t)fmax(1, ceil(.010 * sr));
  e->reset_gain = e->modulation = 1;
  e->status = c->value[NG_CONFIG_BACKEND] == 0 ? NG_STATUS_READY : NG_STATUS_NOT_PREPARED;
  return e;
}
double *ng_source(NGEngine *e) { return e->source; }
int ng_source_cosine(NGEngine *e) {
  if (!e || e->frame || !e->config.value[NG_CONFIG_SOURCE_LOOP]) return 0;
  for (size_t i=0;i<e->source_length;++i)
    e->source[i]=cos(2*NG_PI*(double)i/(double)e->source_length);
  e->reader.cosine=1;
  return 1;
}
int ng_sound_space(NGEngine *e,double low,double high) {
  if(!e||!e->reader.cosine||e->pending_plan||(!e->sound_space&&e->frame)||
     !isfinite(low)||!isfinite(high)||low<55||high>12000||low>high)return 0;
  high=fmin(high,.45*e->sr);low=fmin(low,high);
  e->target_low_log=log2(low);e->target_high_log=log2(high);
  if(!e->sound_space||!e->frame){e->low_log=e->target_low_log;e->high_log=e->target_high_log;}
  e->sound_space=1;return 1;
}
void ng_sound_bounds(const NGEngine *e,double out[2]) {
  out[0]=ng_osc_pitch(e->low_log);out[1]=ng_osc_pitch(e->high_log);
}
static void sound_target(const NGEngine *e,NGVoice *v) {
  const NGParticle *p=&v->particle;
  double cell_x=floor(p->path_x),cell_y=floor(p->path_y);
  ng_sound_position(p->x,p->y,p->motion_speed*e->smooth[NG_CONTROL_FLOW_SPEED],v->space_target);
  /* The displayed chart has an edge at each periodic seam. Smoothing a wrap
   * would invent a flight (and a pitch sweep) through its interior. */
  if(cell_x!=v->space_cell[0]||cell_y!=v->space_cell[1])
    memcpy(v->space,v->space_target,sizeof(v->space));
  v->space_cell[0]=cell_x;v->space_cell[1]=cell_y;
}
static void sound_voice(const NGEngine *e,NGVoice *v) {
  double height=clamp(v->space[1]/NG_SOUND_HEIGHT+.5,0,1);
  v->log_pitch=e->low_log+height*(e->high_log-e->low_log);
  v->increment=ng_osc_pitch(v->log_pitch)*(double)e->source_length/e->sr;
  v->pan=.5+.425*v->space[0];
}
int ng_bind_external(NGEngine *e, const NGConfig *config, double sr,
                      void *owner, NGExternalUpdate update) {
  if (!e || !config || !owner || !update || e->frame || e->external_update ||
      e->config.value[NG_CONFIG_BACKEND] != 1 || sr != e->sr)
    return 0;
  const unsigned keys[] = {NG_CONFIG_GRID_SIZE, NG_CONFIG_SEED,
    NG_CONFIG_FLUID_HZ, NG_CONFIG_PARTICLE_HZ, NG_CONFIG_PRESSURE_ITERATIONS,
    NG_CONFIG_VISCOSITY_ITERATIONS, NG_CONFIG_INSTANCE_ID, NG_CONFIG_BACKEND};
  for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
    if (config->value[keys[i]] != e->config.value[keys[i]]) return 0;
  e->provider = owner;
  e->external_update = update;
  NGExternalClock clock = {.controls = e->smooth, .moving = 1};
  e->external = update(owner, &clock, &e->field);
  e->status = NG_STATUS_READY;
  return 1;
}

void ng_controls(NGEngine *e, const double v[NG_CONTROL_COUNT]) {
  for (size_t i = 0; i < NG_CONTROL_COUNT; ++i) {
    NGParameter p = ng_control_parameters[i];
    double x = v[i];
    if (!isfinite(x) ||
        (p.discrete && (x != floor(x) || x < p.minimum || x > p.maximum))) {
      ++e->counters.control_events;
      continue;
    }
    double safe = clamp(x, p.minimum, p.maximum);
    if (safe != x)
      ++e->counters.control_events;
    if (i == NG_CONTROL_SCHEDULER && safe != e->target[i]) {
      e->phase = e->hazard = 0;
      e->threshold = -log(uniform(&e->schedule_rng));
    }
    if (i == NG_CONTROL_RESET && safe == 1 && e->target[i] == 0 &&
        e->reset_stage == 0) {
      e->reset_stage = 1;
      e->fade_cursor = 0;
    }
    e->target[i] = safe;
    if (i >= NG_CONTROL_FREEZE)
      e->smooth[i] = safe;
  }
  if (!e->controls_initialized) {
    memcpy(e->smooth, e->target, sizeof(e->smooth));
    e->smooth[NG_CONTROL_PITCH_RATIO] = log2(e->target[NG_CONTROL_PITCH_RATIO]);
    e->controls_initialized = 1;
  }
}
static void birth(NGEngine *e) {
  /* Selection stream advances for every proposal, including pool drops. */
  double density_mix = e->smooth[NG_CONTROL_MAPPING_MIX] * e->modulation *
                       e->smooth[NG_CONTROL_SPEED_TO_DENSITY];
  double draw =
      uniform(&e->select_rng) *
      (e->emitter_count + density_mix * e->weights[e->emitter_count - 1]);
  uint32_t lo = 0, hi = e->emitter_count - 1;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (draw < mid + 1 + density_mix * e->weights[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  uint32_t emitter = lo;
  if (!e->free_count) {
    ++e->counters.voice_drops;
    return;
  }
  NGVoice *v = &e->voices[e->free_indices[--e->free_count]];
  double *c = e->smooth;
  double mix = c[NG_CONTROL_MAPPING_MIX] * e->modulation;
  const NGParticle *mapping = emitter_mapping(e, emitter);
  double position =
      c[NG_CONTROL_POSITION_CENTER] +
      mix * c[NG_CONTROL_POSITION_SPAN] * (mapping->x - .5);
  if (e->config.value[NG_CONFIG_SOURCE_LOOP])
    position -= floor(position);
  else
    position = clamp(position, 0, 1);
  v->phase =
      position * (double)(e->source_length -
                          (e->config.value[NG_CONFIG_SOURCE_LOOP] ? 0u : 1u));
  v->base_log_pitch = c[NG_CONTROL_PITCH_RATIO];
  v->particle = e->emitters[emitter];
  v->particle.time = e->simulation_time;
#ifdef NG_TEST_SPATIAL_SHUFFLE
  v->mapping = *mapping;
#endif
  v->id = e->counters.births;
  v->pitch_omega = mapping->omega;
  v->pitch_bend = tanh(mapping->omega / 10);
  v->log_pitch =
      clamp(v->base_log_pitch + mix * c[NG_CONTROL_PITCH_DEPTH] *
                                    (e->reader.cosine ? ng_cosine_bend(mapping->y, mapping->motion_speed*c[NG_CONTROL_FLOW_SPEED]) : v->pitch_bend) / 12,
            -2, 2);
  v->increment = (e->reader.cosine ? ng_osc_pitch(v->log_pitch) : exp2(v->log_pitch)) * (e->source_sr / e->sr);
  v->pan = .5 + mix * c[NG_CONTROL_STEREO_WIDTH] * (mapping->y - .5);
  if(e->sound_space){sound_target(e,v);memcpy(v->space,v->space_target,sizeof(v->space));sound_voice(e,v);}
  v->age = 0;
  double strain = mapping->strain / (mapping->strain + 10);
  double duration =
      clamp(c[NG_CONTROL_GRAIN_MS] /
                (1 + mix * c[NG_CONTROL_STRAIN_TO_DURATION] * strain),
            5, 500);
  if(e->reader.cosine)
    duration=clamp(ng_cosine_duration(c[NG_CONTROL_GRAIN_MS],mix,
      c[NG_CONTROL_STRAIN_TO_DURATION],mapping->motion_speed*c[NG_CONTROL_FLOW_SPEED]),5,500);
  v->length = (uint32_t)fmax(2, round(duration * .001 * e->sr));
  v->active = 1;
  ++e->counters.births;
}
static void reset_step(NGEngine *e) {
  if (e->reset_stage == 1) {
    e->reset_gain = 1 - (double)++e->fade_cursor / e->fade_samples;
    if (e->fade_cursor == e->fade_samples) {
      e->reset_stage = 2;
      e->reset_cursor = 0;
      e->free_count = 0;
      seed_streams(e);
    }
  } else if (e->reset_stage == 2) {
    uint32_t records = e->capacity + e->emitter_count;
    uint32_t limit = records + (uint32_t)ng_field_doubles(e->field.n);
    for (uint32_t j = 0; j < NG_RESET_SLICE && e->reset_cursor < limit;
         ++j, ++e->reset_cursor) {
      uint32_t i = e->reset_cursor;
      if (i < e->capacity) {
        e->voices[i].active = 0;
        e->free_indices[e->free_count++] = e->capacity - 1 - i;
      } else if (i < records)
        seed_emitter(e, i - e->capacity);
      else
        e->field.storage[i - records] = 0;
    }
    if (e->reset_cursor == limit) {
      ++e->counters.epoch;
      e->overlap = e->fluid_phase = e->particle_phase = 0;
      e->simulation_time = e->mean_speed = 0;
      e->field_frame = e->frame;
      e->modulation = 1;
      ng_field_reset(&e->field);
#ifdef NG_TEST_SPATIAL_SHUFFLE
      shuffle_mappings(e);
#endif
      e->reset_stage = 3;
      e->fade_cursor = 0;
    }
  } else if (e->reset_stage == 3) {
    e->reset_gain = (double)++e->fade_cursor / e->fade_samples;
    if (e->fade_cursor == e->fade_samples)
      e->reset_stage = 0;
  }
}
static void simulation_step(NGEngine *e) {
  double *c = e->smooth;
  int moving =
      !e->target[NG_CONTROL_FREEZE] && e->target[NG_CONTROL_FLOW_SPEED] > 0;
  double fluid_hz = fmin(e->sr, e->config.value[NG_CONFIG_FLUID_HZ]);
  double particle_hz = fmin(e->sr, e->config.value[NG_CONFIG_PARTICLE_HZ]);
  int active = e->reset_stage == 0 || e->reset_stage == 3;
  int fluid_tick = 0, particle_tick = 0;
  if (active && moving) {
    e->fluid_phase += fluid_hz / e->sr;
    e->particle_phase += particle_hz / e->sr;
    if (e->fluid_phase >= 1 - 1e-12) {
      e->fluid_phase = fmax(0, e->fluid_phase - 1);
      fluid_tick = 1;
    }
    if (e->particle_phase >= 1 - 1e-12) {
      e->particle_phase = fmax(0, e->particle_phase - 1);
      particle_tick = 1;
    }
  }
  if (e->external_update) {
    NGExternalClock clock = {.frame = e->frame,
      .reset_epoch = e->counters.epoch, .fluid_tick = fluid_tick,
      .particle_tick = particle_tick, .moving = moving,
      .resetting = !active, .controls = c};
    e->external = e->external_update(e->provider, &clock, &e->field);
    e->field_frame = e->external.field_frame;
  }
  if (active && moving) {
    double motion = e->external_update ? e->external.motion : 1;
    e->simulation_time += motion * c[NG_CONTROL_FLOW_SPEED] / e->sr;
    if (fluid_tick && !e->external_update) {
      if (ng_field_step(
              &e->field, c, fluid_hz, particle_hz,
              (unsigned)e->config.value[NG_CONFIG_PRESSURE_ITERATIONS],
              (unsigned)e->config.value[NG_CONFIG_VISCOSITY_ITERATIONS]))
        e->field_frame = e->frame;
    }
    if (particle_tick) {
      double speed_sum = 0;
      for (uint32_t i = 0; i < e->emitter_count; ++i) {
        NGParticle *p = &e->emitters[i];
        if (e->field.valid)
          e->counters.particle_interventions += ng_particle_advance(
              &e->field, p, e->simulation_time - p->time,
              c[NG_CONTROL_INERTIA_MS], c[NG_CONTROL_ATTRACTION]);
        p->time = e->simulation_time;
        speed_sum += p->speed / (p->speed + 1);
        e->weights[i] = speed_sum;
      }
      e->mean_speed = speed_sum / e->emitter_count;
      for (uint32_t i = 0; i < e->capacity; ++i) {
        NGVoice *v = &e->voices[i];
        if (!v->active)
          continue;
        if (e->field.valid)
          e->counters.particle_interventions += ng_particle_advance(
              &e->field, &v->particle, e->simulation_time - v->particle.time,
              c[NG_CONTROL_INERTIA_MS], c[NG_CONTROL_ATTRACTION]);
        v->particle.time = e->simulation_time;
        if(e->sound_space)sound_target(e,v);
      }
#ifdef NG_TEST_SPATIAL_SHUFFLE
      shuffle_mappings(e);
#endif
    }
  }
  e->modulation += e->smooth20 * ((e->field.valid ? (e->external_update ? e->external.motion : 1) : 0) - e->modulation);
  /* The cumulative speed table updates only at particle ticks. Its affine
   * uniform-plus-speed weighting uses current controls in the birth search. */
  e->effective_rate =
      e->target[NG_CONTROL_GRAIN_RATE] == 0
          ? 0
          : c[NG_CONTROL_GRAIN_RATE] *
                (1 + c[NG_CONTROL_MAPPING_MIX] * e->modulation *
                         c[NG_CONTROL_SPEED_TO_DENSITY] * e->mean_speed);
}
static void sample_step(NGEngine *e, double *left, double *right,
                        NGGrainPlan *plan) {
  if (e->status & NG_STATUS_NOT_PREPARED) {
    *left = *right = 0;
    return;
  }
  for (size_t i = 0; i < NG_CONTROL_FREEZE; ++i) {
    double x = e->target[i];
    if (i == NG_CONTROL_PITCH_RATIO)
      x = log2(x);
    /* Linear viscosity smoothing defines the zero/positive transition without
     * log(0). */
    double coefficient =
        i == NG_CONTROL_PITCH_RATIO ? e->smooth10 : e->smooth20;
    e->smooth[i] += coefficient * (x - e->smooth[i]);
  }
  reset_step(e);
  if(e->sound_space){e->low_log+=e->smooth20*(e->target_low_log-e->low_log);e->high_log+=e->smooth20*(e->target_high_log-e->high_log);}
  simulation_step(e);
  double rate = e->effective_rate;
  ++e->frame;
  if (e->reset_stage == 0 || e->reset_stage == 3) {
    if (e->target[NG_CONTROL_SCHEDULER] == 0) {
      e->phase += rate / e->sr;
      double due = floor(e->phase + 1e-12);
      e->phase = fmax(0, e->phase - due);
      unsigned count = (unsigned)fmin(due, 4);
      for (unsigned i = 0; i < count; ++i)
        birth(e);
      e->counters.cap_drops += (uint64_t)(due - count);
    } else if (rate > 0) {
      e->hazard += rate / e->sr;
      unsigned count = 0;
      while (count < 4 && e->hazard >= e->threshold) {
        e->hazard -= e->threshold;
        birth(e);
        ++count;
        e->threshold = -log(uniform(&e->schedule_rng));
      }
      if (e->hazard >= e->threshold) {
        /* Censor one fifth proposal and discard the unexamined hazard interval.
         */
        ++e->counters.cap_drops;
        e->counters.discarded_hazard += e->hazard;
        e->hazard = 0;
        e->threshold = -log(uniform(&e->schedule_rng));
      }
    }
  }
  double l = 0, r = 0, power = 0;
  NGPlannedFrame *planned = plan ? &plan->frame[plan->frames] : NULL;
  if (planned) {
    planned->epoch = e->counters.epoch;
    planned->offset = plan->grains;
    planned->count = 0;
  }
  if (e->reset_stage != 2) {
    double mix = e->smooth[NG_CONTROL_MAPPING_MIX] * e->modulation;
    for (uint32_t i = 0; i < e->capacity; ++i) {
      NGVoice *v = &e->voices[i];
      if (!v->active)
        continue;
      double angle=2 * NG_PI * v->age / (v->length - 1);
      double env = (v->age == 0 || v->age == v->length - 1)
                       ? 0
                       : .5 - .5 * (e->reader.cosine ? ng_osc_cos(angle) : cos(angle));
      const NGParticle *mapping = voice_mapping(v);
      if(e->sound_space) {
        for(unsigned axis=0;axis<3;++axis)v->space[axis]+=e->smooth10*(v->space_target[axis]-v->space[axis]);
        sound_voice(e,v);
      } else {
      double pitch =
          clamp(v->base_log_pitch + mix * e->smooth[NG_CONTROL_PITCH_DEPTH] *
                                        (e->reader.cosine ? ng_cosine_bend(mapping->y,mapping->motion_speed*e->smooth[NG_CONTROL_FLOW_SPEED]) : voice_pitch_bend(v, mapping->omega)) / 12,
                -2, 2);
      v->log_pitch += e->smooth10 * (pitch - v->log_pitch);
      v->increment = (e->reader.cosine ? ng_osc_pitch(v->log_pitch) : exp2(v->log_pitch)) * (e->source_sr / e->sr);
      double pan =
          .5 + mix * e->smooth[NG_CONTROL_STEREO_WIDTH] * (mapping->y - .5);
      v->pan += e->smooth10 * (pan - v->pan);
      }
      if (plan) {
        NGPlannedGrain *g = &plan->grain[plan->grains++];
        *g = (NGPlannedGrain){.id = v->id, .slot = i, .phase = v->phase,
          .increment = v->increment, .pan = v->pan, .envelope = env};
        ++planned->count;
      } else {
        double sample =
            ng_read_bandlimited(&e->reader, e->source, e->source_length, v->phase,
                                v->increment,
                                (int)e->config.value[NG_CONFIG_SOURCE_LOOP],
                                .002 * e->source_sr) *
            env;
        l += sample * (e->reader.cosine ? ng_osc_cos(.5 * NG_PI * v->pan) : cos(.5 * NG_PI * v->pan));
        r += sample * (e->reader.cosine ? ng_osc_sin(.5 * NG_PI * v->pan) : sin(.5 * NG_PI * v->pan));
      }
      power += env * env;
      v->phase += v->increment;
      if (e->config.value[NG_CONFIG_SOURCE_LOOP] &&
          !(v->phase >= 0 && v->phase < (double)e->source_length))
        v->phase = fmod(v->phase, (double)e->source_length);
      if (++v->age >= v->length) {
        v->active = 0;
        e->free_indices[e->free_count++] = i;
      }
    }
  }
  e->overlap += e->smooth50 * (power - e->overlap);
  double gain =
      e->smooth[NG_CONTROL_GAIN] * e->reset_gain / sqrt(fmax(1, e->overlap));
  if (planned) {
    planned->gain = gain;
    ++plan->frames;
    return;
  }
  *left = l * gain;
  *right = r * gain;
  if (!isfinite(*left) || !isfinite(*right)) {
    *left = *right = 0;
    e->status |= NG_STATUS_NUMERIC_INTERVENTION;
    ++e->counters.numeric_interventions;
  }
  e->peak = fmax(e->peak, fmax(fabs(*left), fabs(*right)));
}
void ng_sample(NGEngine *e, double *left, double *right) {
  /* A pending offline plan must be resolved first; never lose its samples. */
  if (e->pending_plan) {
    *left = *right = 0;
    return;
  }
  sample_step(e, left, right, NULL);
}

size_t ng_grain_plan_size(const NGConfig *config, uint32_t max_frames) {
  if (!config || !max_frames || max_frames > NG_GRAIN_PLAN_MAX_FRAMES)
    return 0;
  double voices = config->value[NG_CONFIG_MAX_GRAINS];
  if (!isfinite(voices) || voices < 1 || voices > 4096 || floor(voices) != voices)
    return 0;
  return sizeof(NGGrainPlan) + max_frames * sizeof(NGPlannedFrame) +
      (size_t)voices * max_frames * sizeof(NGPlannedGrain);
}
NGGrainPlan *ng_grain_plan_init(void *memory, size_t bytes,
                               const NGConfig *config, uint32_t max_frames) {
  size_t required = ng_grain_plan_size(config, max_frames);
  if (!memory || !required || bytes < required) return NULL;
  memset(memory, 0, required);
  NGGrainPlan *plan = memory;
  plan->capacity = (uint32_t)config->value[NG_CONFIG_MAX_GRAINS];
  plan->max_frames = max_frames;
  plan->frame = (NGPlannedFrame *)(plan + 1);
  plan->grain = (NGPlannedGrain *)(plan->frame + max_frames);
  return plan;
}
int ng_grain_plan_begin(NGEngine *e, NGGrainPlan *plan) {
  if (!e || !plan || e->pending_plan || plan->owner ||
      e->capacity != plan->capacity || (e->status & NG_STATUS_NOT_PREPARED))
    return 0;
  plan->owner = e;
  plan->start_frame = e->frame;
  plan->frames = 0;
  plan->grains = 0;
  plan->sealed = 0;
  e->pending_plan = plan;
  return 1;
}
int ng_grain_plan_sample(NGEngine *e, NGGrainPlan *plan) {
  if (!e || !plan || e->pending_plan != plan || plan->owner != e ||
      plan->sealed || plan->frames == plan->max_frames) return 0;
  double left = 0, right = 0;
  sample_step(e, &left, &right, plan);
  return 1;
}
int ng_grain_plan_seal(NGEngine *e, NGGrainPlan *plan) {
  if (!e || !plan || e->pending_plan != plan || plan->owner != e ||
      plan->sealed || !plan->frames) return 0;
  plan->sealed = 1;
  return 1;
}
int ng_grain_plan_view(const NGGrainPlan *plan, NGGrainPlanView *out) {
  if (!plan || !out || !plan->owner || !plan->sealed) return 0;
  *out = (NGGrainPlanView){.start_frame = plan->start_frame,
    .frames = plan->frames, .capacity = plan->capacity, .grains = plan->grains,
    .frame = plan->frame, .grain = plan->grain};
  return 1;
}
int ng_engine_grain_resources(const NGEngine *e, NGGrainResources *out) {
  if (!e || !out || (e->status & NG_STATUS_NOT_PREPARED)) return 0;
  *out = (NGGrainResources){e->source, e->source_length, &e->reader,
    .002 * e->source_sr, (int)e->config.value[NG_CONFIG_SOURCE_LOOP]};
  return 1;
}
int ng_grain_plan_resources(const NGGrainPlan *plan, NGGrainResources *out) {
  return plan && plan->sealed && plan->owner && ng_engine_grain_resources(plan->owner, out);
}
int ng_grain_plan_render(const NGGrainPlan *plan, double *output, size_t samples) {
  if (!plan || !output || !plan->sealed || !plan->owner ||
      samples < (size_t)plan->frames * 2) return 0;
  const NGEngine *e = plan->owner;
  for (uint32_t f = 0; f < plan->frames; ++f) {
    const NGPlannedFrame *frame = &plan->frame[f];
    double left = 0, right = 0;
    for (uint32_t i = 0; i < frame->count; ++i) {
      const NGPlannedGrain *g = &plan->grain[frame->offset + i];
      double sample = ng_read_bandlimited(&e->reader, e->source,
          e->source_length, g->phase, g->increment,
          (int)e->config.value[NG_CONFIG_SOURCE_LOOP], .002 * e->source_sr) *
          g->envelope;
      left += sample * (e->reader.cosine ? ng_osc_cos(.5 * NG_PI * g->pan) : cos(.5 * NG_PI * g->pan));
      right += sample * (e->reader.cosine ? ng_osc_sin(.5 * NG_PI * g->pan) : sin(.5 * NG_PI * g->pan));
    }
    output[2u*f] = left * frame->gain;
    output[2u*f+1u] = right * frame->gain;
  }
  return 1;
}
int ng_grain_plan_commit(NGEngine *e, NGGrainPlan *plan, double *output,
                          size_t samples) {
  if (!e || !plan || !output || plan->owner != e || e->pending_plan != plan ||
      !plan->sealed || samples < (size_t)plan->frames * 2 ||
      e->frame - plan->start_frame != plan->frames) return 0;
  for (uint32_t f = 0; f < plan->frames; ++f) {
    double *left = &output[2u*f], *right = &output[2u*f+1u];
    if (!isfinite(*left) || !isfinite(*right)) {
      *left = *right = 0;
      e->status |= NG_STATUS_NUMERIC_INTERVENTION;
      ++e->counters.numeric_interventions;
    }
    e->peak = fmax(e->peak, fmax(fabs(*left), fabs(*right)));
  }
  e->pending_plan = NULL;
  plan->owner = NULL;
  plan->sealed = 0;
  return 1;
}

static double display_counter(uint64_t x) {
  return (double)(x % UINT64_C(16777216));
}
void ng_stats(NGEngine *e, double s[NG_STAT_COUNT]) {
  memset(s, 0, NG_STAT_COUNT * sizeof(*s));
  s[NG_STAT_LIVE_GRAINS] =
      e->reset_stage == 2 ? 0 : e->capacity - e->free_count;
  s[NG_STAT_EMITTER_COUNT] = e->emitter_count;
  s[NG_STAT_EFFECTIVE_RATE] = e->target[NG_CONTROL_GRAIN_RATE] == 0 ||
                                      e->reset_stage == 1 || e->reset_stage == 2
                                  ? 0
                                  : e->effective_rate;
  s[NG_STAT_KINETIC_ENERGY] = e->field.energy;
  s[NG_STAT_RMS_VORTICITY] = e->field.rms_omega;
  s[NG_STAT_RMS_STRAIN] = e->field.rms_strain;
  s[NG_STAT_RMS_DIVERGENCE] = e->field.rms_divergence;
  s[NG_STAT_MAX_DIVERGENCE] = e->field.max_divergence;
  s[NG_STAT_FIELD_AGE_MS] = 1000 * (double)(e->frame - e->field_frame) / e->sr;
  s[NG_STAT_SNAPSHOT_SEQUENCE] = display_counter(e->field.sequence);
  s[NG_STAT_VOICE_DROPS] = display_counter(e->counters.voice_drops);
  s[NG_STAT_CAP_DROPS] = display_counter(e->counters.cap_drops);
  s[NG_STAT_CONTROL_EVENTS] = display_counter(e->counters.control_events);
  s[NG_STAT_NUMERIC_INTERVENTIONS] = display_counter(
      e->counters.numeric_interventions + e->field.interventions +
      e->counters.particle_interventions);
  s[NG_STAT_EPOCH] = display_counter(e->counters.epoch);
  unsigned status = e->status;
  if (e->external_update) {
    status |= e->external.status;
    s[NG_STAT_BACKEND] = 1;
    s[NG_STAT_EPOCH] = display_counter(e->external.epoch);
    s[NG_STAT_SNAPSHOT_DROPS] = display_counter(e->external.drops);
  }
  if (e->field.interventions || e->counters.particle_interventions)
    status |= NG_STATUS_NUMERIC_INTERVENTION;
  if (e->target[NG_CONTROL_FREEZE] || e->target[NG_CONTROL_FLOW_SPEED] == 0)
    status |= NG_STATUS_FROZEN;
  if (e->counters.control_events)
    status |= NG_STATUS_CONTROLS_CLAMPED;
  if (e->counters.voice_drops)
    status |= NG_STATUS_VOICE_OVERFLOW;
  s[NG_STAT_STATUS] = status;
  s[NG_STAT_SOURCE_LENGTH] = (double)e->source_length;
  s[NG_STAT_PRE_LIMITER_PEAK] = e->peak;
  e->peak = 0;
}
NGCounters ng_counters(const NGEngine *e) {
  NGCounters counters = e->counters;
  counters.numeric_interventions +=
      e->field.interventions + counters.particle_interventions;
  return counters;
}

int ng_grain_state(const NGEngine *e, uint32_t slot, NGGrainState *out) {
  if (!e || !out || slot >= e->capacity || e->reset_stage == 2 ||
      !e->voices[slot].active)
    return 0;
  const NGVoice *v = &e->voices[slot];
  const NGParticle *p = &v->particle;
  *out = (NGGrainState){.id = v->id,
                        .age = v->age,
                        .length = v->length,
                        .x = p->x,
                        .y = p->y,
                        .path_x = p->path_x,
                        .path_y = p->path_y,
                        .velocity_x = p->vx,
                        .velocity_y = p->vy,
                        .simulation_time = p->time,
                        .speed = p->motion_speed*e->smooth[NG_CONTROL_FLOW_SPEED],
                        .frequency = v->increment*e->sr/(double)e->source_length,
                        .space_x=v->space[0],.space_y=v->space[1],.space_z=v->space[2],.pan=v->pan};
  return 1;
}

static void view_words(double *out, uint64_t value) {
  out[0] = (double)(uint32_t)value;
  out[1] = (double)(uint32_t)(value >> 32u);
}
int ng_view(const NGEngine *e, double *out, size_t count) {
  if (!e || !out || count < NG_VIEW_SIZE) return 0;
  memset(out, 0, NG_VIEW_SIZE * sizeof(*out));
  out[0] = 1; out[1] = NG_VIEW_SIZE;
  view_words(out + 2, e->counters.epoch);
  view_words(out + 4, e->frame);
  out[6] = e->simulation_time;
  out[7] = NG_VIEW_SIDE;
  out[10] = e->field.n;
  out[11] = e->field.valid && e->reset_stage != 2;
  view_words(out + 12, e->field.sequence);
  out[14] = e->field.time;
  /* Incremental reset clears storage over several blocks. Do not publish that
   * partial field or partially reseeded particle pool as a coherent snapshot. */
  if (e->reset_stage == 2) return 1;
  if (e->field.valid) {
    const double h = 1.0 / e->field.n;
    for (unsigned j = 0; j < NG_VIEW_SIDE; ++j)
      for (unsigned i = 0; i < NG_VIEW_SIDE; ++i) {
        double x = (i + .5) / NG_VIEW_SIDE, y = (j + .5) / NG_VIEW_SIDE;
        NGFieldSample f = ng_field_sample(&e->field, x, y);
        NGFieldSample xp = ng_field_sample(&e->field, x + h, y);
        NGFieldSample xm = ng_field_sample(&e->field, x - h, y);
        NGFieldSample yp = ng_field_sample(&e->field, x, y + h);
        NGFieldSample ym = ng_field_sample(&e->field, x, y - h);
        double *v = out + NG_VIEW_HEADER + (j * NG_VIEW_SIDE + i) * NG_VIEW_FIELD_STRIDE;
        v[0] = f.u; v[1] = f.v; v[2] = f.omega; v[3] = f.strain;
        v[4] = (xp.u - xm.u) / (2 * h); v[5] = (yp.u - ym.u) / (2 * h);
        v[6] = (xp.v - xm.v) / (2 * h); v[7] = (yp.v - ym.v) / (2 * h);
      }
  }
  unsigned grains = 0;
  for (uint32_t slot = 0; slot < e->capacity && grains < NG_VIEW_PARTICLES; ++slot) {
    NGGrainState g;
    if (!ng_grain_state(e, slot, &g)) continue;
    double *v = out + NG_VIEW_HEADER + NG_VIEW_SIDE * NG_VIEW_SIDE * NG_VIEW_FIELD_STRIDE + grains * NG_VIEW_GRAIN_STRIDE;
    view_words(v, g.id); v[2] = g.x; v[3] = g.y;
    v[4] = g.path_x; v[5] = g.path_y;
    v[6] = (double)g.age / g.length; v[7] = slot;
    ++grains;
  }
  out[8] = grains;
  unsigned emitters = e->emitter_count < NG_VIEW_PARTICLES ? e->emitter_count : NG_VIEW_PARTICLES;
  out[9] = emitters;
  double *v = out + NG_VIEW_HEADER + NG_VIEW_SIDE * NG_VIEW_SIDE * NG_VIEW_FIELD_STRIDE + NG_VIEW_PARTICLES * NG_VIEW_GRAIN_STRIDE;
  for (unsigned i = 0; i < emitters; ++i) {
    v[2 * i] = e->emitters[i].x; v[2 * i + 1] = e->emitters[i].y;
  }
  return 1;
}
