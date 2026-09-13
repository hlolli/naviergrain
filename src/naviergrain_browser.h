#ifndef NAVIERGRAIN_BROWSER_H
#define NAVIERGRAIN_BROWSER_H
#include "naviergrain_provider.h"

/* Private browser ABI v2. Host-local WASM offsets are never sent to workers.
 * Same-owner operations only; this mailbox is NOT shared memory.
 * A separate four-slot SAB queue crosses each worker boundary. */
#define NG_BROWSER_MAGIC 0x4e474231u
#define NG_BROWSER_HEADER 64u
#define NG_COMMAND_BYTES (64u + 8u * NG_CONTROL_COUNT)
#define NG_BROWSER_BYTES (NG_BROWSER_HEADER + 2u * NG_PACKET_MAX_BYTES)
typedef struct {
  uint32_t magic, version, bytes, mode, packet_bytes, command_bytes;
  uint32_t input_ready, output_ready, result, resume;
  uint32_t acceptance[6]; /* absolute audio frame, epoch, sequence: low/high */
  unsigned char input[NG_PACKET_MAX_BYTES];
  unsigned char output[NG_PACKET_MAX_BYTES];
} NGBrowserMailbox;
_Static_assert(sizeof(NGBrowserMailbox) == NG_BROWSER_BYTES, "mailbox layout");
void ng_browser_init(NGBrowserMailbox *, unsigned mode, unsigned n);
/* Audio mode: bounded ingress/command export, never a solve.
 * Solver mode: at most one complete solve. No allocator or wait in either.
 * The host sets input_ready after copying, clears output_ready after copying. */
void ng_browser_service(NGBrowserMailbox *, NGProvider *);
int ng_command_encode(void *, const NGCommand *, uint64_t key);
int ng_command_decode(const void *, NGCommand *, uint64_t key);
#endif
