#ifndef FLUIDGRAIN_BROWSER_H
#define FLUIDGRAIN_BROWSER_H
#include "fluidgrain_provider.h"

/* Private browser ABI v2. Host-local WASM offsets are never sent to workers.
 * Same-owner operations only; this mailbox is NOT shared memory.
 * A separate four-slot SAB queue crosses each worker boundary. */
#define FG_BROWSER_MAGIC 0x46474231u
#define FG_BROWSER_HEADER 64u
#define FG_COMMAND_BYTES (64u + 8u * FG_CONTROL_COUNT)
#define FG_BROWSER_BYTES (FG_BROWSER_HEADER + 2u * FG_PACKET_MAX_BYTES)
typedef struct {
  uint32_t magic, version, bytes, mode, packet_bytes, command_bytes;
  uint32_t input_ready, output_ready, result, resume;
  uint32_t acceptance[6]; /* absolute audio frame, epoch, sequence: low/high */
  unsigned char input[FG_PACKET_MAX_BYTES];
  unsigned char output[FG_PACKET_MAX_BYTES];
} FGBrowserMailbox;
_Static_assert(sizeof(FGBrowserMailbox) == FG_BROWSER_BYTES, "mailbox layout");
void fg_browser_init(FGBrowserMailbox *, unsigned mode, unsigned n);
/* Audio mode: bounded ingress/command export, never a solve.
 * Solver mode: at most one complete solve. No allocator or wait in either.
 * The host sets input_ready after copying, clears output_ready after copying. */
void fg_browser_service(FGBrowserMailbox *, FGProvider *);
int fg_command_encode(void *, const FGCommand *, uint64_t key);
int fg_command_decode(const void *, FGCommand *, uint64_t key);
#endif
