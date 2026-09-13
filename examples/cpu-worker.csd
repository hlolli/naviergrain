<CsoundSynthesizer>
<CsOptions>
-n -d -m0
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 2
0dbfs = 1
#include "include/naviergrain.inc"
#include "include/naviergrain-prepared.inc"
; Requires the project-owned native API host in cpu-worker.c.
instr 1
  iSource ftgen 0, 0, -997, 10, 1, .2, .1
  iConfig[] fillarray $NG_CONFIG_DEFAULTS
  iConfig[$NG_CONFIG_GRID_SIZE] = 16
  iConfig[$NG_CONFIG_EMITTER_COUNT] = 128
  iConfig[$NG_CONFIG_MAX_GRAINS] = 128
  iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
  iConfig[$NG_CONFIG_BACKEND] = 1
  iConfig[$NG_CONFIG_INSTANCE_ID] = 1
  kControl[] fillarray $NG_CONTROL_DEFAULTS
  kControl[$NG_CONTROL_GAIN] init .03
  kControl[$NG_CONTROL_GRAIN_RATE] init 180
  kControl[$NG_CONTROL_GRAIN_MS] init 120
  kControl[$NG_CONTROL_PITCH_DEPTH] init 12
  kControl[$NG_CONTROL_DRIVE] chnget "ng.drive"
  kControl[$NG_CONTROL_FREEZE] chnget "ng.freeze"
  kControl[$NG_CONTROL_RESET] chnget "ng.reset"
  kRun chnget "ng.run"
  aL, aR, kStats[] NaviergrainPrepared iSource, sr, iConfig, kControl, kRun
  chnset kStats[$NG_STAT_STATUS], "ng.status"
  chnset kStats[$NG_STAT_BACKEND], "ng.backend"
  chnset kStats[$NG_STAT_EPOCH], "ng.epoch"
  chnset kStats[$NG_STAT_SNAPSHOT_SEQUENCE], "ng.sequence"
  chnset kStats[$NG_STAT_FIELD_AGE_MS], "ng.age"
  outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 0 -1
f 0 z
</CsScore>
</CsoundSynthesizer>
