; Shared source, no effects. p4 selects the scene; p5 is mapping mix (0 = baseline).
; Input density is grains/second, NOT particle/emitter count or fluid density.
iConfig[$NG_CONFIG_MAX_GRAINS] = 2048
iConfig[$NG_CONFIG_EMITTER_COUNT] = 1024
iConfig[$NG_CONFIG_PRESSURE_ITERATIONS] = 128
iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
iConfig[$NG_CONFIG_SEED] = 27180 + p4
kControl[$NG_CONTROL_GAIN] init .8
kControl[$NG_CONTROL_MAPPING_MIX] init p5
kControl[$NG_CONTROL_SPEED_TO_DENSITY] init .8
kControl[$NG_CONTROL_PITCH_DEPTH] init 16
kControl[$NG_CONTROL_POSITION_CENTER] init .5
kControl[$NG_CONTROL_POSITION_SPAN] init .85
if p4 == 1 then
  ; Paired Vortices / Vortex bloom: a few sparks thicken into a revolving choir.
  kControl[$NG_CONTROL_GRAIN_RATE] expseg 110, p3*.6, 1300, p3*.4, 650
  kControl[$NG_CONTROL_GRAIN_MS] linseg 80, p3, 400
  kControl[$NG_CONTROL_PITCH_RATIO] init .6
  kControl[$NG_CONTROL_DRIVE] init .55
  kControl[$NG_CONTROL_SWIRL] init .85
  kControl[$NG_CONTROL_TURBULENCE] init .03
  kControl[$NG_CONTROL_VISCOSITY] init .00035
  kControl[$NG_CONTROL_EDDY_SIZE] init .24
elseif p4 == 2 then
  ; Viscous Drift / Honey drift: a thick low cloud pulled into soft ribbons.
  kControl[$NG_CONTROL_GRAIN_RATE] init 1500
  kControl[$NG_CONTROL_GRAIN_MS] init 440
  kControl[$NG_CONTROL_PITCH_RATIO] init .33
  kControl[$NG_CONTROL_DRIVE] init .4
  kControl[$NG_CONTROL_SWIRL] init -.6
  kControl[$NG_CONTROL_TURBULENCE] init .08
  kControl[$NG_CONTROL_VISCOSITY] init .007
  kControl[$NG_CONTROL_INERTIA_MS] init 180
  kControl[$NG_CONTROL_PITCH_DEPTH] init 9
  kControl[$NG_CONTROL_EDDY_SIZE] init .32
elseif p4 == 3 then
  ; Eddy Spray / Ion blizzard: short, bright grains gather into a granular storm.
  kControl[$NG_CONTROL_GRAIN_RATE] expseg 350, p3*.4, 2000, p3*.6, 1100
  kControl[$NG_CONTROL_GRAIN_MS] linseg 22, p3, 120
  kControl[$NG_CONTROL_PITCH_RATIO] init 1.15
  kControl[$NG_CONTROL_DRIVE] init .5
  kControl[$NG_CONTROL_SWIRL] init .15
  kControl[$NG_CONTROL_TURBULENCE] init .8
  kControl[$NG_CONTROL_STRAIN_DRIVE] init .15
  kControl[$NG_CONTROL_VISCOSITY] init .0001
  kControl[$NG_CONTROL_EDDY_SIZE] init .08
  kControl[$NG_CONTROL_STRAIN_TO_DURATION] init 1
else
  ; Inward Spiral / Gravity silk: particle-only attraction gathers a winding cloud.
  kControl[$NG_CONTROL_GRAIN_RATE] init 1800
  kControl[$NG_CONTROL_GRAIN_MS] init 480
  kControl[$NG_CONTROL_PITCH_RATIO] init .48
  kControl[$NG_CONTROL_DRIVE] init .45
  kControl[$NG_CONTROL_SWIRL] init .75
  kControl[$NG_CONTROL_TURBULENCE] init .1
  kControl[$NG_CONTROL_VISCOSITY] init .001
  kControl[$NG_CONTROL_INERTIA_MS] init 220
  kControl[$NG_CONTROL_ATTRACTION] linseg 0, p3*.3, 0, p3*.7, 1
  kControl[$NG_CONTROL_EDDY_SIZE] init .23
endif
