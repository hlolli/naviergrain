<CsoundSynthesizer>
<CsOptions>
-d -m0
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 32
nchnls = 2
0dbfs = 1
#include "../include/naviergrain.inc"
instr 1
#include "source.inc"
iConfig[] fillarray $NG_CONFIG_DEFAULTS
iConfig[$NG_CONFIG_SEED] = 1907 + p4
kControl[] fillarray $NG_CONTROL_DEFAULTS
; Preserve the original static-field preview sketches.
kControl[$NG_CONTROL_FLOW_SPEED] init 0
kControl[$NG_CONTROL_GAIN] = .1
if p4 == 1 then
  ; Glass pollen: spacious flecks of inharmonic bell, closing into a cloud.
  kControl[$NG_CONTROL_GRAIN_RATE] expseg 9, p3, 170
  kControl[$NG_CONTROL_GRAIN_MS] linseg 180, p3, 32
  kControl[$NG_CONTROL_PITCH_RATIO] expseg 1.8, p3, .6
  kControl[$NG_CONTROL_POSITION_CENTER] linseg .1, p3, .55
  kControl[$NG_CONTROL_POSITION_SPAN] = .35
elseif p4 == 2 then
  ; Magnetic rain: periodic droplets accelerate into a pitched beating texture.
  kControl[$NG_CONTROL_SCHEDULER] = 0
  kControl[$NG_CONTROL_GRAIN_RATE] expseg 12, p3*.6, 340, p3*.4, 70
  kControl[$NG_CONTROL_GRAIN_MS] = 75
  kControl[$NG_CONTROL_PITCH_RATIO] = .75
  kControl[$NG_CONTROL_POSITION_CENTER] linseg .25, p3, .8
  kControl[$NG_CONTROL_POSITION_SPAN] linseg .03, p3, .7
else
  ; Velvet fracture: stretch the buzz, then let the grains pull apart.
  kControl[$NG_CONTROL_GRAIN_RATE] expseg 220, p3, 6
  kControl[$NG_CONTROL_GRAIN_MS] linseg 380, p3, 24
  kControl[$NG_CONTROL_PITCH_RATIO] expseg .3, p3, 1.4
  kControl[$NG_CONTROL_POSITION_CENTER] = .72
  kControl[$NG_CONTROL_POSITION_SPAN] = .4
endif
aL, aR, kStats[] naviergrain iSource, sr, iConfig, kControl
aFade linseg 0, .03, 1, p3-.23, 1, .2, 0
outs tanh(aL)*aFade, tanh(aR)*aFade
endin
</CsInstruments>
<CsScore>
i 1 0 8 1
i 1 8.5 8 2
i 1 17 8 3
e
</CsScore>
</CsoundSynthesizer>
