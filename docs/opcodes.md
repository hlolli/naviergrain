# Csound opcodes

```csound
aL, aR, kStats[] naviergrain iSourceTable, iSourceSampleRate, iConfig[], kControl[]
```

Include `include/naviergrain.inc` and initialize the arrays with
`$NG_CONFIG_DEFAULTS` and `$NG_CONTROL_DEFAULTS`. Use the named indices to
change values. [examples/naviergrain.csd](../examples/naviergrain.csd) renders
a looping cosine table. Other mono tables work too.

`iConfig` has 13 entries, `kControl` has 24, and `kStats` has 20. The
[schema](../schema/naviergrain-v1.json) lists their order, defaults and bounds.
Configuration fixes the grid, emitter count and voice capacity at initialization.
Continuous controls smooth during playback. For a k-array value needed at
initialization, use Csound's `init` assignment.

The opcode copies its source table at initialization, excluding the guard point.
It rejects non-finite samples and multichannel tables. Playback uses a
Blackman-windowed sinc reader with ratio-dependent cutoff bands. Each grain
owns a particle, so its pitch and pan can keep changing after birth.

These sample-based controls use pitch ratios and source positions. The live
app instead enables the engine's fixed spatial frequency mapping. Its controls
are defined in `src/naviergrain_live.h` and `ui/live-controls.js`.

Rate zero stops births. Freeze holds the fluid and particles while existing
grains keep playing. Reset fades out, clears the simulation in bounded steps,
reseeds and fades back in. Stats include grain drops, field divergence and
numerical corrections. The core has overlap compensation but no peak limiter.

## Workers and preparation

`naviergrain` renders on the CPU. `naviergrain_buffered` uses a host-prepared
worker with Metal, CUDA or CPU rendering. The synchronous `naviergrain_gpu`
opcode is CUDA-only and intended for offline use.

For host integration, see [examples/buffered-device.c](../examples/buffered-device.c)
and [examples/prepared.csd](../examples/prepared.csd). Prepare sources and arenas
before starting audio callbacks. Csound can initialize instruments inside
`csoundPerformKsmps`, so calling `csoundStart` alone does not complete preparation.
Check readiness and source length after a silent pre-roll block.

Stop callbacks before resetting, recompiling or destroying a host. Stop and
join external workers before freeing their storage. The prepared wrapper's
run gate pauses engine time as well as sound, and its example stays silent
until the host sets its channels.
