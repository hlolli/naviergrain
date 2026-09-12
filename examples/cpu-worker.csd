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
#include "include/fluidgrain-prepared.inc"
; Requires the project-owned native API host in cpu-worker.c.
instr 1
  iSource ftgen 0, 0, -997, 10, 1, .2, .1
  iConfig[] fillarray $FG_CONFIG_DEFAULTS
  iConfig[$FG_CONFIG_GRID_SIZE] = 16
  iConfig[$FG_CONFIG_EMITTER_COUNT] = 128
  iConfig[$FG_CONFIG_MAX_GRAINS] = 128
  iConfig[$FG_CONFIG_SOURCE_LOOP] = 1
  iConfig[$FG_CONFIG_BACKEND] = 1
  iConfig[$FG_CONFIG_INSTANCE_ID] = 1
  kControl[] fillarray $FG_CONTROL_DEFAULTS
  kControl[$FG_CONTROL_GAIN] init .03
  kControl[$FG_CONTROL_GRAIN_RATE] init 180
  kControl[$FG_CONTROL_GRAIN_MS] init 120
  kControl[$FG_CONTROL_PITCH_DEPTH] init 12
  kControl[$FG_CONTROL_DRIVE] chnget "fg.drive"
  kControl[$FG_CONTROL_FREEZE] chnget "fg.freeze"
  kControl[$FG_CONTROL_RESET] chnget "fg.reset"
  kRun chnget "fg.run"
  aL, aR, kStats[] FluidGrainPrepared iSource, sr, iConfig, kControl, kRun
  chnset kStats[$FG_STAT_STATUS], "fg.status"
  chnset kStats[$FG_STAT_BACKEND], "fg.backend"
  chnset kStats[$FG_STAT_EPOCH], "fg.epoch"
  chnset kStats[$FG_STAT_SNAPSHOT_SEQUENCE], "fg.sequence"
  chnset kStats[$FG_STAT_FIELD_AGE_MS], "fg.age"
  outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 0 -1
f 0 z
</CsScore>
</CsoundSynthesizer>
