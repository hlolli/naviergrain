#ifndef NAVIERGRAIN_FIELD_H
#define NAVIERGRAIN_FIELD_H
#include "naviergrain_schema.h"
#include <stddef.h>
#include <stdint.h>

/* Periodic MAC grid: u(i,j) at (i,j+.5)h, v(i,j) at (i+.5,j)h.
 * Published arrays change only after a complete, finite step. */
#define NG_FIELD_ARRAYS 16u
typedef struct {
  unsigned n;
  double *storage, *u, *v, *omega, *strain;
  double *au, *av, *tu, *tv, *rhs, *phi, *work, *psi, *fu, *fv, *cu, *cv;
  double time, phases[6];
  double energy, rms_omega, rms_strain, rms_divergence, max_divergence;
  double max_speed;
  uint64_t sequence, interventions;
  int valid;
} NGField;
typedef struct {
  double u, v, omega, strain;
} NGFieldSample;
size_t ng_field_doubles(unsigned n);
void ng_field_init(NGField *f, unsigned n, double *storage, uint32_t seed);
/* Reset metadata after the owner has incrementally zeroed storage. */
void ng_field_reset(NGField *f);
NGFieldSample ng_field_sample(const NGField *f, double x, double y);
void ng_field_velocity(const NGField *f, const double *u, const double *v,
                       double x, double y, double *vx, double *vy);
void ng_field_divergence(const NGField *f, const double *u, const double *v,
                         double *out);
void ng_field_gradient(const NGField *f, const double *p, double *u, double *v);
void ng_field_project(NGField *f, double *u, double *v, unsigned iterations);
void ng_field_diffuse(NGField *f, double *a, double coefficient,
                      unsigned iterations);
/* Returns 1 on publication, 0 on rejected numerical state. No allocation. */
int ng_field_step(NGField *f, const double controls[NG_CONTROL_COUNT],
                  double fluid_hz, double particle_hz,
                  unsigned pressure_iterations, unsigned viscosity_iterations);
#endif
