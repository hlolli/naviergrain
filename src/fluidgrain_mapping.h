#ifndef FLUIDGRAIN_MAPPING_H
#define FLUIDGRAIN_MAPPING_H
#include <math.h>
#define FG_SOUND_HEIGHT 3.6
/* One world-space mapping, evaluated by the render owner at particle ticks.
 * The renderer consumes these positions; it does not rebuild a second spiral. */
static inline void fg_sound_position(double x,double y,double speed,double out[3]) {
  const double pi=3.14159265358979323846;
  double activity=speed/(1+speed),turns=.45+3.2*activity;
  double angle=2*pi*y*turns+(x-.5)*1.1;
  double radius=(.28+.72*activity)*(.75+.25*sin(pi*y));
  out[0]=cos(angle)*radius;out[1]=(y-.5)*FG_SOUND_HEIGHT;out[2]=sin(angle)*radius;
}
/* Shared live cosine mapping. Height is the particle's periodic y coordinate;
 * speed is distance per wall-clock second, including the flow-speed control. */
static inline double fg_cosine_bend(double height, double speed) {
  double activity=speed/(1+speed);
  return .75*(2*height-1)+.25*(2*activity-1);
}
static inline double fg_cosine_duration(double milliseconds,double mix,
                                       double depth,double speed) {
  return milliseconds/(1+3*mix*depth*speed/(1+speed));
}
#endif
