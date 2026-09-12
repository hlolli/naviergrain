# Engine notes

The C11 engine owns its working memory in one caller-supplied arena. Csound
provides that storage through AUXCH. The core does no allocation, file I/O or
locking during rendering.

## Fluid and particles

`src/fluidgrain_field.c` uses a periodic MAC grid with staggered velocity faces
and cell-centered pressure. Supported grids are 16, 32 and 64 cells per side.
Each tick advects velocity with RK2 backtracing, applies forcing and implicit
viscosity diffusion, then projects the field with a fixed number of weighted
Jacobi passes. Finite iteration counts leave some divergence.

Paired vortices and seeded Fourier modes supply forcing. The fluid and particle
clocks follow rendered sample time, independently of the host's block size.
Freeze holds those clocks without a catch-up step on resume.

`src/fluidgrain_particles.c` moves persistent emitters and a separate particle
for each sounding grain. Passive particles use RK2. Inertia adds exponential
relaxation toward the field velocity. Attraction adds a periodic drift toward
the center to the particles, leaving the fluid field unchanged.

`src/fluidgrain_core.c` schedules grains, applies their envelopes and mixes audio.
The cosine path lives in `src/fluidgrain_oscillator.h`. Sample-table playback
uses `src/fluidgrain_resampler.c`. Fixed pools bound the work, with counters for
skipped births and numerical corrections.

## Hosts and GPU work

`src/fluidgrain_live.c` provides the shared interface for `desktop/` and `ui/`.
The desktop host uses webview for the canvas and miniaudio for output. In the
browser, a WASM worker feeds an AudioWorklet. The snapshot includes active grain
positions, frequencies and pan values.

The spiral maps the periodic 2D field into 3D display coordinates. At a periodic
boundary, a grain reappears at the opposite edge. Position smoothing and trails
stop at that crossing so they cannot draw a false path through the interior.

GPU grain rendering consumes packed plans made by the C scheduler. Metal and
CUDA run on a synthesis worker. If a GPU batch fails, its retained C plan
renders the replacement before the worker switches to CPU for the rest of
the take. The audio callback does not wait for GPU completion.

The older `web/` workbench also has a WGSL fluid solver and external CPU-field
workers. Field transport uses versioned little-endian packets and a four-slot
queue. See `src/fluidgrain_transport.h` and `tests/test_packet_wire.py` for the
wire contract. The offline view layout is defined by `fg_view` in
`src/fluidgrain_core.c` and decoded in `web/visualizer.ts`.

## Further work

The desktop build needs Linux and Windows testing, followed by packaging and
signing. More live voices will need profiling of scheduling and plan packing.
A full 3D solver would be a separate change to the model. The current view
cannot describe 3D vortex stretching.
