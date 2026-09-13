<CsoundSynthesizer>
<CsOptions>
-d -m0 --sample-accurate
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 32
nchnls = 2
0dbfs = 1
#include "../include/naviergrain.inc"
; Six ten-second scenes. Each starts with the same source, seed and controls,
; varying ONLY the named p4 control. Hold 2 s, sweep 6 s, hold 2 s.
; No limiter, reverb, EQ or nonlinear processing. Endpoint fades only.
instr 1
#include "source.inc"
iConfig[] fillarray $NG_CONFIG_DEFAULTS
iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
iConfig[$NG_CONFIG_PRESSURE_ITERATIONS] = 128
kControl[] fillarray $NG_CONTROL_DEFAULTS
kControl[$NG_CONTROL_GAIN] init .3
kControl[$NG_CONTROL_GRAIN_RATE] init 600
kControl[$NG_CONTROL_GRAIN_MS] init 150
kControl[$NG_CONTROL_PITCH_RATIO] init .7
kControl[$NG_CONTROL_PITCH_DEPTH] init 24
kControl[$NG_CONTROL_POSITION_SPAN] init 1
kControl[$NG_CONTROL_DRIVE] init .5
kControl[$NG_CONTROL_SWIRL] init .7
kControl[$NG_CONTROL_TURBULENCE] init .2
kControl[p4] init p5
kControl[p4] linseg p5, 2, p5, 6, p6, 2, p6
aL, aR, kStats[] naviergrain iSource, sr, iConfig, kControl
aEdge linseg 0, .02, 1, 9.96, 1, .02, 0
outs aL * aEdge, aR * aEdge
printks "NG_DIAGNOSTIC control=%d value=%.6f live=%d peak=%.6f numeric=%d drops=%d\n", 1, p4, kControl[p4], kStats[$NG_STAT_LIVE_GRAINS], kStats[$NG_STAT_PRE_LIMITER_PEAK], kStats[$NG_STAT_NUMERIC_INTERVENTIONS], kStats[$NG_STAT_VOICE_DROPS]
endin
</CsInstruments>
<CsScore>
#include "../include/naviergrain.inc"
; Time    Control                  From       To
i 1  0 10 $NG_CONTROL_VISCOSITY       0        .01
i 1 10 10 $NG_CONTROL_SWIRL          -1         1
i 1 20 10 $NG_CONTROL_TURBULENCE      0         1
i 1 30 10 $NG_CONTROL_STRAIN_DRIVE   -1         1
i 1 40 10 $NG_CONTROL_INERTIA_MS      0       300
i 1 50 10 $NG_CONTROL_ATTRACTION     0         1
e
</CsScore>
</CsoundSynthesizer>
