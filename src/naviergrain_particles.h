#ifndef NAVIERGRAIN_PARTICLES_H
#define NAVIERGRAIN_PARTICLES_H
#include "naviergrain_field.h"
typedef struct {
  double x, y, path_x, path_y, vx, vy, time;
  double omega, strain, speed, motion_speed;
} NGParticle;
void ng_particle_observe(const NGField *field, NGParticle *p);
/* Bounded exponential-midpoint RK2; dt is this particle's actual elapsed
 * simulation time. The field must be a finite published snapshot. Damaged
 * particle state recovers locally; non-finite dt skips motion. The owner
 * re-anchors time. Returns the number of safety interventions. */
unsigned ng_particle_advance(const NGField *field, NGParticle *p, double dt,
                             double inertia_ms, double attraction);
#endif
