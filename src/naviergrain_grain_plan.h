#ifndef NAVIERGRAIN_GRAIN_PLAN_H
#define NAVIERGRAIN_GRAIN_PLAN_H
#include "naviergrain_core.h"
#include "naviergrain_resampler.h"

/* Internal offline integration boundary, not a Csound opcode or wire ABI.
 * Caller prepares aligned storage once. One engine/plan pair is audio-owned;
 * only sealed descriptors and immutable source/filter resources may be read
 * by a renderer. The engine and its source must outlive the plan.
 *
 * Planning advances the actual clock, particles, scheduler, voices and overlap
 * state without convolution. It cannot be rolled back: a failed renderer must
 * retry this SAME sealed plan (CPU is always available), then commit it.
 * No second batch or ordinary sample can advance that engine before commit.
 * ng_sample returns silence without advancing if incorrectly called while a
 * plan is pending. Read final playback stats only after commit.
 * Controls may change between capture calls; they are never pre-integrated.
 */
#define NG_GRAIN_PLAN_MAX_FRAMES 512u
typedef struct NGGrainPlan NGGrainPlan;
typedef struct {
  uint64_t id;
  uint32_t slot;
  double phase, increment, pan, envelope;
} NGPlannedGrain;
typedef struct {
  uint64_t epoch;
  size_t offset;
  uint32_t count;
  double gain;
} NGPlannedFrame;
typedef struct {
  uint64_t start_frame;
  uint32_t frames, capacity;
  size_t grains;
  const NGPlannedFrame *frame;
  const NGPlannedGrain *grain;
} NGGrainPlanView;

typedef struct {
  const double *source;
  size_t source_length;
  const NGResampler *reader;
  double edge_samples;
  int loop;
} NGGrainResources;
/* Immutable, owner-local resources. No pointer is serialized. */
int ng_engine_grain_resources(const NGEngine *, NGGrainResources *);
int ng_grain_plan_resources(const NGGrainPlan *, NGGrainResources *);

size_t ng_grain_plan_size(const NGConfig *, uint32_t max_frames);
NGGrainPlan *ng_grain_plan_init(void *, size_t bytes, const NGConfig *,
                                uint32_t max_frames);
int ng_grain_plan_begin(NGEngine *, NGGrainPlan *);
int ng_grain_plan_sample(NGEngine *, NGGrainPlan *);
int ng_grain_plan_seal(NGEngine *, NGGrainPlan *);
int ng_grain_plan_view(const NGGrainPlan *, NGGrainPlanView *);
/* Reusable production C convolution fallback: no scheduler advance, allocation,
 * gain history or other mutation. Repeated renders of a sealed plan are exact.
 * Output is interleaved stereo. commit performs the same nonfinite muting and
 * peak/intervention accounting as ng_sample, and releases the pending plan. */
int ng_grain_plan_render(const NGGrainPlan *, double *output, size_t samples);
int ng_grain_plan_commit(NGEngine *, NGGrainPlan *, double *output,
                          size_t samples);
#endif
