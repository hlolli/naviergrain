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
#include "fluid-presets.inc"
aL, aR, kStats[] naviergrain iSource, sr, iConfig, kControl
; Un-limited opcode peak and numerical diagnostics, accumulated every block.
kLive init 0
kPeak init 0
kRelative init 0
kLive = max(kLive, kStats[$FG_STAT_LIVE_GRAINS])
kPeak = max(kPeak, kStats[$FG_STAT_PRE_LIMITER_PEAK])
kRelative = max(kRelative, kStats[$FG_STAT_RMS_DIVERGENCE]/(32*max(sqrt(2*kStats[$FG_STAT_KINETIC_ENERGY]), .001)))
kEnd release
if kEnd == 1 then
  printks "FG_CLOUD %d live=%.0f peak=%.8f divergence=%.8f numeric=%.0f drops=%.0f\n", 0, p4, kLive, kPeak, kRelative, kStats[$FG_STAT_NUMERIC_INTERVENTIONS], kStats[$FG_STAT_VOICE_DROPS]
endif
xtratim .001
; Only outer fades and an explicit host safety limiter; no reverb/delay layering.
aFade linseg 0, .05, 1, p3-.35, 1, .3, 0
outs tanh(aL)*aFade, tanh(aR)*aFade
endin
</CsInstruments>
<CsScore>
i 1 0 10 1 1
i 1 10.5 10 2 1
i 1 21 10 3 1
i 1 31.5 10 4 1
e
</CsScore>
</CsoundSynthesizer>
