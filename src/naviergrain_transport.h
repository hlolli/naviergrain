#ifndef NAVIERGRAIN_TRANSPORT_H
#define NAVIERGRAIN_TRANSPORT_H
#include "naviergrain_field.h"

/* Wire ABI, not a serialized C struct. See tests/test_packet_wire.py.
 * All integers and IEEE floats are little endian; timestamps remain uint64. */
#define NG_PACKET_VERSION 1u
#define NG_PACKET_HEADER 128u
#define NG_PACKET_MAX_GRID 64u
#define NG_PACKET_MAX_BYTES (NG_PACKET_HEADER + 16u * NG_PACKET_MAX_GRID * NG_PACKET_MAX_GRID)
#define NG_FIELD_QUEUE_SLOTS 4u
typedef enum {
  NG_PACKET_OK, NG_PACKET_EMPTY, NG_PACKET_MALFORMED, NG_PACKET_NONFINITE,
  NG_PACKET_INCOMPATIBLE, NG_PACKET_INSTANCE, NG_PACKET_EPOCH,
  NG_PACKET_ORDER, NG_PACKET_FULL, NG_PACKET_RESULT_COUNT
} NGPacketResult;
typedef struct {
  uint64_t instance_id, epoch, sequence, audio_frame, interventions;
  unsigned n;
  double simulation_time;
  double energy, rms_omega, rms_strain, rms_divergence, max_divergence, max_speed;
} NGPacketInfo;

size_t ng_packet_size(unsigned n); /* 0 for unsupported grids */
/* Encode a complete valid field. Destination untouched on failure; source,
 * packet and output storage must not overlap. Decode validates before writing
 * any published field or metadata, and leaves solver scratch/forcing untouched. */
NGPacketResult ng_packet_encode(void *packet, size_t bytes, const NGField *field,
                                uint64_t instance_id, uint64_t epoch,
                                uint64_t audio_frame);
NGPacketResult ng_packet_inspect(const void *packet, size_t bytes, NGPacketInfo *info);
NGPacketResult ng_packet_decode(const void *packet, size_t bytes, NGField *field,
                                NGPacketInfo *info);

typedef struct NGFieldQueue NGFieldQueue;
size_t ng_field_queue_size(void);
/* malloc-aligned caller-owned memory. Prepare and bind before starting threads.
 * Returns NULL if arguments or lock-free atomic support are unsuitable.
 * No runtime allocation, Csound call, or wait in any transport operation. */
NGFieldQueue *ng_field_queue_init(void *memory, size_t bytes,
                                  uint64_t instance_id, uint64_t epoch, unsigned n);
/* Exactly one producer. Copies caller-owned immutable bytes. Successful push
 * means published, NOT validated/applied. Full queues drop NEW packets. */
NGPacketResult ng_field_queue_push(NGFieldQueue *queue, const void *packet, size_t bytes);
/* Exactly one consumer. Inspects at most four slots, retains future fields,
 * applies newest eligible complete field to prepared storage, then frees slots.
 * audio_frame must not decrease within an epoch. No concurrent field access. */
NGPacketResult ng_field_queue_consume(NGFieldQueue *queue, uint64_t audio_frame,
                                      NGField *field, NGPacketInfo *info);
/* Consumer only; strictly increasing epochs. Does not reset producer storage,
 * so an old publication already in flight will be rejected safely. */
NGPacketResult ng_field_queue_epoch(NGFieldQueue *queue, uint64_t epoch);
typedef struct {
  uint64_t published, full, bad_size; /* producer-owned */
  uint64_t rejected[NG_PACKET_RESULT_COUNT]; /* consumer-owned */
  uint64_t applied, superseded;
} NGFieldQueueStats;
/* Read ONLY when producer and consumer are quiescent (e.g. after join). */
NGFieldQueueStats ng_field_queue_stats(const NGFieldQueue *queue);
#endif
