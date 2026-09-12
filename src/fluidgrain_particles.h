#ifndef FLUIDGRAIN_PARTICLES_H
#define FLUIDGRAIN_PARTICLES_H
#include "fluidgrain_field.h"
typedef struct {
  double x, y, path_x, path_y, vx, vy, time;
  double omega, strain, speed, motion_speed;
} FGParticle;
void fg_particle_observe(const FGField *field, FGParticle *p);
/* Bounded exponential-midpoint RK2; dt is this particle's actual elapsed
 * simulation time. The field must be a finite published snapshot. Damaged
 * particle state recovers locally; non-finite dt skips motion. The owner
 * re-anchors time. Returns the number of safety interventions. */
unsigned fg_particle_advance(const FGField *field, FGParticle *p, double dt,
                             double inertia_ms, double attraction);
#endif
