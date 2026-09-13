#ifndef NAVIERGRAIN_CORE_H
#define NAVIERGRAIN_CORE_H
#include "naviergrain_schema.h"
#include "naviergrain_field.h"
#include <stddef.h>
#include <stdint.h>

typedef struct NGEngine NGEngine;
typedef struct {
  double value[NG_CONFIG_COUNT];
} NGConfig;

void ng_defaults(double *config, double *controls);
/* Return NULL on success; otherwise a stable diagnostic string. */
const char *ng_config_parse(NGConfig *out, const double *values, size_t count);
size_t ng_memory_size(const NGConfig *config, size_t source_length,
                      double source_sr, double engine_sr);
/* Memory is caller-owned, suitably aligned (malloc/AUXCH), and prepared
 * offline. */
NGEngine *ng_init(void *memory, size_t bytes, const NGConfig *config,
                  size_t source_length, double source_sr, double engine_sr);
/* Fill once before rendering, with validated finite mono samples. Never a guard
 * point. */
double *ng_source(NGEngine *engine);
/* Select a unit cosine cycle before rendering or GPU resource capture. Requires
 * looping source. Pitch in Hz is source_sr / source_length * pitch_ratio. */
int ng_source_cosine(NGEngine *engine);
/* Optional shared sound space for cosine grains. Enable before the first
 * frame; update bounds between batches, never while a plan awaits commit.
 * Bounds: 55..12000 Hz, ordered (equal bounds give a single carrier pitch).
 * The upper edge also stays below .45 * sample rate. Legacy sample/ratio
 * mapping remains the default for engines that do not opt into this mode. */
int ng_sound_space(NGEngine *engine,double low_hz,double high_hz);
void ng_sound_bounds(const NGEngine *engine,double out[2]);
void ng_controls(NGEngine *engine, const double values[NG_CONTROL_COUNT]);
void ng_sample(NGEngine *engine, double *left, double *right);
void ng_stats(NGEngine *engine, double values[NG_STAT_COUNT]);
/* Prepared provider hook. Audio-owner calls only; worker never receives engine
 * storage. Hook may replace the field only at a particle tick. */
typedef struct {
  uint64_t frame, reset_epoch;
  int fluid_tick, particle_tick, moving, resetting;
  const double *controls; /* authoritative, sanitized and smoothed */
} NGExternalClock;
typedef struct {
  double motion;
  uint64_t field_frame, epoch, drops;
  unsigned status;
} NGExternalState;
typedef NGExternalState (*NGExternalUpdate)(void *, const NGExternalClock *,
                                            NGField *);
int ng_bind_external(NGEngine *, const NGConfig *, double sample_rate,
                      void *owner, NGExternalUpdate update);
/* Exact native diagnostics; host transport is a later milestone. */
typedef struct {
  uint64_t births, voice_drops, cap_drops, control_events, epoch,
      numeric_interventions, particle_interventions;
  double discarded_hazard;
} NGCounters;
NGCounters ng_counters(const NGEngine *engine);
/* Read-only native inspection; an inactive/out-of-range pool slot returns 0.
 * No pointer to mutable engine storage is exported. */
typedef struct {
  uint64_t id;
  uint32_t age, length;
  double x, y, path_x, path_y, velocity_x, velocity_y, simulation_time, speed, frequency;
  double space_x,space_y,space_z,pan;
} NGGrainState;
int ng_grain_state(const NGEngine *engine, uint32_t slot, NGGrainState *out);
/* Read-only offline visualization, separate from the external-field packet ABI.
 * Fixed bounded double layout, decoded by web/visualizer.ts. Never call
 * concurrently with rendering. Returns 0 without writing on insufficient space. */
#define NG_VIEW_SIDE 16u
#define NG_VIEW_PARTICLES 128u
#define NG_VIEW_HEADER 16u
#define NG_VIEW_FIELD_STRIDE 8u
#define NG_VIEW_GRAIN_STRIDE 8u
#define NG_VIEW_SIZE (NG_VIEW_HEADER + NG_VIEW_SIDE * NG_VIEW_SIDE * NG_VIEW_FIELD_STRIDE + NG_VIEW_PARTICLES * (NG_VIEW_GRAIN_STRIDE + 2u))
int ng_view(const NGEngine *engine, double *out, size_t count);
#endif
