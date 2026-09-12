/* Test-only, single-threaded probe. Never linked into the production module.
 * --wrap observes direct references in our static core, not host/libc internals.
 */
#define _POSIX_C_SOURCE 200809L
#include "fluidgrain_core.h"
#include "fluidgrain_field.h"
#include <stdlib.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))
static int active;
static double steps, step_ns, maximum_ns, allocations, clock_errors;
static size_t arena_bytes;

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
size_t __real_fg_memory_size(const FGConfig *, size_t, double, double);
int __real_fg_field_step(FGField *, const double *, double, double, unsigned, unsigned);

void *__wrap_malloc(size_t n) {
  allocations += active != 0;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t size) {
  allocations += active != 0;
  return __real_calloc(n, size);
}
void *__wrap_realloc(void *p, size_t n) {
  allocations += active != 0;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  allocations += active != 0;
  __real_free(p);
}
size_t __wrap_fg_memory_size(const FGConfig *c, size_t length, double source_sr,
                            double engine_sr) {
  arena_bytes = __real_fg_memory_size(c, length, source_sr, engine_sr);
  return arena_bytes;
}
int __wrap_fg_field_step(FGField *f, const double *c, double fluid_hz,
                        double particle_hz, unsigned pressure, unsigned viscosity) {
  if (!active)
    return __real_fg_field_step(f, c, fluid_hz, particle_hz, pressure, viscosity);
  struct timespec begin, end;
  int start_ok = clock_gettime(CLOCK_MONOTONIC, &begin) == 0;
  int result = __real_fg_field_step(f, c, fluid_hz, particle_hz, pressure, viscosity);
  int end_ok = clock_gettime(CLOCK_MONOTONIC, &end) == 0;
  steps += 1;
  if (!start_ok || !end_ok) {
    clock_errors += 1;
    return result;
  }
  double elapsed = (double)(end.tv_sec - begin.tv_sec) * 1e9 +
                   (double)(end.tv_nsec - begin.tv_nsec);
  step_ns += elapsed;
  if (elapsed > maximum_ns)
    maximum_ns = elapsed;
  return result;
}
/* Called outside csoundPerformKsmps; one host, one calling thread only.
 * Counters are per callback; arena size is the latest prepared engine size. */
EXPORT void fg_profile_begin(void) {
  steps = step_ns = maximum_ns = allocations = clock_errors = 0;
  active = 1;
}
EXPORT void fg_profile_end(double values[6]) {
  active = 0;
  values[0] = steps;
  values[1] = step_ns;
  values[2] = maximum_ns;
  values[3] = allocations;
  values[4] = (double)arena_bytes;
  values[5] = clock_errors;
}
