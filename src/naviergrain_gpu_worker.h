#ifndef NAVIERGRAIN_GPU_WORKER_H
#define NAVIERGRAIN_GPU_WORKER_H
#include "naviergrain_core.h"

#define NG_GPU_WORKER_SLOTS 4u
typedef struct NGGPUWorker NGGPUWorker;
typedef struct {
  uint64_t sequence, first_frame;
  uint32_t frames;
  int gpu_active, ok;
  double stats[NG_STAT_COUNT];
} NGGPUResult;
/* Prepare/destroy on a non-audio thread. Owns a COPY of source/config and a
 * private engine. Internal fluid backend only; all synthesis and GPU calls
 * (including exact failed-plan recovery) execute on the worker thread.
 * Destroy joins outstanding device work: it is deliberately NOT callback-safe.
 * No GPU device reset, cancellation of shared device work, or live guarantee. */
NGGPUWorker *ng_gpu_worker_create(const NGConfig *, const double *source,
  size_t length, double source_sr, double engine_sr, uint32_t max_frames,
  int device);
void ng_gpu_worker_destroy(NGGPUWorker *);
/* Exactly one host thread calls BOTH submit and read, serially. No locks,
 * waits, allocation, synthesis or GPU calls. Results are FIFO and occupy a
 * slot until read. Returns 1 accepted/read, 0 full/not ready, -1 invalid.
 * A rejected submit changes nothing (including reset/enable). Retry the same
 * controls if the event must be retained. enable=0 permanently selects CPU
 * when that accepted request reaches the worker. Controls held for frames.
 * No silence substitution, overwriting or hidden scheduler advancement:
 * the host must implement its own buffering/underrun and latency policy. */
int ng_gpu_worker_submit(NGGPUWorker *, const double controls[NG_CONTROL_COUNT],
  uint32_t frames, int enable);
/* On 0/-1, caller buffers are untouched and no result is consumed. */
int ng_gpu_worker_read(NGGPUWorker *, double *stereo, size_t samples,
  NGGPUResult *result);
/* Optional plugin export: naviergrain_native_gpu_api(). The host keeps the
 * module loaded until every worker is destroyed; no Csound pointer is shared.
 * Obtain this versioned table using the same native-loader pattern as the
 * existing naviergrain_native_api() provider binding. */
typedef struct {
  unsigned version;
  NGGPUWorker *(*create)(const NGConfig *, const double *, size_t, double,
    double, uint32_t, int);
  void (*destroy)(NGGPUWorker *);
  int (*submit)(NGGPUWorker *, const double *, uint32_t, int);
  int (*read)(NGGPUWorker *, double *, size_t, NGGPUResult *);
} NGNativeGPUWorkerAPI;
#endif
