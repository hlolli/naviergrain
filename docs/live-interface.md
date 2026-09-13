# Sound and graphics

Every visible particle is a sounding grain. Each plays a cosine carrier under
a Hann envelope, and a short trail follows that grain until it ends. Adding
grains makes the spiral clearer.

The fluid solver runs on a periodic 2D grid. The engine maps those particles
into 3D, using speed to shape the spiral and height to set frequency, then
sends the same positions to the renderer. This is an artistic mapping of the
field rather than a 3D fluid simulation.

At normalized height `h`, the carrier frequency is
`55 * (12000 / 55)^h` Hz. Lower audio sample rates reduce the upper endpoint
to stay below Nyquist. A wide height distribution produces a wide frequency
range. As particles gather at one height, their frequencies gather too.

World x sets equal-power stereo pan: `0.5 + 0.425 * world_x`. Positions smooth
over 10 ms before the engine derives frequency and pan. Camera rotation only
changes the view. Depth has no separate audio effect.

Faster particles get shorter grains, broadening their spectra. Grain starts
follow a Poisson process. The live app uses 512 emitters and 256 voice slots,
with a rate control up to 32,000 starts per second. A full pool skips new
starts and lets active grains finish. One voice is one grain.

The default starts 700 grains per second with a 180 ms lifetime, gentler
forcing and slower motion. Velocity can shorten that lifetime. This leaves
room in the voice pool and lets individual tones stand out.

Drag a control, use its arrow keys, or double-click to restore its default.
The labels show field and particle coefficients. Observation amplitude A
scales the output signal. It is separate from the fluid equation.
Phase dispersion changes how grains combine. The shared initial phase has
no slider: rotating every cosine together gives little audible change.

The waveform and spectrogram use rendered audio. The spectrogram has a
2,048-sample Hann window and 96 logarithmic bands. Its scroll rate follows
display updates, so it has no fixed seconds-per-pixel scale.

Both audio hosts queue up to 2,048 frames ahead, about 43 ms at 48 kHz, plus
device buffering. Snapshots can lead audible playback by that amount. An
underrun fades to silence and waits for the queue to refill.

Metal renders native grains when enabled. The live browser uses CPU synthesis
through WASM. At the current voice count, CPU rendering measured faster than
Metal on the M5 Max. Higher grain counts still need performance work.
