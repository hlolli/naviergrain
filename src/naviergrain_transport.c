#include "naviergrain_transport.h"
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128, "packet ABI needs IEEE binary32");
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "packet ABI needs IEEE binary64");
static uint32_t read32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const unsigned char *p) {
  return read32(p) | (uint64_t)read32(p + 4) << 32;
}
static void write32(unsigned char *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
static void write64(unsigned char *p, uint64_t v) {
  write32(p, (uint32_t)v); write32(p + 4, (uint32_t)(v >> 32));
}
static double read_float(const unsigned char *p) {
  uint32_t word = read32(p); float v; memcpy(&v, &word, 4); return v;
}
static double read_double(const unsigned char *p) {
  uint64_t word = read64(p); double v; memcpy(&v, &word, 8); return v;
}
static void write_float(unsigned char *p, double v) {
  float single = (float)v; uint32_t word; memcpy(&word, &single, 4);
  write32(p, word);
}
static void write_double(unsigned char *p, double v) {
  uint64_t word; memcpy(&word, &v, 8); write64(p, word);
}
size_t ng_packet_size(unsigned n) {
  return n == 16 || n == 32 || n == 64 ? NG_PACKET_HEADER + 16u * (size_t)n * n : 0;
}
static int field_storage(const NGField *f) {
  return f && ng_packet_size(f->n) && f->u && f->v && f->omega && f->strain;
}
NGPacketResult ng_packet_encode(void *packet, size_t bytes, const NGField *f,
                                uint64_t instance_id, uint64_t epoch,
                                uint64_t audio_frame) {
  if (!packet || !field_storage(f) || bytes != ng_packet_size(f->n) ||
      !instance_id || !epoch || !f->sequence || !f->valid)
    return NG_PACKET_MALFORMED;
  if (!isfinite(f->time)) return NG_PACKET_NONFINITE;
  if (f->time < 0) return NG_PACKET_MALFORMED;
  double diagnostics[] = { f->energy, f->rms_omega, f->rms_strain,
    f->rms_divergence, f->max_divergence, f->max_speed };
  for (unsigned i = 0; i < 6; ++i) {
    if (!isfinite(diagnostics[i]) || diagnostics[i] > FLT_MAX)
      return NG_PACKET_NONFINITE;
    if (diagnostics[i] < 0) return NG_PACKET_MALFORMED;
  }
  const double *arrays[] = { f->u, f->v, f->omega, f->strain };
  size_t count = (size_t)f->n * f->n;
  /* Validate the entire source before any write or narrowing conversion. */
  for (unsigned a = 0; a < 4; ++a)
    for (size_t i = 0; i < count; ++i) {
      double v = arrays[a][i];
      if (!isfinite(v) || fabs(v) > FLT_MAX) return NG_PACKET_NONFINITE;
      if (a == 3 && v < 0) return NG_PACKET_MALFORMED;
    }
  unsigned char *p = packet;
  memset(p, 0, NG_PACKET_HEADER);
  memcpy(p, "NGFL", 4);
  write32(p + 4, NG_PACKET_VERSION);
  write32(p + 8, NG_PACKET_HEADER);
  write32(p + 12, (uint32_t)(bytes - NG_PACKET_HEADER));
  write64(p + 16, instance_id); write64(p + 24, epoch);
  write64(p + 32, f->sequence); write64(p + 40, audio_frame);
  write_double(p + 48, f->time);
  write32(p + 56, f->n); write32(p + 60, f->n);
  write32(p + 64, 1); write32(p + 68, 1);
  for (unsigned i = 0; i < 6; ++i) write_float(p + 72 + 4 * i, diagnostics[i]);
  write64(p + 96, f->interventions);
  for (unsigned a = 0; a < 4; ++a)
    for (size_t i = 0; i < count; ++i)
      write_float(p + NG_PACKET_HEADER + 4 * (a * count + i), arrays[a][i]);
  return NG_PACKET_OK;
}
NGPacketResult ng_packet_inspect(const void *packet, size_t bytes, NGPacketInfo *info) {
  if (!packet || !info || bytes < NG_PACKET_HEADER || bytes > NG_PACKET_MAX_BYTES)
    return NG_PACKET_MALFORMED;
  const unsigned char *p = packet;
  if (memcmp(p, "NGFL", 4) || read32(p + 8) != NG_PACKET_HEADER ||
      read32(p + 12) != bytes - NG_PACKET_HEADER)
    return NG_PACKET_MALFORMED;
  if (read32(p + 4) != NG_PACKET_VERSION || read32(p + 64) != 1 ||
      read32(p + 68) != 1) return NG_PACKET_INCOMPATIBLE;
  unsigned n = read32(p + 56);
  if (!ng_packet_size(n) || read32(p + 60) != n) return NG_PACKET_INCOMPATIBLE;
  if (bytes != ng_packet_size(n)) return NG_PACKET_MALFORMED;
  for (unsigned i = 104; i < NG_PACKET_HEADER; ++i)
    if (p[i]) return NG_PACKET_MALFORMED;
  NGPacketInfo m = {
    .instance_id = read64(p + 16), .epoch = read64(p + 24),
    .sequence = read64(p + 32), .audio_frame = read64(p + 40),
    .simulation_time = read_double(p + 48), .n = n,
    .energy = read_float(p + 72), .rms_omega = read_float(p + 76),
    .rms_strain = read_float(p + 80), .rms_divergence = read_float(p + 84),
    .max_divergence = read_float(p + 88), .max_speed = read_float(p + 92),
    .interventions = read64(p + 96)
  };
  if (!m.instance_id || !m.epoch || !m.sequence) return NG_PACKET_MALFORMED;
  if (!isfinite(m.simulation_time)) return NG_PACKET_NONFINITE;
  if (m.simulation_time < 0) return NG_PACKET_MALFORMED;
  for (unsigned i = 72; i < 96; i += 4) {
    double v = read_float(p + i);
    if (!isfinite(v)) return NG_PACKET_NONFINITE;
    if (v < 0) return NG_PACKET_MALFORMED;
  }
  size_t strain_offset = NG_PACKET_HEADER + 12u * (size_t)n * n;
  for (size_t i = NG_PACKET_HEADER; i < bytes; i += 4) {
    double v = read_float(p + i);
    if (!isfinite(v)) return NG_PACKET_NONFINITE;
    if (i >= strain_offset && v < 0) return NG_PACKET_MALFORMED;
  }
  *info = m;
  return NG_PACKET_OK;
}
static void apply_packet(const unsigned char *p, const NGPacketInfo *m, NGField *f) {
  double *arrays[] = { f->u, f->v, f->omega, f->strain };
  size_t count = (size_t)m->n * m->n;
  for (unsigned a = 0; a < 4; ++a)
    for (size_t i = 0; i < count; ++i)
      arrays[a][i] = read_float(p + NG_PACKET_HEADER + 4 * (a * count + i));
  f->sequence = m->sequence; f->time = m->simulation_time;
  f->energy = m->energy; f->rms_omega = m->rms_omega; f->rms_strain = m->rms_strain;
  f->rms_divergence = m->rms_divergence; f->max_divergence = m->max_divergence;
  f->max_speed = m->max_speed; f->interventions = m->interventions; f->valid = 1;
}
NGPacketResult ng_packet_decode(const void *packet, size_t bytes, NGField *f,
                                NGPacketInfo *info) {
  if (!field_storage(f) || !info) return NG_PACKET_MALFORMED;
  NGPacketInfo m;
  NGPacketResult result = ng_packet_inspect(packet, bytes, &m);
  if (result != NG_PACKET_OK) return result;
  if (m.n != f->n) return NG_PACKET_INCOMPATIBLE;
  apply_packet(packet, &m, f); *info = m;
  return NG_PACKET_OK;
}

typedef struct {
  atomic_uint ready;
  /* Written by producer before release publication; immutable until consumer
   * release. Not a seqlock: ordinary bytes never have concurrent writers/readers. */
  uint64_t ticket;
  size_t bytes;
  unsigned char packet[NG_PACKET_MAX_BYTES];
  /* Consumer-only cache, cleared BEFORE releasing slot. */
  int checked;
  NGPacketInfo info;
} NGFieldSlot;
struct NGFieldQueue {
  NGFieldSlot slots[NG_FIELD_QUEUE_SLOTS];
  uint64_t instance_id;
  unsigned n;
  atomic_uint_fast64_t published_ticket;
  uint64_t epoch, last_sequence, last_frame, clock;
  double last_time; /* consumer only */
  NGFieldQueueStats stats; /* disjoint members, read jointly only at quiescence */
};
size_t ng_field_queue_size(void) { return sizeof(NGFieldQueue); }
NGFieldQueue *ng_field_queue_init(void *memory, size_t bytes,
                                  uint64_t instance_id, uint64_t epoch, unsigned n) {
  if (!memory || bytes < sizeof(NGFieldQueue) || !instance_id || !epoch ||
      !ng_packet_size(n)) return NULL;
  NGFieldQueue *q = memory;
  memset(q, 0, sizeof(*q));
  atomic_init(&q->published_ticket, 0);
  if (!atomic_is_lock_free(&q->published_ticket)) return NULL;
  for (unsigned i = 0; i < NG_FIELD_QUEUE_SLOTS; ++i) {
    atomic_init(&q->slots[i].ready, 0);
    if (!atomic_is_lock_free(&q->slots[i].ready)) return NULL;
  }
  q->instance_id = instance_id; q->epoch = epoch; q->n = n;
  return q;
}
NGPacketResult ng_field_queue_push(NGFieldQueue *q, const void *packet, size_t bytes) {
  if (!q) return NG_PACKET_MALFORMED;
  if (!packet || bytes < NG_PACKET_HEADER || bytes > NG_PACKET_MAX_BYTES) {
    ++q->stats.bad_size; return NG_PACKET_MALFORMED;
  }
  /* Never wrap publication ordering (requires a new prepared queue). */
  uint64_t ticket = atomic_load_explicit(&q->published_ticket, memory_order_relaxed);
  if (ticket == UINT64_MAX) return NG_PACKET_ORDER;
  for (unsigned i = 0; i < NG_FIELD_QUEUE_SLOTS; ++i) {
    NGFieldSlot *s = &q->slots[i];
    if (atomic_load_explicit(&s->ready, memory_order_acquire)) continue;
    memcpy(s->packet, packet, bytes);
    s->bytes = bytes; s->ticket = ticket + 1;
    atomic_store_explicit(&s->ready, 1, memory_order_release);
    atomic_store_explicit(&q->published_ticket, ticket + 1, memory_order_release);
    ++q->stats.published;
    return NG_PACKET_OK;
  }
  ++q->stats.full; return NG_PACKET_FULL;
}
static void release_slot(NGFieldSlot *s) {
  s->checked = 0;
  atomic_store_explicit(&s->ready, 0, memory_order_release);
}
NGPacketResult ng_field_queue_epoch(NGFieldQueue *q, uint64_t epoch) {
  if (!q || epoch <= q->epoch) return NG_PACKET_EPOCH;
  q->epoch = epoch; q->last_sequence = q->last_frame = q->clock = 0;
  q->last_time = 0;
  /* No payload writes or producer reset; old in-flight packets are rejected. */
  for (unsigned i = 0; i < NG_FIELD_QUEUE_SLOTS; ++i) q->slots[i].checked = 0;
  return NG_PACKET_OK;
}
NGPacketResult ng_field_queue_consume(NGFieldQueue *q, uint64_t audio_frame,
                                      NGField *f, NGPacketInfo *info) {
  if (!q || !field_storage(f) || !info) return NG_PACKET_MALFORMED;
  if (q->n != f->n) return NG_PACKET_INCOMPATIBLE;
  if (audio_frame < q->clock) {
    ++q->stats.rejected[NG_PACKET_ORDER]; return NG_PACKET_ORDER;
  }
  q->clock = audio_frame;
  NGFieldSlot *ready[NG_FIELD_QUEUE_SLOTS];
  unsigned count = 0;
  /* A publication cutoff makes this bounded scan a consistent snapshot even
   * when the producer publishes into an already-scanned slot. Later tickets
   * wait until the next consume; no older eligible packet is lost to a newer
   * future packet arriving halfway through this scan. */
  uint64_t cutoff = atomic_load_explicit(&q->published_ticket, memory_order_acquire);
  for (unsigned i = 0; i < NG_FIELD_QUEUE_SLOTS; ++i) {
    NGFieldSlot *s = &q->slots[i];
    if (!atomic_load_explicit(&s->ready, memory_order_acquire)) continue;
    if (s->ticket > cutoff) continue;
    /* Slot reuse need not follow array order. Sort at most four publications. */
    unsigned j = count++;
    while (j && ready[j - 1]->ticket > s->ticket) {
      ready[j] = ready[j - 1]; --j;
    }
    ready[j] = s;
  }
  NGFieldSlot *chosen = NULL;
  for (unsigned i = 0; i < count; ++i) {
    NGFieldSlot *s = ready[i];
    if (!s->checked) {
      NGPacketResult result = ng_packet_inspect(s->packet, s->bytes, &s->info);
      if (result == NG_PACKET_OK) {
        if (s->info.instance_id != q->instance_id) result = NG_PACKET_INSTANCE;
        else if (s->info.epoch != q->epoch) result = NG_PACKET_EPOCH;
        else if (s->info.n != q->n) result = NG_PACKET_INCOMPATIBLE;
        else if (s->info.sequence <= q->last_sequence ||
                 s->info.audio_frame < q->last_frame ||
                 s->info.simulation_time < q->last_time) result = NG_PACKET_ORDER;
      }
      if (result != NG_PACKET_OK) {
        ++q->stats.rejected[result]; release_slot(s); continue;
      }
      q->last_sequence = s->info.sequence; q->last_frame = s->info.audio_frame;
      q->last_time = s->info.simulation_time; s->checked = 1;
    }
    if (s->info.audio_frame > audio_frame) continue;
    if (chosen) { ++q->stats.superseded; release_slot(chosen); }
    chosen = s;
  }
  if (!chosen) return NG_PACKET_EMPTY;
  /* Producer cannot reuse chosen until this entire conversion/copy finishes. */
  apply_packet(chosen->packet, &chosen->info, f); *info = chosen->info;
  release_slot(chosen); ++q->stats.applied;
  return NG_PACKET_OK;
}
NGFieldQueueStats ng_field_queue_stats(const NGFieldQueue *q) { return q->stats; }
