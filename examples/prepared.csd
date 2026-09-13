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

; API-host example: deliberately silent until the host writes ng.run = 1.
; Keep this one instrument alive. Do not reschedule it to play each note.
instr 1
  iSource ftgen 0, 0, -997, 10, 1, .2, .1
  iConfig[] fillarray $NG_CONFIG_DEFAULTS
  iConfig[$NG_CONFIG_GRID_SIZE] = 16
  iConfig[$NG_CONFIG_EMITTER_COUNT] = 128
  iConfig[$NG_CONFIG_MAX_GRAINS] = 128
  iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
  kControl[] fillarray $NG_CONTROL_DEFAULTS
  kControl[$NG_CONTROL_GAIN] init .03
  kControl[$NG_CONTROL_GRAIN_RATE] init 80
  kControl[$NG_CONTROL_GRAIN_MS] init 60
  kRun chnget "ng.run"
  kControl[$NG_CONTROL_GRAIN_RATE] chnget "ng.rate"
  kControl[$NG_CONTROL_GAIN] chnget "ng.gain"
  kControl[$NG_CONTROL_PITCH_RATIO] chnget "ng.pitch"
  kControl[$NG_CONTROL_FREEZE] chnget "ng.freeze"
  kControl[$NG_CONTROL_RESET] chnget "ng.reset"
  aL, aR, kStats[] NaviergrainPrepared iSource, sr, iConfig, kControl, kRun
  chnset kStats[$NG_STAT_STATUS], "ng.status"
  chnset kStats[$NG_STAT_LIVE_GRAINS], "ng.live"
  chnset kStats[$NG_STAT_SNAPSHOT_SEQUENCE], "ng.sequence"
  chnset kStats[$NG_STAT_FIELD_AGE_MS], "ng.age"
  chnset kStats[$NG_STAT_EPOCH], "ng.epoch"
  chnset kStats[$NG_STAT_SOURCE_LENGTH], "ng.source_length"
  chnset kStats[$NG_STAT_NUMERIC_INTERVENTIONS], "ng.numeric"
  chnset kStats[$NG_STAT_VOICE_DROPS], "ng.drops"
  outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 0 -1
f 0 z
</CsScore>
</CsoundSynthesizer>
