#ifndef FLUIDGRAIN_TRANSPORT_H
#define FLUIDGRAIN_TRANSPORT_H
#include "fluidgrain_field.h"

/* Wire ABI, not a serialized C struct. See tests/test_packet_wire.py.
 * All integers and IEEE floats are little endian; timestamps remain uint64. */
#define FG_PACKET_VERSION 1u
#define FG_PACKET_HEADER 128u
#define FG_PACKET_MAX_GRID 64u
#define FG_PACKET_MAX_BYTES (FG_PACKET_HEADER + 16u * FG_PACKET_MAX_GRID * FG_PACKET_MAX_GRID)
#define FG_FIELD_QUEUE_SLOTS 4u
typedef enum {
  FG_PACKET_OK, FG_PACKET_EMPTY, FG_PACKET_MALFORMED, FG_PACKET_NONFINITE,
  FG_PACKET_INCOMPATIBLE, FG_PACKET_INSTANCE, FG_PACKET_EPOCH,
  FG_PACKET_ORDER, FG_PACKET_FULL, FG_PACKET_RESULT_COUNT
} FGPacketResult;
typedef struct {
  uint64_t instance_id, epoch, sequence, audio_frame, interventions;
  unsigned n;
  double simulation_time;
  double energy, rms_omega, rms_strain, rms_divergence, max_divergence, max_speed;
} FGPacketInfo;

size_t fg_packet_size(unsigned n); /* 0 for unsupported grids */
/* Encode a complete valid field. Destination untouched on failure; source,
 * packet and output storage must not overlap. Decode validates before writing
 * any published field or metadata, and leaves solver scratch/forcing untouched. */
FGPacketResult fg_packet_encode(void *packet, size_t bytes, const FGField *field,
                                uint64_t instance_id, uint64_t epoch,
                                uint64_t audio_frame);
FGPacketResult fg_packet_inspect(const void *packet, size_t bytes, FGPacketInfo *info);
FGPacketResult fg_packet_decode(const void *packet, size_t bytes, FGField *field,
                                FGPacketInfo *info);

typedef struct FGFieldQueue FGFieldQueue;
size_t fg_field_queue_size(void);
/* malloc-aligned caller-owned memory. Prepare and bind before starting threads.
 * Returns NULL if arguments or lock-free atomic support are unsuitable.
 * No runtime allocation, Csound call, or wait in any transport operation. */
FGFieldQueue *fg_field_queue_init(void *memory, size_t bytes,
                                  uint64_t instance_id, uint64_t epoch, unsigned n);
/* Exactly one producer. Copies caller-owned immutable bytes. Successful push
 * means published, NOT validated/applied. Full queues drop NEW packets. */
FGPacketResult fg_field_queue_push(FGFieldQueue *queue, const void *packet, size_t bytes);
/* Exactly one consumer. Inspects at most four slots, retains future fields,
 * applies newest eligible complete field to prepared storage, then frees slots.
 * audio_frame must not decrease within an epoch. No concurrent field access. */
FGPacketResult fg_field_queue_consume(FGFieldQueue *queue, uint64_t audio_frame,
                                      FGField *field, FGPacketInfo *info);
/* Consumer only; strictly increasing epochs. Does not reset producer storage,
 * so an old publication already in flight will be rejected safely. */
FGPacketResult fg_field_queue_epoch(FGFieldQueue *queue, uint64_t epoch);
typedef struct {
  uint64_t published, full, bad_size; /* producer-owned */
  uint64_t rejected[FG_PACKET_RESULT_COUNT]; /* consumer-owned */
  uint64_t applied, superseded;
} FGFieldQueueStats;
/* Read ONLY when producer and consumer are quiescent (e.g. after join). */
FGFieldQueueStats fg_field_queue_stats(const FGFieldQueue *queue);
#endif
