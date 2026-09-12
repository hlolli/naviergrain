#ifndef FLUIDGRAIN_BUFFERED_H
#define FLUIDGRAIN_BUFFERED_H
#include "fluidgrain_core.h"
typedef struct FGBuffered FGBuffered;
/* Host preparation API exported as fluidgrain_native_buffered_api().
 * Compile orchestra, create/register while host is stopped, start Csound and
 * perform one mode=0 preparation block BEFORE attaching audio callbacks.
 * One fresh binding per opcode instance; keys 1..16, batch >= ksmps.
 * Source/config are copied. Set kEnable before the preparation block.
 * Stop callbacks, reset/destroy Csound, then destroy the binding and unload the
 * plugin. Opcode deinit/reset never joins its worker. destroy returns 0 while
 * registered/claimed; unregister is allowed only after instrument deinit.
 * All API calls are non-audio host operations. No concurrent registry calls.
 *
 * aL,aR,kStats[],kGPU,kUnderruns,kQueued,kPlayed fluidgrain_buffered
 *     iKey,kControls[],kEnable,kMode
 * mode=0 pauses consumption/submission, 1 runs, 2 drains without submitting.
 * Keep the instrument alive in mode=2 until kQueued==0 to play the final tail.
 * Control values are held per batch, with up to four batches of lookahead.
 * Reset edges coalesce while awaiting submission; permanent CPU downgrade
 * is latched. Reset pulses have a separating accepted low batch.
 * Startup/rebuffer waits for two completed batches (drain accepts one).
 * Underruns insert a 32-sample fade and silence, then fade in retained FIFO
 * audio; no synthesis samples are discarded or replayed. kPlayed counts source
 * frames consumed, excluding underrun silence/fades. kQueued includes in-flight
 * frames. Stats/GPU flag describe the latest completed batch, not device time.
 * This is bounded buffered playback, NOT a sustained realtime qualification.
 */
typedef struct {
  unsigned version;
  FGBuffered *(*create)(const FGConfig *, const double *, size_t, double,
                       double, uint32_t, int);
  int (*destroy)(FGBuffered *);
  int (*register_buffer)(void *csound, unsigned key, FGBuffered *);
  int (*unregister_buffer)(void *csound, FGBuffered *);
} FGNativeBufferedAPI;
#endif
