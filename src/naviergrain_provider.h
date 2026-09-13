#ifndef NAVIERGRAIN_PROVIDER_H
#define NAVIERGRAIN_PROVIDER_H
#include "naviergrain_core.h"
#include "naviergrain_transport.h"

/* Native prepared provider. One audio owner and one producer, no hidden thread.
 * Host starts/stops/joins its worker; all storage is host owned. */
typedef struct NGProvider NGProvider;
typedef struct {
  uint64_t epoch, reset_epoch, sequence, frame;
  double controls[NG_CONTROL_COUNT];
} NGCommand;
/* Split producer boundary for a separate browser WASM instance. Take returns
 * 0 empty, 1 current, 2 obsolete. Solve owns a separately prepared provider,
 * accepts strictly ordered complete commands, and writes a caller-owned packet.
 * Never call these concurrently with work() on the same provider. */
int ng_provider_take_command(NGProvider *provider, NGCommand *command);
int ng_provider_solve_command(NGProvider *provider, const NGCommand *command,
                              void *packet, size_t bytes);
size_t ng_provider_size(const NGConfig *config);
NGProvider *ng_provider_init(void *memory, size_t bytes, const NGConfig *config,
                             double sample_rate, uint64_t generation);
/* Audio preparation only. A provider cannot be claimed by two engines.
 * Release only after audio AND worker are stopped; never recycle generations. */
int ng_provider_attach(NGProvider *provider, NGEngine *engine);
void ng_provider_release(NGProvider *provider);
uint64_t ng_provider_key(const NGProvider *provider);
int ng_provider_claimed(const NGProvider *provider);
/* Optional audio-owner telemetry, six uint32 words: absolute acceptance frame,
 * epoch, sequence (low/high pairs). Prepared storage must outlive the provider.
 * No callbacks, packet copies, or allocations. NULL disables observation. */
void ng_provider_observe_acceptance(NGProvider *provider, uint32_t words[6]);
/* Producer only. Process at most one authoritative control command.
 * 0 = empty, 1 = processed (including obsolete epoch), -1 = publish/solve failed.
 * A missed command never causes an unbounded catch-up solve. */
int ng_provider_work(NGProvider *provider);
/* Alternative producer, NOT concurrent with ng_provider_work. Owned bytes only. */
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
NGPacketResult naviergrain_push_field(NGProvider *provider,
                                     const void *packet, size_t bytes);
/* Audio owner only, before the next resumed sample. Advances epoch and
 * discards outstanding commands/fields logically, never simulates wall time. */
int ng_provider_resume(NGProvider *provider);
typedef struct {
  uint64_t commands, command_drops, processed, obsolete, publish_failures,
      unsafe_fields, epoch, applied, last_target, last_control;
  double motion_gain, worker_simulation_time, mean_latency_ms, max_latency_ms;
  NGFieldQueueStats queue;
} NGProviderStats;
/* Quiescent only (after worker join and audio stop). */
NGProviderStats ng_provider_stats(const NGProvider *provider);

/* Plugin API table is versioned, obtained with dlsym("naviergrain_native_api").
 * Registry is per CSOUND, fixed at 16 entries; registration/unregistration must
 * occur while that host is stopped. key = config.instance_id, never a pointer.
 * Compile CSD, register prepared providers, start + silent pre-roll, start worker,
 * then attach callbacks. Stop callbacks, stop/join worker, turn off the instrument,
 * unregister, reset/destroy Csound, free provider. Recompile requires a newly prepared generation. */
typedef struct {
  unsigned version;
  size_t (*size)(const NGConfig *);
  NGProvider *(*prepare)(void *, size_t, const NGConfig *, double, uint64_t);
  int (*register_provider)(void *csound, NGProvider *);
  int (*unregister_provider)(void *csound, NGProvider *);
  int (*work)(NGProvider *);
  int (*resume)(NGProvider *);
  NGProviderStats (*stats)(const NGProvider *);
} NGNativeAPI;
#endif
