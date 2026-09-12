#ifndef FLUIDGRAIN_CORE_H
#define FLUIDGRAIN_CORE_H
#include "fluidgrain_schema.h"
#include "fluidgrain_field.h"
#include <stddef.h>
#include <stdint.h>

typedef struct FGEngine FGEngine;
typedef struct {
  double value[FG_CONFIG_COUNT];
} FGConfig;

void fg_defaults(double *config, double *controls);
/* Return NULL on success; otherwise a stable diagnostic string. */
const char *fg_config_parse(FGConfig *out, const double *values, size_t count);
size_t fg_memory_size(const FGConfig *config, size_t source_length,
                      double source_sr, double engine_sr);
/* Memory is caller-owned, suitably aligned (malloc/AUXCH), and prepared
 * offline. */
FGEngine *fg_init(void *memory, size_t bytes, const FGConfig *config,
                  size_t source_length, double source_sr, double engine_sr);
/* Fill once before rendering, with validated finite mono samples. Never a guard
 * point. */
double *fg_source(FGEngine *engine);
/* Select a unit cosine cycle before rendering or GPU resource capture. Requires
 * looping source. Pitch in Hz is source_sr / source_length * pitch_ratio. */
int fg_source_cosine(FGEngine *engine);
/* Optional shared sound space for cosine grains. Enable before the first
 * frame; update bounds between batches, never while a plan awaits commit.
 * Bounds: 55..12000 Hz, ordered (equal bounds give a single carrier pitch).
 * The upper edge also stays below .45 * sample rate. Legacy sample/ratio
 * mapping remains the default for engines that do not opt into this mode. */
int fg_sound_space(FGEngine *engine,double low_hz,double high_hz);
void fg_sound_bounds(const FGEngine *engine,double out[2]);
void fg_controls(FGEngine *engine, const double values[FG_CONTROL_COUNT]);
void fg_sample(FGEngine *engine, double *left, double *right);
void fg_stats(FGEngine *engine, double values[FG_STAT_COUNT]);
/* Prepared provider hook. Audio-owner calls only; worker never receives engine
 * storage. Hook may replace the field only at a particle tick. */
typedef struct {
  uint64_t frame, reset_epoch;
  int fluid_tick, particle_tick, moving, resetting;
  const double *controls; /* authoritative, sanitized and smoothed */
} FGExternalClock;
typedef struct {
  double motion;
  uint64_t field_frame, epoch, drops;
  unsigned status;
} FGExternalState;
typedef FGExternalState (*FGExternalUpdate)(void *, const FGExternalClock *,
                                            FGField *);
int fg_bind_external(FGEngine *, const FGConfig *, double sample_rate,
                      void *owner, FGExternalUpdate update);
/* Exact native diagnostics; host transport is a later milestone. */
typedef struct {
  uint64_t births, voice_drops, cap_drops, control_events, epoch,
      numeric_interventions, particle_interventions;
  double discarded_hazard;
} FGCounters;
FGCounters fg_counters(const FGEngine *engine);
/* Read-only native inspection; an inactive/out-of-range pool slot returns 0.
 * No pointer to mutable engine storage is exported. */
typedef struct {
  uint64_t id;
  uint32_t age, length;
  double x, y, path_x, path_y, velocity_x, velocity_y, simulation_time, speed, frequency;
  double space_x,space_y,space_z,pan;
} FGGrainState;
int fg_grain_state(const FGEngine *engine, uint32_t slot, FGGrainState *out);
/* Read-only offline visualization, separate from the external-field packet ABI.
 * Fixed bounded double layout, decoded by web/visualizer.ts. Never call
 * concurrently with rendering. Returns 0 without writing on insufficient space. */
#define FG_VIEW_SIDE 16u
#define FG_VIEW_PARTICLES 128u
#define FG_VIEW_HEADER 16u
#define FG_VIEW_FIELD_STRIDE 8u
#define FG_VIEW_GRAIN_STRIDE 8u
#define FG_VIEW_SIZE (FG_VIEW_HEADER + FG_VIEW_SIDE * FG_VIEW_SIDE * FG_VIEW_FIELD_STRIDE + FG_VIEW_PARTICLES * (FG_VIEW_GRAIN_STRIDE + 2u))
int fg_view(const FGEngine *engine, double *out, size_t count);
#endif
