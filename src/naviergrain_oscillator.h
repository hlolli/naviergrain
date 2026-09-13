#ifndef NAVIERGRAIN_OSCILLATOR_H
#define NAVIERGRAIN_OSCILLATOR_H
#include <math.h>
/* Small-range polynomials for the analytic carrier path. Keep the general
 * sample reader unchanged. Double precision, without fast-math. */
static inline double ng_osc_sin(double x) {
  const double pi=3.14159265358979323846;
  x-=floor(x/(2*pi)+.5)*(2*pi);
  if(x>pi*.5)x=pi-x;
  if(x< -pi*.5)x= -pi-x;
  double z=x*x;
  return x*(1+z*(-1.0/6+z*(1.0/120+z*(-1.0/5040+z*(1.0/362880+
    z*(-1.0/39916800+z*(1.0/6227020800+z*(-1.0/1307674368000+
    z*(1.0/355687428096000+z*(-1.0/121645100408832000+z/51090942171709440000.0))))))))));
}
static inline double ng_osc_cos(double x) {return ng_osc_sin(x+1.57079632679489661923);}
/* Validated log2 ratios/frequencies in [-16,16]. Exact power-of-two scaling
 * also covers the live sound space's 55 Hz–12 kHz frequency interval. */
static inline double ng_osc_pitch(double x) {
  double whole=floor(x),r=(x-whole)*.69314718055994530942;
  int exponent=(int)whole;
  double scale=exponent<0?1.0/(double)(1u<<(unsigned)-exponent):(double)(1u<<(unsigned)exponent);
  return scale*(1+r*(1+r*(.5+r*(1.0/6+r*(1.0/24+r*(1.0/120+
    r*(1.0/720+r*(1.0/5040+r*(1.0/40320+r*(1.0/362880+
    r*(1.0/3628800+r*(1.0/39916800+r/479001600))))))))))));
}
#endif
