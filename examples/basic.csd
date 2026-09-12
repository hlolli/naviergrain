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
iConfig[] fillarray $FG_CONFIG_DEFAULTS
kControl[] fillarray $FG_CONTROL_DEFAULTS
kControl[$FG_CONTROL_GAIN] = .12
kControl[$FG_CONTROL_POSITION_CENTER] linseg .1, p3*.5, .8, p3*.5, .3
kControl[$FG_CONTROL_GRAIN_RATE] expseg 18, p3*.5, 170, p3*.5, 35
kControl[$FG_CONTROL_GRAIN_MS] linseg 180, p3, 35
aL, aR, kStats[] naviergrain iSource, sr, iConfig, kControl
; Explicit host soft limiter plus entrance/exit fade; diagnostics remain pre-limiter.
aFade linseg 0, .03, 1, p3-.13, 1, .1, 0
outs tanh(aL)*aFade, tanh(aR)*aFade
printks "preview: grains=%d pre-limiter peak=%.4f\n", 1, kStats[$FG_STAT_LIVE_GRAINS], kStats[$FG_STAT_PRE_LIMITER_PEAK]
endin
</CsInstruments>
<CsScore>
i 1 0 8
e
</CsScore>
</CsoundSynthesizer>
