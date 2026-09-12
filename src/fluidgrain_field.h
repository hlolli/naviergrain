#ifndef FLUIDGRAIN_FIELD_H
#define FLUIDGRAIN_FIELD_H
#include "fluidgrain_schema.h"
#include <stddef.h>
#include <stdint.h>

/* Periodic MAC grid: u(i,j) at (i,j+.5)h, v(i,j) at (i+.5,j)h.
 * Published arrays change only after a complete, finite step. */
#define FG_FIELD_ARRAYS 16u
typedef struct {
  unsigned n;
  double *storage, *u, *v, *omega, *strain;
  double *au, *av, *tu, *tv, *rhs, *phi, *work, *psi, *fu, *fv, *cu, *cv;
  double time, phases[6];
  double energy, rms_omega, rms_strain, rms_divergence, max_divergence;
  double max_speed;
  uint64_t sequence, interventions;
  int valid;
} FGField;
typedef struct {
  double u, v, omega, strain;
} FGFieldSample;
size_t fg_field_doubles(unsigned n);
void fg_field_init(FGField *f, unsigned n, double *storage, uint32_t seed);
/* Reset metadata after the owner has incrementally zeroed storage. */
void fg_field_reset(FGField *f);
FGFieldSample fg_field_sample(const FGField *f, double x, double y);
void fg_field_velocity(const FGField *f, const double *u, const double *v,
                       double x, double y, double *vx, double *vy);
void fg_field_divergence(const FGField *f, const double *u, const double *v,
                         double *out);
void fg_field_gradient(const FGField *f, const double *p, double *u, double *v);
void fg_field_project(FGField *f, double *u, double *v, unsigned iterations);
void fg_field_diffuse(FGField *f, double *a, double coefficient,
                      unsigned iterations);
/* Returns 1 on publication, 0 on rejected numerical state. No allocation. */
int fg_field_step(FGField *f, const double controls[FG_CONTROL_COUNT],
                  double fluid_hz, double particle_hz,
                  unsigned pressure_iterations, unsigned viscosity_iterations);
#endif
