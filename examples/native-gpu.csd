<CsoundSynthesizer>
<CsOptions>
; Offline CUDA grain renderer. Load build-native-cuda/libnaviergrain.so.
; CPU is used automatically if CUDA is unavailable.
-d -m0 -W -f -o native-gpu.wav
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 512
nchnls = 2
0dbfs = 1
#include "../include/naviergrain.inc"
giSource ftgen 1, 0, -48000, 10, 1, .3, .1, .05
instr 1
iConfig[] fillarray $NG_CONFIG_DEFAULTS
iConfig[$NG_CONFIG_MAX_GRAINS] = 1024
iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
kControl[] fillarray $NG_CONTROL_DEFAULTS
kControl[$NG_CONTROL_GRAIN_RATE] init 1200
kControl[$NG_CONTROL_GRAIN_MS] init 180
kControl[$NG_CONTROL_GAIN] init .12
; kEnable=0 keeps this instance on C from that point forward.
kEnable init 1
aL, aR, kStats[], kGPU naviergrain_gpu giSource, sr, iConfig, kControl, 0, kEnable
outs aL, aR
printks "CUDA active: %d\n", 1, kGPU
endin
</CsInstruments>
<CsScore>
i 1 0 4
e
</CsScore>
</CsoundSynthesizer>
