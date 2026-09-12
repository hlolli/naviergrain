#include "fluidgrain_particles.h"
#include <math.h>
#define TAU 6.28318530717958647692
static unsigned bound(double *x, double *y, double limit) {
  double speed = hypot(*x, *y);
  if (speed <= limit)
    return 0;
  double scale = limit / speed;
  *x *= scale;
  *y *= scale;
  return 1;
}
static unsigned target(const FGField *f, double x, double y, double attraction,
                       double limit, double *vx, double *vy) {
  fg_field_velocity(f, f->u, f->v, x, y, vx, vy);
  unsigned count = bound(vx, vy, .5 * limit);
  double dx = .5 - x, dy = .5 - y;
  dx -= floor(dx + .5);
  dy -= floor(dy + .5);
  /* Smooth periodic shortest-displacement drift; a particle force, not a sink.
   */
  double ax = attraction * sin(TAU * dx) / TAU,
         ay = attraction * sin(TAU * dy) / TAU;
  count += bound(&ax, &ay, .5 * limit);
  *vx += ax;
  *vy += ay;
  return count;
}
void fg_particle_observe(const FGField *f, FGParticle *p) {
  FGFieldSample s = fg_field_sample(f, p->x, p->y);
  p->omega = s.omega;
  p->strain = s.strain;
  p->speed = hypot(s.u, s.v);
  p->motion_speed = hypot(p->vx,p->vy);
}
/* A damaged particle must not reach floor-to-index conversion in the field
 * sampler. Preserve its usable position, drop only its motion/history, and
 * let the owner re-anchor simulation time. No RNG or other particle changes. */
static unsigned recover(FGParticle *p) {
  if (isfinite(p->x) && isfinite(p->y) && isfinite(p->path_x) &&
      isfinite(p->path_y) && isfinite(p->vx) && isfinite(p->vy) &&
      isfinite(p->time))
    return 0;
  p->x = isfinite(p->x) ? p->x - floor(p->x) : .5;
  p->y = isfinite(p->y) ? p->y - floor(p->y) : .5;
  p->path_x = p->x;
  p->path_y = p->y;
  p->vx = p->vy = 0;
  if (!isfinite(p->time))
    p->time = 0;
  return 1;
}
unsigned fg_particle_advance(const FGField *f, FGParticle *p, double dt,
                             double inertia_ms, double attraction) {
  unsigned count = recover(p);
  if (!isfinite(dt) || dt <= 0) {
    fg_particle_observe(f, p);
    return count + (unsigned)!isfinite(dt);
  }
  double limit = 3 / (f->n * dt);
  count += bound(&p->vx, &p->vy, limit);
  double speed = fmax(hypot(p->vx, p->vy),
                      fmin(f->max_speed, .5 * limit) +
                          fmin(attraction * sqrt(2) / TAU, .5 * limit));
  unsigned steps = (unsigned)fmax(1, fmin(4, ceil(speed * dt * f->n / .75)));
  double step = dt / steps, relaxation = inertia_ms * .001;
  for (unsigned i = 0; i < steps; ++i) {
    double ux, uy, mx, my;
    count += target(f, p->x, p->y, attraction, limit, &ux, &uy);
    double half_drag = relaxation > 0 ? -expm1(-.5 * step / relaxation) : 1;
    double full_drag = relaxation > 0 ? -expm1(-step / relaxation) : 1;
    double hx = .5 * step * ux + (p->vx - ux) * relaxation * half_drag;
    double hy = .5 * step * uy + (p->vy - uy) * relaxation * half_drag;
    count += target(f, p->x + hx, p->y + hy, attraction, limit, &mx, &my);
    double dx = step * mx + (p->vx - mx) * relaxation * full_drag;
    double dy = step * my + (p->vy - my) * relaxation * full_drag;
    /* Protect the travel bound even when a stale field or flow-speed change
     * exhausts the predicted subdivision budget. */
    count += bound(&dx, &dy, .75 / f->n);
    p->path_x += dx;
    p->path_y += dy;
    p->x += dx;
    p->x -= floor(p->x);
    p->y += dy;
    p->y -= floor(p->y);
    p->vx += full_drag * (mx - p->vx);
    p->vy += full_drag * (my - p->vy);
  }
  fg_particle_observe(f, p);
  return count;
}
