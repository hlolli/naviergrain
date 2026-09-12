#ifndef FLUIDGRAIN_PROVIDER_H
#define FLUIDGRAIN_PROVIDER_H
#include "fluidgrain_core.h"
#include "fluidgrain_transport.h"

/* Native prepared provider. One audio owner and one producer, no hidden thread.
 * Host starts/stops/joins its worker; all storage is host owned. */
typedef struct FGProvider FGProvider;
typedef struct {
  uint64_t epoch, reset_epoch, sequence, frame;
  double controls[FG_CONTROL_COUNT];
} FGCommand;
/* Split producer boundary for a separate browser WASM instance. Take returns
 * 0 empty, 1 current, 2 obsolete. Solve owns a separately prepared provider,
 * accepts strictly ordered complete commands, and writes a caller-owned packet.
 * Never call these concurrently with work() on the same provider. */
int fg_provider_take_command(FGProvider *provider, FGCommand *command);
int fg_provider_solve_command(FGProvider *provider, const FGCommand *command,
                              void *packet, size_t bytes);
size_t fg_provider_size(const FGConfig *config);
FGProvider *fg_provider_init(void *memory, size_t bytes, const FGConfig *config,
                             double sample_rate, uint64_t generation);
/* Audio preparation only. A provider cannot be claimed by two engines.
 * Release only after audio AND worker are stopped; never recycle generations. */
int fg_provider_attach(FGProvider *provider, FGEngine *engine);
void fg_provider_release(FGProvider *provider);
uint64_t fg_provider_key(const FGProvider *provider);
int fg_provider_claimed(const FGProvider *provider);
/* Optional audio-owner telemetry, six uint32 words: absolute acceptance frame,
 * epoch, sequence (low/high pairs). Prepared storage must outlive the provider.
 * No callbacks, packet copies, or allocations. NULL disables observation. */
void fg_provider_observe_acceptance(FGProvider *provider, uint32_t words[6]);
/* Producer only. Process at most one authoritative control command.
 * 0 = empty, 1 = processed (including obsolete epoch), -1 = publish/solve failed.
 * A missed command never causes an unbounded catch-up solve. */
int fg_provider_work(FGProvider *provider);
/* Alternative producer, NOT concurrent with fg_provider_work. Owned bytes only. */
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
FGPacketResult fluidgrain_push_field(FGProvider *provider,
                                     const void *packet, size_t bytes);
/* Audio owner only, before the next resumed sample. Advances epoch and
 * discards outstanding commands/fields logically, never simulates wall time. */
int fg_provider_resume(FGProvider *provider);
typedef struct {
  uint64_t commands, command_drops, processed, obsolete, publish_failures,
      unsafe_fields, epoch, applied, last_target, last_control;
  double motion_gain, worker_simulation_time, mean_latency_ms, max_latency_ms;
  FGFieldQueueStats queue;
} FGProviderStats;
/* Quiescent only (after worker join and audio stop). */
FGProviderStats fg_provider_stats(const FGProvider *provider);

/* Plugin API table is versioned, obtained with dlsym("fluidgrain_native_api").
 * Registry is per CSOUND, fixed at 16 entries; registration/unregistration must
 * occur while that host is stopped. key = config.instance_id, never a pointer.
 * Compile CSD, register prepared providers, start + silent pre-roll, start worker,
 * then attach callbacks. Stop callbacks, stop/join worker, turn off the instrument,
 * unregister, reset/destroy Csound, free provider. Recompile requires a newly prepared generation. */
typedef struct {
  unsigned version;
  size_t (*size)(const FGConfig *);
  FGProvider *(*prepare)(void *, size_t, const FGConfig *, double, uint64_t);
  int (*register_provider)(void *csound, FGProvider *);
  int (*unregister_provider)(void *csound, FGProvider *);
  int (*work)(FGProvider *);
  int (*resume)(FGProvider *);
  FGProviderStats (*stats)(const FGProvider *);
} FGNativeAPI;
#endif
