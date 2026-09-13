/* Independent numerical fixture source: link the unmodified C solver, not a
 * JavaScript transcription of it. stdout is JSON, diagnostics go to stderr. */
#include "naviergrain_core.h"
#include "naviergrain_field.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TAU 6.28318530717958647692
static void array(const double *a, size_t n) {
  putchar('[');
  for(size_t k=0;k<n;k++) printf("%s%.17g",k?",":"",a[k]);
  putchar(']');
}
int main(void) {
  puts("[");
  unsigned record=0;
  for(unsigned n=16;n<=64;n*=2) for(unsigned mode=0;mode<11;mode++) {
    const char *names[]={"zero","constant","diffusion","normal","swirl","turbulence",
      "strain","confinement","extreme","odd-passes","velocity-cap"};
    double c[NG_CONTROL_COUNT], config[NG_CONFIG_COUNT];
    ng_defaults(config,c);
    double *storage=calloc(ng_field_doubles(n),sizeof(double));
    if(!storage)return 1;
    NGField f; ng_field_init(&f,n,storage,mode==9?0:12345);
    size_t count=(size_t)n*n;
    unsigned steps=mode<3?1:12, pressure=mode==9?65:64, diffusion=mode==9?15:16;
    if(mode<3 || mode==10)c[NG_CONTROL_DRIVE]=0;
    if(mode==10) {
      steps=1;
      for(size_t k=0;k<count;k++){f.u[k]=100;f.v[k]=-50;}
    }
    if(mode==1)for(size_t k=0;k<count;k++){f.u[k]=.25;f.v[k]=-.125;}
    if(mode==2) {
      c[NG_CONTROL_VISCOSITY]=.002;
      for(unsigned j=0;j<n;j++)for(unsigned i=0;i<n;i++)
        f.u[j*n+i]=cos(TAU*(j+.5)/n);
    }
    if(mode>=4 && mode<=6) {
      c[NG_CONTROL_SWIRL]=mode==4?-.8:0;
      c[NG_CONTROL_TURBULENCE]=mode==5?1:0;
      c[NG_CONTROL_STRAIN_DRIVE]=mode==6?-.7:0;
      c[NG_CONTROL_VISCOSITY]=0;
    }
    if(mode==7)c[NG_CONTROL_CONFINEMENT]=.7;
    if(mode==8) {
      c[NG_CONTROL_VISCOSITY]=.01;c[NG_CONTROL_DRIVE]=2;
      c[NG_CONTROL_SWIRL]=-1;c[NG_CONTROL_TURBULENCE]=1;
      c[NG_CONTROL_STRAIN_DRIVE]=-1;c[NG_CONTROL_CONFINEMENT]=1;
      c[NG_CONTROL_FLOW_SPEED]=4;c[NG_CONTROL_EDDY_SIZE]=.05;
    }
    printf("%s{\"name\":\"%s\",\"grid\":%u,\"seed\":%u,\"steps\":%u,"
      "\"pressure\":%u,\"diffusion\":%u,\"controls\":",record++?",":"",names[mode],n,
      mode==9?0:12345,steps,pressure,diffusion);
    array(c,NG_CONTROL_COUNT);
    printf(",\"initial\":");array(f.u,2*count);
    for(unsigned step=0;step<steps;step++)
      if(!ng_field_step(&f,c,60,240,pressure,diffusion)){free(storage);return 2;}
    printf(",\"planes\":");array(f.u,4*count);
    double diagnostics[]={f.max_speed, f.energy,f.rms_omega,f.rms_strain,
      f.rms_divergence,f.max_divergence};
    printf(",\"diagnostics\":");array(diagnostics,6);
    printf(",\"interventions\":%llu,\"time\":%.17g}",(unsigned long long)f.interventions,f.time);
    free(storage);
  }
  puts("]");
  return 0;
}
