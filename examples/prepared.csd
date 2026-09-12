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

; API-host example: deliberately silent until the host writes fg.run = 1.
; Keep this one instrument alive. Do not reschedule it to play each note.
instr 1
  iSource ftgen 0, 0, -997, 10, 1, .2, .1
  iConfig[] fillarray $FG_CONFIG_DEFAULTS
  iConfig[$FG_CONFIG_GRID_SIZE] = 16
  iConfig[$FG_CONFIG_EMITTER_COUNT] = 128
  iConfig[$FG_CONFIG_MAX_GRAINS] = 128
  iConfig[$FG_CONFIG_SOURCE_LOOP] = 1
  kControl[] fillarray $FG_CONTROL_DEFAULTS
  kControl[$FG_CONTROL_GAIN] init .03
  kControl[$FG_CONTROL_GRAIN_RATE] init 80
  kControl[$FG_CONTROL_GRAIN_MS] init 60
  kRun chnget "fg.run"
  kControl[$FG_CONTROL_GRAIN_RATE] chnget "fg.rate"
  kControl[$FG_CONTROL_GAIN] chnget "fg.gain"
  kControl[$FG_CONTROL_PITCH_RATIO] chnget "fg.pitch"
  kControl[$FG_CONTROL_FREEZE] chnget "fg.freeze"
  kControl[$FG_CONTROL_RESET] chnget "fg.reset"
  aL, aR, kStats[] FluidGrainPrepared iSource, sr, iConfig, kControl, kRun
  chnset kStats[$FG_STAT_STATUS], "fg.status"
  chnset kStats[$FG_STAT_LIVE_GRAINS], "fg.live"
  chnset kStats[$FG_STAT_SNAPSHOT_SEQUENCE], "fg.sequence"
  chnset kStats[$FG_STAT_FIELD_AGE_MS], "fg.age"
  chnset kStats[$FG_STAT_EPOCH], "fg.epoch"
  chnset kStats[$FG_STAT_SOURCE_LENGTH], "fg.source_length"
  chnset kStats[$FG_STAT_NUMERIC_INTERVENTIONS], "fg.numeric"
  chnset kStats[$FG_STAT_VOICE_DROPS], "fg.drops"
  outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 0 -1
f 0 z
</CsScore>
</CsoundSynthesizer>
