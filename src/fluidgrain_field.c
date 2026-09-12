#include "fluidgrain_field.h"
#include <math.h>
#include <string.h>

#define TAU 6.28318530717958647692
static size_t cell(const FGField *f, int i, int j) {
  unsigned mask = f->n - 1;
  return (size_t)((unsigned)j & mask) * f->n + ((unsigned)i & mask);
}
static double bilinear(const FGField *f, const double *a, double x, double y,
                       double ox, double oy) {
  x = (x - floor(x)) * f->n - ox;
  y = (y - floor(y)) * f->n - oy;
  int i = (int)floor(x), j = (int)floor(y);
  double tx = x - i, ty = y - j;
  double lo = a[cell(f, i, j)] * (1 - tx) + a[cell(f, i + 1, j)] * tx;
  double hi = a[cell(f, i, j + 1)] * (1 - tx) + a[cell(f, i + 1, j + 1)] * tx;
  return lo * (1 - ty) + hi * ty;
}
size_t fg_field_doubles(unsigned n) { return (size_t)n * n * FG_FIELD_ARRAYS; }
void fg_field_reset(FGField *f) {
  f->time = f->energy = f->rms_omega = f->rms_strain = 0;
  f->rms_divergence = f->max_divergence = f->max_speed = 0;
  f->sequence = 0;
  f->valid = 1;
}
void fg_field_init(FGField *f, unsigned n, double *storage, uint32_t seed) {
  memset(f, 0, sizeof(*f));
  f->n = n;
  f->storage = storage;
  size_t count = (size_t)n * n;
  double **arrays[] = {&f->u,  &f->v,  &f->omega, &f->strain, &f->au,   &f->av,
                       &f->tu, &f->tv, &f->rhs,   &f->phi,    &f->work, &f->psi,
                       &f->fu, &f->fv, &f->cu,    &f->cv};
  for (unsigned i = 0; i < FG_FIELD_ARRAYS; ++i)
    *arrays[i] = storage + i * count;
  memset(storage, 0, fg_field_doubles(n) * sizeof(double));
  /* A forcing-only stream; independent of births, emitters and display counts.
   */
  uint32_t state = seed ^ UINT32_C(0xd1b54a35);
  for (unsigned i = 0; i < 6; ++i) {
    state = state * UINT32_C(747796405) + UINT32_C(2891336453);
    uint32_t word =
        ((state >> ((state >> 28u) + 4u)) ^ state) * UINT32_C(277803737);
    f->phases[i] = TAU * ((double)((word >> 22u) ^ word) + .5) / 4294967296.0;
  }
  fg_field_reset(f);
}
void fg_field_velocity(const FGField *f, const double *u, const double *v,
                       double x, double y, double *vx, double *vy) {
  *vx = bilinear(f, u, x, y, 0, .5);
  *vy = bilinear(f, v, x, y, .5, 0);
}
FGFieldSample fg_field_sample(const FGField *f, double x, double y) {
  FGFieldSample s;
  fg_field_velocity(f, f->u, f->v, x, y, &s.u, &s.v);
  s.omega = bilinear(f, f->omega, x, y, .5, .5);
  s.strain = bilinear(f, f->strain, x, y, .5, .5);
  return s;
}
void fg_field_divergence(const FGField *f, const double *u, const double *v,
                         double *out) {
  int n = (int)f->n;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      size_t k = cell(f, i, j);
      out[k] = n * (u[cell(f, i + 1, j)] - u[k] + v[cell(f, i, j + 1)] - v[k]);
    }
}
void fg_field_gradient(const FGField *f, const double *p, double *u,
                       double *v) {
  int n = (int)f->n;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      size_t k = cell(f, i, j);
      u[k] = n * (p[k] - p[cell(f, i - 1, j)]);
      v[k] = n * (p[k] - p[cell(f, i, j - 1)]);
    }
}
static void mean_free(double *a, size_t count) {
  double sum = 0;
  for (size_t k = 0; k < count; ++k)
    sum += a[k];
  double mean = sum / (double)count;
  for (size_t k = 0; k < count; ++k)
    a[k] -= mean;
}
void fg_field_project(FGField *f, double *u, double *v, unsigned iterations) {
  size_t count = (size_t)f->n * f->n;
  int n = (int)f->n;
  fg_field_divergence(f, u, v, f->rhs);
  mean_free(f->rhs, count);
  memset(f->phi, 0, count * sizeof(double));
  double *p = f->phi, *next = f->work;
  double h2 = 1.0 / (n * n);
  for (unsigned pass = 0; pass < iterations; ++pass) {
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        size_t k = cell(f, i, j);
        double sum = p[cell(f, i - 1, j)] + p[cell(f, i + 1, j)] +
                     p[cell(f, i, j - 1)] + p[cell(f, i, j + 1)];
        next[k] = p[k] / 3 + (sum - h2 * f->rhs[k]) / 6;
      }
    double *swap = p;
    p = next;
    next = swap;
  }
  mean_free(p, count);
  if (p != f->phi)
    memcpy(f->phi, p, count * sizeof(double));
  fg_field_gradient(f, f->phi, f->tu, f->tv);
  for (size_t k = 0; k < count; ++k) {
    u[k] -= f->tu[k];
    v[k] -= f->tv[k];
  }
}
void fg_field_diffuse(FGField *f, double *a, double coefficient,
                      unsigned iterations) {
  if (coefficient == 0)
    return;
  size_t count = (size_t)f->n * f->n;
  int n = (int)f->n;
  memcpy(f->rhs, a, count * sizeof(double));
  double *p = a, *next = f->work;
  for (unsigned pass = 0; pass < iterations; ++pass) {
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        size_t k = cell(f, i, j);
        double sum = p[cell(f, i - 1, j)] + p[cell(f, i + 1, j)] +
                     p[cell(f, i, j - 1)] + p[cell(f, i, j + 1)];
        next[k] = (f->rhs[k] + coefficient * sum) / (1 + 4 * coefficient);
      }
    double *swap = p;
    p = next;
    next = swap;
  }
  if (p != a)
    memcpy(a, p, count * sizeof(double));
}
static void advect(FGField *f, double dt) {
  int n = (int)f->n;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i)
      for (int component = 0; component < 2; ++component) {
        double x = (i + (component ? .5 : 0)) / n;
        double y = (j + (component ? 0 : .5)) / n;
        double vx, vy, mx, my;
        fg_field_velocity(f, f->u, f->v, x, y, &vx, &vy);
        fg_field_velocity(f, f->u, f->v, x - .5 * dt * vx, y - .5 * dt * vy,
                          &mx, &my);
        double *out = component ? f->av : f->au;
        out[cell(f, i, j)] =
            bilinear(f, component ? f->v : f->u, x - dt * mx, y - dt * my,
                     component ? .5 : 0, component ? 0 : .5);
      }
}
static void gradients(FGField *f, const double *u, const double *v,
                      double *omega, double *strain) {
  int n = (int)f->n;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      size_t k = cell(f, i, j);
      f->cu[k] = .5 * (u[k] + u[cell(f, i + 1, j)]);
      f->cv[k] = .5 * (v[k] + v[cell(f, i, j + 1)]);
    }
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      size_t k = cell(f, i, j);
      double ux = n * (u[cell(f, i + 1, j)] - u[k]);
      double vy = n * (v[cell(f, i, j + 1)] - v[k]);
      double uy =
          .5 * n * (f->cu[cell(f, i, j + 1)] - f->cu[cell(f, i, j - 1)]);
      double vx =
          .5 * n * (f->cv[cell(f, i + 1, j)] - f->cv[cell(f, i - 1, j)]);
      omega[k] = vx - uy;
      strain[k] = sqrt(2 * (ux * ux + vy * vy) + (uy + vx) * (uy + vx));
    }
}
static void add_basis(FGField *f, double amplitude) {
  int n = (int)f->n;
  size_t count = (size_t)n * (size_t)n;
  double power = 0;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      size_t k = cell(f, i, j);
      f->tu[k] = n * (f->psi[cell(f, i, j + 1)] - f->psi[k]);
      f->tv[k] = -n * (f->psi[cell(f, i + 1, j)] - f->psi[k]);
      power += f->tu[k] * f->tu[k] + f->tv[k] * f->tv[k];
    }
  double scale = amplitude / fmax(1e-12, sqrt(power / (double)count));
  for (size_t k = 0; k < count; ++k) {
    f->fu[k] += scale * f->tu[k];
    f->fv[k] += scale * f->tv[k];
  }
}
static void forcing(FGField *f, const double *c, double dt) {
  int n = (int)f->n;
  size_t count = (size_t)n * (size_t)n;
  memset(f->fu, 0, count * sizeof(double));
  memset(f->fv, 0, count * sizeof(double));
  /* sigma=eddy_size/2 in the local Gaussian approximation of the bump. */
  double kappa = 1 / pow(3.14159265358979323846 * c[FG_CONTROL_EDDY_SIZE], 2);
  static const int modes[6][2] = {{1, 1},  {1, -2}, {2, 1},
                                  {2, -3}, {3, 2},  {3, -3}};
  for (unsigned basis = 0; basis < 3; ++basis) {
    double amplitude =
        c[FG_CONTROL_DRIVE] * c[basis == 0   ? FG_CONTROL_SWIRL
                                : basis == 1 ? FG_CONTROL_TURBULENCE
                                             : FG_CONTROL_STRAIN_DRIVE];
    if (amplitude == 0)
      continue;
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        double x = (double)i / n, y = (double)j / n, value = 0;
        if (basis == 0) {
          double cy = cos(TAU * (y - .5));
          value = exp(kappa * (cos(TAU * (x - .33)) + cy - 2)) -
                  exp(kappa * (cos(TAU * (x - .67)) + cy - 2));
        } else if (basis == 1) {
          for (unsigned m = 0; m < 6; ++m) {
            double radius = hypot(modes[m][0], modes[m][1]);
            double weight =
                exp(-.5 * pow(radius * c[FG_CONTROL_EDDY_SIZE], 2)) / radius;
            value += weight *
                     sin(TAU * (modes[m][0] * x + modes[m][1] * y) +
                         f->phases[m] + (f->time + .5 * dt) * (.37 + .11 * m));
          }
        } else
          value = sin(TAU * x) * sin(TAU * y);
        f->psi[cell(f, i, j)] = value;
      }
    add_basis(f, amplitude);
  }
  if (c[FG_CONTROL_CONFINEMENT] > 0) {
    gradients(f, f->au, f->av, f->psi, f->rhs);
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        size_t k = cell(f, i, j);
        double nx =
            .5 * n *
            (fabs(f->psi[cell(f, i + 1, j)]) - fabs(f->psi[cell(f, i - 1, j)]));
        double ny =
            .5 * n *
            (fabs(f->psi[cell(f, i, j + 1)]) - fabs(f->psi[cell(f, i, j - 1)]));
        double scale = c[FG_CONTROL_CONFINEMENT] * f->psi[k] /
                       (n * (hypot(nx, ny) + 1e-12));
        f->tu[k] = scale * ny;
        f->tv[k] = -scale * nx;
      }
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        size_t k = cell(f, i, j);
        f->fu[k] += .5 * (f->tu[k] + f->tu[cell(f, i - 1, j)]);
        f->fv[k] += .5 * (f->tv[k] + f->tv[cell(f, i, j - 1)]);
      }
  }
}
static double speed_bound(const double *u, const double *v, size_t count) {
  double x = 0, y = 0;
  for (size_t k = 0; k < count; ++k) {
    if (!isfinite(u[k]) || !isfinite(v[k]))
      return INFINITY;
    x = fmax(x, fabs(u[k]));
    y = fmax(y, fabs(v[k]));
  }
  return hypot(x, y); /* also bounds face-aware interpolated velocities */
}
int fg_field_step(FGField *f, const double c[FG_CONTROL_COUNT], double fluid_hz,
                  double particle_hz, unsigned pressure_iterations,
                  unsigned viscosity_iterations) {
  if (c[FG_CONTROL_FREEZE] || c[FG_CONTROL_FLOW_SPEED] == 0)
    return 1;
  size_t count = (size_t)f->n * f->n;
  double dt = c[FG_CONTROL_FLOW_SPEED] / fluid_hz;
  advect(f, dt);
  double diffusion = c[FG_CONTROL_VISCOSITY] * dt * f->n * f->n;
  fg_field_diffuse(f, f->au, diffusion, viscosity_iterations);
  fg_field_diffuse(f, f->av, diffusion, viscosity_iterations);
  forcing(f, c, dt);
  double force_max = speed_bound(f->fu, f->fv, count);
  if (!isfinite(force_max))
    goto reject;
  double force_scale = fmin(1, 32 / fmax(force_max, 1e-12));
  if (force_scale < 1)
    ++f->interventions;
  for (size_t k = 0; k < count; ++k) {
    f->au[k] += dt * force_scale * f->fu[k];
    f->av[k] += dt * force_scale * f->fv[k];
  }
  fg_field_project(f, f->au, f->av, pressure_iterations);
  double max_speed = speed_bound(f->au, f->av, count);
  if (!isfinite(max_speed))
    goto reject;
  /* Half the four-substep travel budget remains available for particle drift.
   */
  double limit = 1.5 * particle_hz / (f->n * c[FG_CONTROL_FLOW_SPEED]);
  double scale = fmin(1, limit / fmax(max_speed, 1e-12));
  if (scale < 1)
    ++f->interventions;
  for (size_t k = 0; k < count; ++k) {
    f->au[k] *= scale;
    f->av[k] *= scale;
  }
  gradients(f, f->au, f->av, f->psi, f->rhs);
  double energy = 0, omega = 0, strain = 0;
  for (size_t k = 0; k < count; ++k) {
    energy += .5 * (f->au[k] * f->au[k] + f->av[k] * f->av[k]);
    omega += f->psi[k] * f->psi[k];
    strain += f->rhs[k] * f->rhs[k];
  }
  if (!isfinite(energy + omega + strain))
    goto reject;
  memcpy(f->u, f->au, count * sizeof(double));
  memcpy(f->v, f->av, count * sizeof(double));
  memcpy(f->omega, f->psi, count * sizeof(double));
  memcpy(f->strain, f->rhs, count * sizeof(double));
  f->energy = energy / (double)count;
  f->rms_omega = sqrt(omega / (double)count);
  f->rms_strain = sqrt(strain / (double)count);
  fg_field_divergence(f, f->u, f->v, f->rhs);
  double divergence = 0, maximum = 0;
  for (size_t k = 0; k < count; ++k) {
    divergence += f->rhs[k] * f->rhs[k];
    maximum = fmax(maximum, fabs(f->rhs[k]));
  }
  f->rms_divergence = sqrt(divergence / (double)count);
  f->max_divergence = maximum;
  f->max_speed = max_speed * scale;
  f->time += dt;
  ++f->sequence;
  f->valid = 1;
  return 1;
reject:
  ++f->interventions;
  f->valid = 0;
  return 0;
}
