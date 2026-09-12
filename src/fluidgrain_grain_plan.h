#ifndef FLUIDGRAIN_GRAIN_PLAN_H
#define FLUIDGRAIN_GRAIN_PLAN_H
#include "fluidgrain_core.h"
#include "fluidgrain_resampler.h"

/* Internal offline integration boundary, not a Csound opcode or wire ABI.
 * Caller prepares aligned storage once. One engine/plan pair is audio-owned;
 * only sealed descriptors and immutable source/filter resources may be read
 * by a renderer. The engine and its source must outlive the plan.
 *
 * Planning advances the actual clock, particles, scheduler, voices and overlap
 * state without convolution. It cannot be rolled back: a failed renderer must
 * retry this SAME sealed plan (CPU is always available), then commit it.
 * No second batch or ordinary sample can advance that engine before commit.
 * fg_sample returns silence without advancing if incorrectly called while a
 * plan is pending. Read final playback stats only after commit.
 * Controls may change between capture calls; they are never pre-integrated.
 */
#define FG_GRAIN_PLAN_MAX_FRAMES 512u
typedef struct FGGrainPlan FGGrainPlan;
typedef struct {
  uint64_t id;
  uint32_t slot;
  double phase, increment, pan, envelope;
} FGPlannedGrain;
typedef struct {
  uint64_t epoch;
  size_t offset;
  uint32_t count;
  double gain;
} FGPlannedFrame;
typedef struct {
  uint64_t start_frame;
  uint32_t frames, capacity;
  size_t grains;
  const FGPlannedFrame *frame;
  const FGPlannedGrain *grain;
} FGGrainPlanView;

typedef struct {
  const double *source;
  size_t source_length;
  const FGResampler *reader;
  double edge_samples;
  int loop;
} FGGrainResources;
/* Immutable, owner-local resources. No pointer is serialized. */
int fg_engine_grain_resources(const FGEngine *, FGGrainResources *);
int fg_grain_plan_resources(const FGGrainPlan *, FGGrainResources *);

size_t fg_grain_plan_size(const FGConfig *, uint32_t max_frames);
FGGrainPlan *fg_grain_plan_init(void *, size_t bytes, const FGConfig *,
                                uint32_t max_frames);
int fg_grain_plan_begin(FGEngine *, FGGrainPlan *);
int fg_grain_plan_sample(FGEngine *, FGGrainPlan *);
int fg_grain_plan_seal(FGEngine *, FGGrainPlan *);
int fg_grain_plan_view(const FGGrainPlan *, FGGrainPlanView *);
/* Reusable production C convolution fallback: no scheduler advance, allocation,
 * gain history or other mutation. Repeated renders of a sealed plan are exact.
 * Output is interleaved stereo. commit performs the same nonfinite muting and
 * peak/intervention accounting as fg_sample, and releases the pending plan. */
int fg_grain_plan_render(const FGGrainPlan *, double *output, size_t samples);
int fg_grain_plan_commit(FGEngine *, FGGrainPlan *, double *output,
                          size_t samples);
#endif
