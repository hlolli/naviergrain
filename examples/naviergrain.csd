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
; GEN09: one cosine cycle, unit amplitude (90-degree sine phase).
iSource ftgen 0, 0, -240, 9, 1, 1, 90
iConfig[] fillarray $NG_CONFIG_DEFAULTS
iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
iConfig[$NG_CONFIG_MAX_GRAINS] = 4096
kControl[] fillarray $NG_CONTROL_DEFAULTS
kControl[$NG_CONTROL_GAIN] = .08
kControl[$NG_CONTROL_GRAIN_RATE] = 4000
; Source rate / cycle length = 52800 / 240 = 220 Hz.
aL, aR, kStats[] naviergrain iSource, 52800, iConfig, kControl
aFade linseg 0, .03, 1, p3-.13, 1, .1, 0
outs tanh(aL)*aFade, tanh(aR)*aFade
endin
</CsInstruments>
<CsScore>
i 1 0 2
e
</CsScore>
</CsoundSynthesizer>
